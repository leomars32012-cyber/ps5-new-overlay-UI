/* Settings web app server (port 1994). One short-lived thread per connection,
 * one request per connection, always "Connection: close". The page is embedded by
 * web_page.c; config edits go through web_config.hpp and are written
 * atomically so the ShellUI overlay never reads a half-written file.
 */
#include "web_server.hpp"

#include "daemon_ops.hpp"
#include "web_config.hpp"

#include <onion/log.h>
#include <onion/net.h>
#include <onion/notify.h>
#include <onion/settings.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern "C" {
extern const uint8_t fps_monitor_web_page_start[];
extern const uint8_t fps_monitor_web_page_end[];
}

namespace {

namespace web = fpsmon::web;

constexpr int kPort = 1994;
constexpr size_t kMaxHead = 8192;
constexpr size_t kMaxBody = 4096;
constexpr int kTimeoutSec = 5;
constexpr unsigned kListenRetrySec = 5;
constexpr int kMaxClients = 4;

std::atomic<int> g_clients{0};
/* Serialises the config.ini read-modify-write across client threads. */
pthread_mutex_t g_post_mutex = PTHREAD_MUTEX_INITIALIZER;

timespec now_mono() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t;
}

/* recv() bounded by one deadline for the whole request, so a slow-drip
 * client cannot hold a connection past kTimeoutSec in total. */
ssize_t recv_by(int fd, char *buf, size_t len, const timespec &deadline) {
  const timespec now = now_mono();
  long usec = (deadline.tv_sec - now.tv_sec) * 1000000L +
              (deadline.tv_nsec - now.tv_nsec) / 1000L;
  if (usec <= 0) {
    LOG_DEBUG("fps-monitor: web request timed out after %d s", kTimeoutSec);
    return -1;
  }
  timeval tv{};
  tv.tv_sec = usec / 1000000L;
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>(usec % 1000000L);
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  const ssize_t n = recv(fd, buf, len, 0);
  if (n < 0)
    LOG_DEBUG("fps-monitor: web recv failed errno=%d", errno);
  return n;
}

bool send_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    const ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

const char *reason(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 413:
    return "Payload Too Large";
  default:
    return "Internal Server Error";
  }
}

void respond(int fd, int code, const char *type, const char *body,
             size_t len) {
  char head[256];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 %d %s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %zu\r\n"
                         "Cache-Control: no-store\r\n"
                         "Connection: close\r\n\r\n",
                         code, reason(code), type, len);
  if (!(n > 0 && send_all(fd, head, static_cast<size_t>(n)) &&
        send_all(fd, body, len)))
    LOG_DEBUG("fps-monitor: web send of %d response failed errno=%d", code,
              errno); // the client went away; not our error
}

void respond_json(int fd, int code, const std::string &json) {
  respond(fd, code, "application/json", json.data(), json.size());
}

void respond_error(int fd, int code, std::string_view msg) {
  if (code < 500) // a bad request from some client is not our error
    LOG_DEBUG("fps-monitor: web request rejected %d: %.*s", code,
              static_cast<int>(msg.size()), msg.data());
  respond_json(fd, code, web::json_error(msg));
}

/* The [overlay] keys as config.ini currently has them (defaults when the
 * file or a key is missing). */
std::string config_json() {
  onion::Settings s{};
  (void)onion::settings_load_file(onion::kConfigPathPrimary, &s);
  auto b = [](bool v) -> std::string_view { return v ? "true" : "false"; };
  const std::string opacity = std::to_string(s.overlay_opacity);
  const std::string graph_opacity =
      std::to_string(s.overlay_frame_graph_opacity);
  const std::string margin = std::to_string(s.overlay_margin);
  return web::config_json(
      {b(s.overlay_enabled), onion::overlay_edge_name(s.overlay_pos),
       onion::overlay_align_name(s.overlay_align),
       onion::overlay_font_size_name(s.overlay_font_size),
       onion::overlay_detail_name(s.overlay_detail), opacity,
       onion::overlay_theme_name(s.overlay_theme), b(s.overlay_brand),
       b(s.overlay_fps_pill), b(s.overlay_system), b(s.overlay_frame_graph),
       graph_opacity, margin, b(s.overlay_fps), b(s.overlay_cpu),
       b(s.overlay_gpu), b(s.overlay_ram), b(s.overlay_ip), b(s.overlay_fan),
       b(s.overlay_cpu_clock), b(s.overlay_gpu_clock), b(s.overlay_ssd_temp),
       b(s.overlay_soc_power)});
}

/* Whole config.ini; a missing file reads as "". */
bool read_config(std::string &out) {
  out.clear();
  const int fd = open(onion::kConfigPathPrimary, O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT)
      return true;
    LOG_ERROR("fps-monitor: web open %s failed errno=%d",
              onion::kConfigPathPrimary, errno);
    return false;
  }
  char buf[1024];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    out.append(buf, static_cast<size_t>(n));
  if (n < 0)
    LOG_ERROR("fps-monitor: web read %s failed errno=%d",
              onion::kConfigPathPrimary, errno);
  close(fd);
  return n == 0;
}

/* Temp file in the same directory, fsync, rename over. */
bool write_config(const std::string &text) {
  const std::string tmp = std::string(onion::kConfigPathPrimary) + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd < 0) {
    LOG_ERROR("fps-monitor: web open %s failed errno=%d", tmp.c_str(), errno);
    return false;
  }
  const ssize_t w = write(fd, text.data(), text.size());
  bool ok = w == static_cast<ssize_t>(text.size());
  if (!ok)
    LOG_ERROR("fps-monitor: web write %s wrote %ld of %zu errno=%d",
              tmp.c_str(), static_cast<long>(w), text.size(), errno);
  if (ok && fsync(fd) != 0) {
    LOG_ERROR("fps-monitor: web fsync %s failed errno=%d", tmp.c_str(), errno);
    ok = false;
  }
  if (close(fd) != 0 && ok) {
    LOG_ERROR("fps-monitor: web close %s failed errno=%d", tmp.c_str(), errno);
    ok = false;
  }
  if (ok && rename(tmp.c_str(), onion::kConfigPathPrimary) == 0)
    return true;
  if (ok)
    LOG_ERROR("fps-monitor: web rename %s -> %s failed errno=%d", tmp.c_str(),
              onion::kConfigPathPrimary, errno);
  unlink(tmp.c_str());
  return false;
}

void handle_post(int fd, std::string_view body) {
  web::Updates updates;
  const std::string err = web::parse_updates(body, updates);
  if (!err.empty()) {
    respond_error(fd, 400, err);
    return;
  }
  std::string text;
  pthread_mutex_lock(&g_post_mutex);
  const bool saved =
      read_config(text) && write_config(web::apply_updates(text, updates));
  if (saved)
    /* The mtime reload gate has 1 s resolution; push this save through. */
    (void)LoadSettings(true);
  pthread_mutex_unlock(&g_post_mutex);
  if (!saved) { // read_config / write_config logged the failing step
    respond_error(fd, 500, "could not save config.ini");
    return;
  }
  std::string summary;
  for (const auto &u : updates)
    summary += " " + u.first + "=" + u.second;
  LOG_INFO("fps-monitor: config.ini saved from web:%s", summary.c_str());
  respond_json(fd, 200, config_json());
}

void handle_client(int fd) {
  timeval tv{};
  tv.tv_sec = kTimeoutSec;
  /* A client that stops reading must not hold its thread forever. */
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  timespec deadline = now_mono();
  deadline.tv_sec += kTimeoutSec;

  std::string req;
  char chunk[1024];
  size_t head_end;
  while ((head_end = req.find("\r\n\r\n")) == std::string::npos) {
    if (req.size() >= kMaxHead) {
      respond_error(fd, 413, "request head too large");
      return;
    }
    const ssize_t n = recv_by(fd, chunk, sizeof(chunk), deadline);
    if (n <= 0)
      return; // closed, error or receive timeout
    req.append(chunk, static_cast<size_t>(n));
  }
  if (head_end + 4 > kMaxHead) {
    respond_error(fd, 413, "request head too large");
    return;
  }

  web::Head h;
  const char *err = nullptr;
  if (const int code = web::parse_head(std::string_view(req.data(), head_end),
                                       kMaxBody, h, err)) {
    respond_error(fd, code, err);
    return;
  }
  std::string body = req.substr(head_end + 4);
  while (body.size() < h.content_length) {
    const ssize_t n = recv_by(fd, chunk, sizeof(chunk), deadline);
    if (n <= 0)
      return;
    body.append(chunk, static_cast<size_t>(n));
  }
  body.resize(h.content_length);

  if (h.path == "/" || h.path == "/index.html") {
    if (h.method != "GET") {
      respond_error(fd, 405, "method not allowed");
      return;
    }
    respond(fd, 200, "text/html; charset=utf-8",
            reinterpret_cast<const char *>(fps_monitor_web_page_start),
            static_cast<size_t>(fps_monitor_web_page_end -
                                fps_monitor_web_page_start));
  } else if (h.path == "/api/config") {
    if (h.method == "GET")
      respond_json(fd, 200, config_json());
    else if (h.method != "POST")
      respond_error(fd, 405, "method not allowed");
    else if (!h.token)
      respond_error(fd, 403, "missing X-FPSMon header");
    else
      handle_post(fd, body);
  } else {
    respond_error(fd, 404, "not found");
  }
}

/* Toast once per run, not on every rest-mode re-open. */
void notify_listening() {
  static bool told = false;
  if (told)
    return;
  told = true;
  char ip[ONION_NET_IP_ADDRESS_SIZE];
  if (onion_net_get_ip_address(ip, sizeof(ip)) == ONION_NET_IP_OK)
    onion_notify(0, "[FPS Monitor] Settings: http://%s:%d", ip, kPort);
  else
    onion_notify(0, "[FPS Monitor] Settings web app on port %d", kPort);
}

void notify_port_failed() {
  static bool told = false;
  if (!told)
    onion_notify(0, "[FPS Monitor] Settings web app: port %d busy, retrying",
                 kPort);
  told = true;
}

/* Retried every kListenRetrySec: log the first failure and the recovery, not
 * every retry. Server thread only. */
bool g_listen_failing = false;
bool g_listen_opened = false;

int open_listener() {
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    if (!g_listen_failing)
      LOG_WARN("fps-monitor: web socket failed errno=%d; retrying every %u s",
               errno, kListenRetrySec);
    g_listen_failing = true;
    return -1;
  }
  const int one = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
      listen(s, 4) < 0) {
    if (!g_listen_failing)
      LOG_WARN("fps-monitor: web port %d unavailable errno=%d; retrying every "
               "%u s",
               kPort, errno, kListenRetrySec);
    g_listen_failing = true;
    close(s);
    notify_port_failed();
    return -1;
  }
  if (!g_listen_opened || g_listen_failing)
    LOG_INFO("fps-monitor: settings web app on port %d", kPort);
  g_listen_opened = true;
  g_listen_failing = false;
  notify_listening();
  return s;
}

void *client_thread(void *arg) {
  const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  handle_client(fd);
  close(fd);
  g_clients.fetch_sub(1);
  return nullptr;
}

void *server_thread(void *) {
  int listener = -1;
  bool accept_failing = false; // warn once until an accept works again
  while (!g_stack_shutting_down.load(std::memory_order_acquire)) {
    if (listener < 0 && (listener = open_listener()) < 0) {
      sleep(kListenRetrySec);
      continue;
    }
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      /* e.g. the network went away over rest mode: start a fresh socket. */
      if (!accept_failing)
        LOG_WARN("fps-monitor: web accept failed errno=%d; reopening", errno);
      accept_failing = true;
      close(listener);
      listener = -1;
      sleep(1);
      continue;
    }
    if (accept_failing)
      LOG_INFO("fps-monitor: web accept working again");
    accept_failing = false;
    /* A thread per connection so an idle browser preconnect socket cannot
     * stall the real request queued behind it. */
    pthread_t thr = nullptr;
    int rc = 0;
    if (g_clients.fetch_add(1) >= kMaxClients ||
        (rc = pthread_create(&thr, nullptr, client_thread,
                             reinterpret_cast<void *>(
                                 static_cast<intptr_t>(client)))) != 0) {
      if (rc != 0)
        LOG_WARN("fps-monitor: web client thread failed to start rc=%d", rc);
      else
        LOG_DEBUG("fps-monitor: web connection dropped, %d clients busy",
                  kMaxClients);
      g_clients.fetch_sub(1);
      close(client);
      continue;
    }
    pthread_detach(thr);
  }
  if (listener >= 0)
    close(listener);
  return nullptr;
}

} // namespace

void web_server_start() {
  pthread_t thr = nullptr;
  if (const int rc = pthread_create(&thr, nullptr, server_thread, nullptr)) {
    LOG_ERROR("fps-monitor: web server thread failed to start rc=%d", rc);
    return;
  }
  pthread_detach(thr);
}
