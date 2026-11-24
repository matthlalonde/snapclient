// Client.SetName on the snapserver's control port, see snapserver_rename.h.
// Each attempt runs on its own short task: connect, send one JSON-RPC line,
// wait for the reply with our id (skipping notifications), close.

#include "snapserver_rename.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "settings_manager.h"

static const char *TAG = "SNAP_RENAME";

#define RENAME_TIMEOUT_MS 3000  // for the connect, and again for the reply
#define RENAME_TASK_STACK (6 * 1024)  // one-shot: cJSON, sockets, event log
#define RENAME_TASK_PRIORITY 2
// our reply is short; longer lines are notifications and are skipped
#define RENAME_LINE_MAX 512

typedef enum {
  RENAME_NONE = 0,
  RENAME_PENDING,
  RENAME_RUNNING,
  RENAME_DONE,
  RENAME_FAILED,
} rename_state_t;

typedef enum {
  REPLY_NONE = 0,  // not our reply (a notification), keep reading
  REPLY_OK,
  REPLY_ERROR,
} reply_t;

// read by the web server's task, written under s_mux or by the one rename
// task
static volatile rename_state_t s_state = RENAME_NONE;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// one rename at a time, so one copy of its inputs
static char s_ip[48];
static char s_client_id[32];
static char s_name[64];
static int s_rpc_id;

// strlcpy-style: always terminated, never overruns
static void copy_str(char *dst, size_t size, const char *src) {
  size_t n = 0;
  if (src != NULL) {
    n = strnlen(src, size - 1);
    memcpy(dst, src, n);
  }
  dst[n] = '\0';
}

// Connect to ip:port within RENAME_TIMEOUT_MS. Returns a blocking socket, or
// -1 with a reason in why.
static int connect_server(const char *ip, uint16_t port, const char **why) {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  memset(&addr, 0, sizeof(addr));
  struct sockaddr_in *a4 = (struct sockaddr_in *)&addr;
  if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
    a4->sin_family = AF_INET;
    a4->sin_port = htons(port);
    addr_len = sizeof(*a4);
#if CONFIG_LWIP_IPV6
  } else if (inet_pton(AF_INET6, ip,
                       &((struct sockaddr_in6 *)&addr)->sin6_addr) == 1) {
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&addr;
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(port);
    addr_len = sizeof(*a6);
#endif
  } else {
    *why = "no server address";
    return -1;
  }

  int s = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) {
    *why = "no free socket";
    return -1;
  }
  // non-blocking connect, so select() can bound the wait
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  if (connect(s, (struct sockaddr *)&addr, addr_len) != 0) {
    if (errno != EINPROGRESS) {
      *why = "could not connect";
      close(s);
      return -1;
    }
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv = {.tv_sec = RENAME_TIMEOUT_MS / 1000,
                         .tv_usec = (RENAME_TIMEOUT_MS % 1000) * 1000};
    int r = select(s + 1, NULL, &wset, NULL, &tv);
    int err = 0;
    socklen_t len = sizeof(err);
    if (r <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
        err != 0) {
      *why = r == 0 ? "no answer from the control port"
                    : "could not connect to the control port";
      close(s);
      return -1;
    }
  }
  fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

  struct timeval to = {.tv_sec = RENAME_TIMEOUT_MS / 1000,
                       .tv_usec = (RENAME_TIMEOUT_MS % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  return s;
}

// {"id":N,"jsonrpc":"2.0","method":"Client.SetName","params":{...}}\r\n,
// built with cJSON so any name is escaped. The caller frees it.
static char *build_request(int id, const char *client_id, const char *name) {
  cJSON *req = cJSON_CreateObject();
  if (req == NULL) return NULL;
  cJSON_AddNumberToObject(req, "id", id);
  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddStringToObject(req, "method", "Client.SetName");
  cJSON *params = cJSON_AddObjectToObject(req, "params");
  char *json = NULL;
  if (params != NULL && cJSON_AddStringToObject(params, "id", client_id) &&
      cJSON_AddStringToObject(params, "name", name)) {
    json = cJSON_PrintUnformatted(req);
  }
  cJSON_Delete(req);
  if (json == NULL) return NULL;

  size_t n = strlen(json);
  char *line = malloc(n + 3);
  if (line != NULL) {
    memcpy(line, json, n);
    memcpy(line + n, "\r\n", 3);
  }
  cJSON_free(json);
  return line;
}

static bool send_all(int s, const char *data, size_t len) {
  while (len > 0) {
    int n = send(s, data, len, 0);
    if (n <= 0) return false;
    data += n;
    len -= (size_t)n;
  }
  return true;
}

// Is this line our reply? On an error reply, why gets the server's reason.
static reply_t check_reply(const char *line, int id, char *why,
                           size_t why_len) {
  cJSON *root = cJSON_Parse(line);
  if (root == NULL) return REPLY_NONE;
  reply_t reply = REPLY_NONE;
  const cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "id");
  const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
  // an error with a null id: the server could not read our request
  bool ours = (cJSON_IsNumber(rid) && rid->valueint == id) ||
              (cJSON_IsNull(rid) && error != NULL);
  if (ours) {
    if (error == NULL && cJSON_HasObjectItem(root, "result")) {
      reply = REPLY_OK;
    } else {
      reply = REPLY_ERROR;
      // snapserver puts the detail in "data" ("Client not found"), the
      // JSON-RPC text in "message"
      const cJSON *data = cJSON_GetObjectItemCaseSensitive(error, "data");
      const cJSON *msg = cJSON_GetObjectItemCaseSensitive(error, "message");
      const char *text = cJSON_IsString(data)  ? data->valuestring
                         : cJSON_IsString(msg) ? msg->valuestring
                                               : "the server refused it";
      snprintf(why, why_len, "%s", text);
    }
  }
  cJSON_Delete(root);
  return reply;
}

// Read lines until our reply or the deadline. Returns true on success.
static bool read_reply(int s, int id, char *why, size_t why_len) {
  char *line = malloc(RENAME_LINE_MAX);
  if (line == NULL) {
    snprintf(why, why_len, "out of memory");
    return false;
  }
  snprintf(why, why_len, "no reply from the server");
  int64_t deadline = esp_timer_get_time() + RENAME_TIMEOUT_MS * 1000LL;
  size_t len = 0;
  bool skipping = false;  // inside a line too long to be our reply
  reply_t reply = REPLY_NONE;
  while (reply == REPLY_NONE) {
    int64_t left = deadline - esp_timer_get_time();
    if (left <= 0) break;
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(s, &rset);
    struct timeval tv = {.tv_sec = left / 1000000, .tv_usec = left % 1000000};
    if (select(s + 1, &rset, NULL, NULL, &tv) <= 0) break;
    char buf[128];
    int n = recv(s, buf, sizeof(buf), 0);
    if (n <= 0) {
      snprintf(why, why_len, "the server closed the connection");
      break;
    }
    for (int i = 0; i < n && reply == REPLY_NONE; i++) {
      char c = buf[i];
      if (c == '\n') {
        if (!skipping) {
          line[len] = '\0';
          reply = check_reply(line, id, why, why_len);
        }
        len = 0;
        skipping = false;
      } else if (c == '\r' || skipping) {
        // line ends and the rest of an overlong line are dropped
      } else if (len < RENAME_LINE_MAX - 1) {
        line[len++] = c;
      } else {
        skipping = true;
      }
    }
  }
  free(line);
  return reply == REPLY_OK;
}

static bool rename_once(char *why, size_t why_len) {
  const char *reason = NULL;
  int s = connect_server(s_ip, CONFIG_SNAPSERVER_CONTROL_PORT, &reason);
  if (s < 0) {
    snprintf(why, why_len, "%s", reason);
    return false;
  }
  bool ok = false;
  char *req = build_request(s_rpc_id, s_client_id, s_name);
  if (req == NULL) {
    snprintf(why, why_len, "out of memory");
  } else if (!send_all(s, req, strlen(req))) {
    snprintf(why, why_len, "could not send the request");
  } else {
    ok = read_reply(s, s_rpc_id, why, why_len);
  }
  free(req);
  close(s);
  return ok;
}

static void rename_task(void *arg) {
  // give the server a moment to add this client from the Hello just sent
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_LOGI(TAG, "setting the name on %s:%d to \"%s\"", s_ip,
           CONFIG_SNAPSERVER_CONTROL_PORT, s_name);

  char why[64];
  if (rename_once(why, sizeof(why))) {
    // a newer name saved meanwhile still needs its own rename
    if (settings_clear_snapcast_rename_if(s_name, NULL) != ESP_OK) {
      ESP_LOGW(TAG, "could not clear the pending rename");
    }
    event_log_addf(EVENT_SRC_SNAPCAST, EVENT_INFO,
                   "Renamed on the snapserver to %s", s_name);
    s_state = RENAME_DONE;
  } else {
    ESP_LOGW(TAG, "rename failed: %s", why);
    event_log_addf(EVENT_SRC_SNAPCAST, EVENT_WARN,
                   "Could not rename on the snapserver: %s", why);
    s_state = RENAME_FAILED;
  }
  ESP_LOGI(TAG, "stack free: %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  vTaskDelete(NULL);
}

void snapserver_rename_start(const char *server_ip, const char *client_id,
                             const char *name) {
  bool busy;
  taskENTER_CRITICAL(&s_mux);
  busy = s_state == RENAME_RUNNING;
  if (!busy) s_state = RENAME_RUNNING;
  taskEXIT_CRITICAL(&s_mux);
  if (busy) return;

  copy_str(s_ip, sizeof(s_ip), server_ip);
  copy_str(s_client_id, sizeof(s_client_id), client_id);
  copy_str(s_name, sizeof(s_name), name);
  s_rpc_id++;
  if (xTaskCreate(rename_task, "snap_rename", RENAME_TASK_STACK, NULL,
                  RENAME_TASK_PRIORITY, NULL) != pdPASS) {
    event_log_add(EVENT_SRC_SNAPCAST, EVENT_WARN,
                  "Could not rename on the snapserver: out of memory");
    s_state = RENAME_FAILED;
  }
}

void snapserver_rename_set_pending(void) {
  taskENTER_CRITICAL(&s_mux);
  if (s_state != RENAME_RUNNING) s_state = RENAME_PENDING;
  taskEXIT_CRITICAL(&s_mux);
}

const char *snapserver_rename_state(void) {
  switch (s_state) {
    case RENAME_PENDING:
      return "pending";
    case RENAME_RUNNING:
      return "renaming";
    case RENAME_DONE:
      return "done";
    case RENAME_FAILED:
      return "failed";
    default:
      return "";
  }
}
