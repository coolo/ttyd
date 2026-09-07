#include <libwebsockets.h>
#include <stdbool.h>
#include <uv.h>

#include "pty.h"

// client message
#define INPUT '0'
#define RESIZE_TERMINAL '1'
#define PAUSE '2'
#define RESUME '3'
#define JSON_DATA '{'

// server message
#define OUTPUT '0'
#define SET_WINDOW_TITLE '1'
#define SET_PREFERENCES '2'
#define SESSION_STATUS '3'

// url paths
struct endpoints {
  char *ws;
  char *index;
  char *token;
  char *parent;
};

extern volatile bool force_exit;
extern struct lws_context *context;
extern struct server *server;
extern struct endpoints endpoints;

struct pss_tty;
struct tty_session {
  char id[64];
  char user[30];
  struct pss_tty *pss;
  pty_process *process;
  pty_buf_t *pty_buf;
  pty_buf_t *pty_buf_tail;
  uv_timer_t *timer;
  bool listed;
  bool expired;
  struct tty_session *next;
};

struct pss_http {
  char path[128];
  char *buffer;
  char *ptr;
  size_t len;
};

struct pss_tty {
  bool initialized;
  int initial_cmd_index;
  bool authenticated;
  char user[30];
  char address[50];
  char path[128];
  char **args;
  int argc;

  struct lws *wsi;
  char *buffer;
  size_t len;

  struct tty_session *session;
  bool session_resumed;

  int lws_close_status;
};


struct server {
  int client_count;        // client count
  char *prefs_json;        // client preferences
  char *credential;        // encoded basic auth credential
  char *auth_header;       // header name used for auth proxy
  char *index;             // custom index.html
  char *command;           // full command line
  char **argv;             // command with arguments
  int argc;                // command + arguments count
  char *cwd;               // working directory
  int sig_code;            // close signal
  char sig_name[20];       // human readable signal string
  bool url_arg;            // allow client to send cli arguments in URL
  bool writable;           // whether clients to write to the TTY
  bool check_origin;       // whether allow websocket connection from different origin
  int max_clients;         // maximum clients to support
  bool once;               // whether accept only one client and exit on disconnection
  bool exit_no_conn;        // whether exit on all clients disconnection
  int reconnect_timeout;    // seconds to retain disconnected sessions
  bool shutting_down;      // whether the server is shutting down
  struct tty_session *sessions;
  char socket_path[255];   // UNIX domain socket path
  char terminal_type[30];  // terminal type to report

  uv_loop_t *loop;         // the libuv event loop
};
