#include <errno.h>
#include <json.h>
#include <libwebsockets.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include "pty.h"
#include "server.h"
#include "utils.h"
#include "compat.h"

// initial message list
static char initial_cmds[] = {SESSION_STATUS, SET_WINDOW_TITLE, SET_PREFERENCES};

static int send_initial_message(struct lws *wsi, struct pss_tty *pss, int index) {
  unsigned char message[LWS_PRE + 1 + 4096];
  unsigned char *p = &message[LWS_PRE];
  char buffer[128];
  int n = 0;

  char cmd = initial_cmds[index];
  switch (cmd) {
    case SESSION_STATUS:
      n = sprintf((char *)p, "%c%s", cmd, pss->session_resumed ? "resumed" : "new");
      break;
    case SET_WINDOW_TITLE:
      gethostname(buffer, sizeof(buffer) - 1);
      n = snprintf((char *)p, 1 + 4096, "%c%s (%s)", cmd, server->command, buffer);
      break;
    case SET_PREFERENCES:
      n = snprintf((char *)p, 1 + 4096, "%c%s", cmd, server->prefs_json);
      break;
    default:
      break;
  }

  return lws_write(wsi, p, (size_t)n, LWS_WRITE_BINARY);
}

static json_object *parse_window_size(const char *buf, size_t len, uint16_t *cols, uint16_t *rows) {
  json_tokener *tok = json_tokener_new();
  json_object *obj = json_tokener_parse_ex(tok, buf, len);
  struct json_object *o = NULL;

  if (obj != NULL && json_object_is_type(obj, json_type_object) &&
      json_object_object_get_ex(obj, "columns", &o)) *cols = (uint16_t)json_object_get_int(o);
  if (obj != NULL && json_object_is_type(obj, json_type_object) &&
      json_object_object_get_ex(obj, "rows", &o)) *rows = (uint16_t)json_object_get_int(o);

  json_tokener_free(tok);
  return obj;
}

static bool check_host_origin(struct lws *wsi) {
  char buf[256];
  memset(buf, 0, sizeof(buf));
  int len = lws_hdr_copy(wsi, buf, (int)sizeof(buf), WSI_TOKEN_ORIGIN);
  if (len <= 0) return false;

  const char *prot, *address, *path;
  int port;
  if (lws_parse_uri(buf, &prot, &address, &port, &path)) return false;
  if (port == 80 || port == 443) {
    snprintf(buf, sizeof(buf), "%s", address);
  } else {
    snprintf(buf, sizeof(buf), "%s:%d", address, port);
  }

  char host_buf[256];
  memset(host_buf, 0, sizeof(host_buf));
  len = lws_hdr_copy(wsi, host_buf, (int)sizeof(host_buf), WSI_TOKEN_HOST);

  return len > 0 && strcasecmp(buf, host_buf) == 0;
}

typedef struct {
  struct tty_session *session;
} pty_ctx_t;

static pty_ctx_t *pty_ctx_init(struct tty_session *session) {
  pty_ctx_t *ctx = xmalloc(sizeof(*ctx));
  ctx->session = session;
  return ctx;
}

static void pty_ctx_free(pty_ctx_t *ctx) { free(ctx); }

static void session_timer_close_cb(uv_handle_t *handle) { free(handle); }

static void session_unlink(struct tty_session *session) {
  if (!session->listed) return;

  struct tty_session **current = &server->sessions;
  while (*current != NULL && *current != session) current = &(*current)->next;
  if (*current == session) *current = session->next;
  session->listed = false;
  session->next = NULL;
}

static void session_stop_timer(struct tty_session *session) {
  if (session->timer == NULL) return;
  uv_timer_stop(session->timer);
  uv_close((uv_handle_t *)session->timer, session_timer_close_cb);
  session->timer = NULL;
}

static void session_free(struct tty_session *session) {
  if (session == NULL) return;
  session_unlink(session);
  session_stop_timer(session);
  while (session->pty_buf != NULL) {
    pty_buf_t *buf = session->pty_buf;
    session->pty_buf = buf->next;
    pty_buf_free(buf);
  }
  session->pty_buf_tail = NULL;
  free(session);
}

static void session_maybe_free(struct tty_session *session) {
  if (session != NULL && session->pss == NULL && session->process == NULL) session_free(session);
}

static struct tty_session *session_find(const char *id) {
  for (struct tty_session *session = server->sessions; session != NULL; session = session->next) {
    if (!strcmp(session->id, id)) return session;
  }
  return NULL;
}

static int retained_session_count(void) {
  int count = 0;
  for (struct tty_session *session = server->sessions; session != NULL; session = session->next) {
    if (session->pss == NULL && session->process != NULL && !session->expired) count++;
  }
  return count;
}

static bool session_id_valid(const char *id) {
  if (id == NULL || id[0] == '\0' || strlen(id) >= sizeof(((struct tty_session *)0)->id)) return false;
  for (const char *p = id; *p != '\0'; p++) {
    if (!(('a' <= *p && *p <= 'z') || ('A' <= *p && *p <= 'Z') || ('0' <= *p && *p <= '9') || *p == '-' || *p == '_'))
      return false;
  }
  return true;
}

static struct tty_session *session_new(const char *id, const char *user) {
  struct tty_session *session = xmalloc(sizeof(*session));
  memset(session, 0, sizeof(*session));
  if (id != NULL) snprintf(session->id, sizeof(session->id), "%s", id);
  if (user != NULL) snprintf(session->user, sizeof(session->user), "%s", user);
  return session;
}

static void session_add(struct tty_session *session) {
  session->listed = true;
  session->next = server->sessions;
  server->sessions = session;
}

static void pause_process(pty_process *process) {
  if (process == NULL) return;
  // pty_pause() in older ttyd releases did not update paused consistently.
  // Stop the watcher explicitly so retention also works with those builds.
  if (process->out != NULL) uv_read_stop((uv_stream_t *)process->out);
  process->paused = true;
}

static void session_timeout_cb(uv_timer_t *timer);

static void session_start_timer(struct tty_session *session) {
  session_stop_timer(session);
  session->timer = xmalloc(sizeof(*session->timer));
  uv_timer_init(server->loop, session->timer);
  session->timer->data = session;
  uv_timer_start(session->timer, session_timeout_cb, (uint64_t)server->reconnect_timeout * 1000, 0);
}

static void session_timeout_cb(uv_timer_t *timer) {
  struct tty_session *session = (struct tty_session *)timer->data;
  session->timer = NULL;
  uv_close((uv_handle_t *)timer, session_timer_close_cb);
  session->expired = true;
  session_unlink(session);

  if (session->process != NULL) {
    pause_process(session->process);
    if (process_running(session->process)) {
      lwsl_notice("reconnect timeout expired, killing process, pid: %d\n", session->process->pid);
      pty_kill(session->process, server->sig_code);
    }
  }
  session_maybe_free(session);
}

static bool session_attach(struct pss_tty *pss, const char *id) {
  struct tty_session *session = NULL;
  bool resumable = server->reconnect_timeout > 0;

  if (resumable) {
    if (!session_id_valid(id)) {
      lwsl_warn("refuse WS client without a valid SessionId while reconnect sessions are enabled\n");
      return false;
    }
    session = session_find(id);
    if (session != NULL) {
      if (session->pss != NULL || session->process == NULL || session->expired || !process_running(session->process)) {
        lwsl_warn("refuse WS client for unavailable or already attached session: %.8s\n", id);
        return false;
      }
      if (strcmp(session->user, pss->user) != 0) {
        lwsl_warn("refuse WS client for session with a different authenticated user: %.8s\n", id);
        return false;
      }
      session_stop_timer(session);
      pss->session_resumed = true;
    } else {
      if (server->max_clients > 0 && server->client_count + retained_session_count() > server->max_clients) {
        lwsl_warn("refuse WS client because --max-clients includes retained sessions\n");
        return false;
      }
      session = session_new(id, pss->user);
      session_add(session);
    }
  } else {
    session = session_new(NULL, pss->user);
    session_add(session);
  }

  session->pss = pss;
  pss->session = session;
  return true;
}

static void session_queue_output(struct tty_session *session, pty_buf_t *buf) {
  if (buf == NULL) return;
  buf->next = NULL;
  if (session->pty_buf_tail != NULL)
    session->pty_buf_tail->next = buf;
  else
    session->pty_buf = buf;
  session->pty_buf_tail = buf;
}

void tty_sessions_shutdown(void) {
  struct tty_session *session = server->sessions;
  while (session != NULL) {
    struct tty_session *next = session->next;
    session->expired = true;
    session_unlink(session);
    session_stop_timer(session);
    if (session->process != NULL) {
      pause_process(session->process);
      if (process_running(session->process)) {
        pty_kill(session->process, server->sig_code);
#ifndef _WIN32
        if (process_running(session->process)) pty_kill(session->process, SIGKILL);
#endif
      }
    }
    session_maybe_free(session);
    session = next;
  }
}

static void process_read_cb(pty_process *process, pty_buf_t *buf, bool eof) {
  pty_ctx_t *ctx = (pty_ctx_t *)process->ctx;
  struct tty_session *session = ctx->session;
  struct pss_tty *pss = session->pss;

  if (eof && !process_running(process)) {
    if (pss != NULL) pss->lws_close_status = process->exit_code == 0 ? 1000 : 1006;
    pty_buf_free(buf);
  } else if (buf != NULL) {
    session_queue_output(session, buf);
  }

  if (pss != NULL) lws_callback_on_writable(pss->wsi);
}

static void process_exit_cb(pty_process *process) {
  pty_ctx_t *ctx = (pty_ctx_t *)process->ctx;
  struct tty_session *session = ctx->session;
  struct pss_tty *pss = session->pss;

  lwsl_notice("process exited with code %d, signal %d, pid: %d\n", process->exit_code, process->exit_signal, process->pid);
  session->process = NULL;
  session_stop_timer(session);

  if (pss != NULL) {
    pss->lws_close_status = process->exit_code == 0 ? 1000 : 1006;
    lws_callback_on_writable(pss->wsi);
  } else {
    session_unlink(session);
  }

  pty_ctx_free(ctx);
  session_maybe_free(session);

  // if we are going to exit, do it now.
  if (force_exit) exit(0);
}

static char **build_args(struct pss_tty *pss) {
  int i, n = 0;
  char **argv = xmalloc((server->argc + pss->argc + 1) * sizeof(char *));

  for (i = 0; i < server->argc; i++) {
    argv[n++] = server->argv[i];
  }

  for (i = 0; i < pss->argc; i++) {
    argv[n++] = pss->args[i];
  }

  argv[n] = NULL;

  return argv;
}

static char **build_env(struct pss_tty *pss) {
  int i = 0, n = 2;
  char **envp = xmalloc(n * sizeof(char *));

  // TERM
  envp[i] = xmalloc(36);
  snprintf(envp[i], 36, "TERM=%s", server->terminal_type);
  i++;

  // TTYD_USER
  if (strlen(pss->user) > 0) {
    envp = xrealloc(envp, (++n) * sizeof(char *));
    envp[i] = xmalloc(40);
    snprintf(envp[i], 40, "TTYD_USER=%s", pss->user);
    i++;
  }

  envp[i] = NULL;

  return envp;
}

static bool spawn_process(struct pss_tty *pss, uint16_t columns, uint16_t rows) {
  struct tty_session *session = pss->session;
  pty_ctx_t *ctx = pty_ctx_init(session);
  pty_process *process = process_init((void *)ctx, server->loop, build_args(pss), build_env(pss));
  if (server->cwd != NULL) process->cwd = strdup(server->cwd);
  if (columns > 0) process->columns = columns;
  if (rows > 0) process->rows = rows;
  if (pty_spawn(process, process_read_cb, process_exit_cb) != 0) {
    lwsl_err("pty_spawn: %d (%s)\n", errno, strerror(errno));
    pty_ctx_free(ctx);
    process_free(process);
    return false;
  }
  lwsl_notice("started process, pid: %d\n", process->pid);
  session->process = process;
  lws_callback_on_writable(pss->wsi);

  return true;
}

static void wsi_output(struct lws *wsi, pty_buf_t *buf) {
  if (buf == NULL) return;
  char *message = xmalloc(LWS_PRE + 1 + buf->len);
  char *ptr = message + LWS_PRE;

  *ptr = OUTPUT;
  memcpy(ptr + 1, buf->base, buf->len);
  size_t n = buf->len + 1;

  if (lws_write(wsi, (unsigned char *)ptr, n, LWS_WRITE_BINARY) < n) {
    lwsl_err("write OUTPUT to WS\n");
  }

  free(message);
}

static bool check_auth(struct lws *wsi, struct pss_tty *pss) {
  if (server->auth_header != NULL) {
    return lws_hdr_custom_copy(wsi, pss->user, sizeof(pss->user), server->auth_header, strlen(server->auth_header)) > 0;
  }

  if (server->credential != NULL) {
    char buf[256];
    size_t n = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_AUTHORIZATION);
    return n >= 7 && strstr(buf, "Basic ") && !strcmp(buf + 6, server->credential);
  }

  return true;
}

int callback_tty(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
  struct pss_tty *pss = (struct pss_tty *)user;
  char buf[256];
  size_t n = 0;

  switch (reason) {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
      if (server->once && server->client_count > 0) {
        lwsl_warn("refuse to serve WS client due to the --once option.\n");
        return 1;
      }
      if (server->max_clients > 0 && server->client_count == server->max_clients) {
        lwsl_warn("refuse to serve WS client due to the --max-clients option.\n");
        return 1;
      }
      if (!check_auth(wsi, pss)) return 1;

      n = lws_hdr_copy(wsi, pss->path, sizeof(pss->path), WSI_TOKEN_GET_URI);
#if defined(LWS_ROLE_H2)
      if (n <= 0) n = lws_hdr_copy(wsi, pss->path, sizeof(pss->path), WSI_TOKEN_HTTP_COLON_PATH);
#endif
      if (strncmp(pss->path, endpoints.ws, n) != 0) {
        lwsl_warn("refuse to serve WS client for illegal ws path: %s\n", pss->path);
        return 1;
      }

      if (server->check_origin && !check_host_origin(wsi)) {
        lwsl_warn(
            "refuse to serve WS client from different origin due to the "
            "--check-origin option.\n");
        return 1;
      }
      break;

    case LWS_CALLBACK_ESTABLISHED:
      pss->initialized = false;
      pss->initial_cmd_index = 0;
      pss->authenticated = false;
      pss->session = NULL;
      pss->session_resumed = false;
      pss->wsi = wsi;
      pss->lws_close_status = LWS_CLOSE_STATUS_NOSTATUS;

      if (server->url_arg) {
        while (lws_hdr_copy_fragment(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_URI_ARGS, n++) > 0) {
          if (strncmp(buf, "arg=", 4) == 0) {
            pss->args = xrealloc(pss->args, (pss->argc + 1) * sizeof(char *));
            pss->args[pss->argc] = strdup(&buf[4]);
            pss->argc++;
          }
        }
      }

      server->client_count++;

      lws_get_peer_simple(lws_get_network_wsi(wsi), pss->address, sizeof(pss->address));
      lwsl_notice("WS   %s - %s, clients: %d\n", pss->path, pss->address, server->client_count);
      break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
      if (!pss->initialized) {
        if (pss->initial_cmd_index == sizeof(initial_cmds)) {
          pss->initialized = true;
          if (pss->lws_close_status > LWS_CLOSE_STATUS_NOSTATUS || pss->session->pty_buf != NULL)
            lws_callback_on_writable(wsi);
          else
            pty_resume(pss->session->process);
          break;
        }
        if (send_initial_message(wsi, pss, pss->initial_cmd_index) < 0) {
          lwsl_err("failed to send initial message, index: %d\n", pss->initial_cmd_index);
          lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
          return -1;
        }
        pss->initial_cmd_index++;
        lws_callback_on_writable(wsi);
        break;
      }

      if (pss->lws_close_status > LWS_CLOSE_STATUS_NOSTATUS) {
        lws_close_reason(wsi, pss->lws_close_status, NULL, 0);
        return 1;
      }

      if (pss->session->pty_buf != NULL) {
        pty_buf_t *buf = pss->session->pty_buf;
        wsi_output(wsi, buf);
        pss->session->pty_buf = buf->next;
        if (pss->session->pty_buf == NULL) pss->session->pty_buf_tail = NULL;
        pty_buf_free(buf);
        if (pss->session->pty_buf != NULL)
          lws_callback_on_writable(wsi);
        else
          pty_resume(pss->session->process);
      }
      break;

    case LWS_CALLBACK_RECEIVE:
      if (pss->buffer == NULL) {
        pss->buffer = xmalloc(len);
        pss->len = len;
        memcpy(pss->buffer, in, len);
      } else {
        pss->buffer = xrealloc(pss->buffer, pss->len + len);
        memcpy(pss->buffer + pss->len, in, len);
        pss->len += len;
      }

      const char command = pss->buffer[0];

      // check auth
      if (server->credential != NULL && !pss->authenticated && command != JSON_DATA) {
        lwsl_warn("WS client not authenticated\n");
        return 1;
      }

      // check if there are more fragmented messages
      if (lws_remaining_packet_payload(wsi) > 0 || !lws_is_final_fragment(wsi)) {
        return 0;
      }

      switch (command) {
        case INPUT:
          if (!server->writable || pss->session == NULL) break;
          int err = pty_write(pss->session->process, pty_buf_init(pss->buffer + 1, pss->len - 1));
          if (err) {
            lwsl_err("uv_write: %s (%s)\n", uv_err_name(err), uv_strerror(err));
            return -1;
          }
          break;
        case RESIZE_TERMINAL:
          if (pss->session == NULL || pss->session->process == NULL) break;
          json_object_put(
              parse_window_size(pss->buffer + 1, pss->len - 1, &pss->session->process->columns, &pss->session->process->rows));
          pty_resize(pss->session->process);
          break;
        case PAUSE:
          if (pss->session != NULL) pause_process(pss->session->process);
          break;
        case RESUME:
          if (pss->session != NULL) pty_resume(pss->session->process);
          break;
        case JSON_DATA: {
          if (pss->session != NULL && pss->session->process != NULL) break;
          uint16_t columns = 0;
          uint16_t rows = 0;
          char session_id[sizeof(((struct tty_session *)0)->id)] = {0};
          json_object *obj = parse_window_size(pss->buffer, pss->len, &columns, &rows);
          if (obj == NULL) {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_INVALID_PAYLOAD, NULL, 0);
            return -1;
          }
          if (server->credential != NULL) {
            struct json_object *o = NULL;
            if (json_object_object_get_ex(obj, "AuthToken", &o)) {
              const char *token = json_object_get_string(o);
              if (token != NULL && !strcmp(token, server->credential))
                pss->authenticated = true;
              else
                lwsl_warn("WS authentication failed with token: %s\n", token);
            }
            if (!pss->authenticated) {
              json_object_put(obj);
              lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
              return -1;
            }
          }
          struct json_object *session_obj = NULL;
          if (json_object_object_get_ex(obj, "SessionId", &session_obj) &&
              json_object_is_type(session_obj, json_type_string)) {
            const char *provided_id = json_object_get_string(session_obj);
            if (provided_id != NULL && strlen(provided_id) < sizeof(session_id))
              snprintf(session_id, sizeof(session_id), "%s", provided_id);
          }
          if (!session_attach(pss, session_id)) {
            json_object_put(obj);
            lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
            return -1;
          }
          bool resumed = pss->session_resumed;
          json_object_put(obj);
          if (pss->session->process == NULL && !spawn_process(pss, columns, rows)) return 1;
          if (resumed && pss->session->process != NULL) {
            if (columns > 0) pss->session->process->columns = columns;
            if (rows > 0) pss->session->process->rows = rows;
            pty_resize(pss->session->process);
          }
          if (pss->session->process != NULL) lws_callback_on_writable(wsi);
          break;
        }
        default:
          lwsl_warn("ignored unknown message type: %c\n", command);
          break;
      }

      if (pss->buffer != NULL) {
        free(pss->buffer);
        pss->buffer = NULL;
      }
      break;

    case LWS_CALLBACK_CLOSED: {
      if (pss->wsi == NULL) break;

      struct tty_session *session = pss->session;
      pss->wsi = NULL;
      if (session != NULL && session->pss == pss) session->pss = NULL;
      pss->session = NULL;

      server->client_count--;
      lwsl_notice("WS closed from %s, clients: %d\n", pss->address, server->client_count);
      if (pss->buffer != NULL) free(pss->buffer);
      for (int i = 0; i < pss->argc; i++) free(pss->args[i]);
      free(pss->args);

      if (session != NULL && session->process != NULL) {
        pause_process(session->process);
        bool retain = server->reconnect_timeout > 0 && !server->shutting_down && !session->expired &&
                      session->id[0] != '\0' && process_running(session->process);
        if (retain) {
          lwsl_notice("retaining process, pid: %d, reconnect timeout: %d seconds\n", session->process->pid,
                      server->reconnect_timeout);
          session_start_timer(session);
        } else {
          session->expired = true;
          session_unlink(session);
          if (process_running(session->process)) {
            lwsl_notice("killing process, pid: %d\n", session->process->pid);
            pty_kill(session->process, server->sig_code);
          }
        }
      }
      session_maybe_free(session);

      if ((server->once || server->exit_no_conn) && server->client_count == 0 && server->sessions == NULL) {
        lwsl_notice("exiting due to the --once/--exit-no-conn option.\n");

        // stop accepting new ws connections
        lws_cancel_service(context);

        if (process_running(pss->process)) {
          force_exit = true;
          lwsl_notice("send ^C to force exit.\n");
        } else {
          exit(0);
        }
      }
      break;
    }

    default:
      break;
  }

  return 0;
}
