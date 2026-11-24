#include "web_cef/cef_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace kajo_web {

namespace {

bool WriteAllFd(int fd, const char* data, size_t len, int timeout_ms) {
  size_t off = 0;
  while (off < len) {
    struct pollfd pfd{fd, POLLOUT, 0};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (pr == 0) return false;
    ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    if (n == 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool FillSockAddr(const std::string& path, struct sockaddr_un* addr) {
  if (path.empty() || path.size() >= sizeof(addr->sun_path)) return false;
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  memcpy(addr->sun_path, path.data(), path.size());
  return true;
}

int ConnectWithTimeout(const std::string& socket_path, int timeout_ms) {
  struct sockaddr_un addr {};
  if (!FillSockAddr(socket_path, &addr)) return -1;

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  if (rc < 0) {
    if (errno != EINPROGRESS) {
      close(fd);
      return -1;
    }
    struct pollfd pfd{fd, POLLOUT, 0};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
      close(fd);
      return -1;
    }
    int so_err = 0;
    socklen_t slen = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0 || so_err != 0) {
      close(fd);
      return -1;
    }
  }
  return fd;
}

}  // namespace

std::string ProfileIpcSocketPath(const std::string& profile_id) {
  std::string safe = profile_id.empty() ? "default" : profile_id;
  for (char& c : safe) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      c = '-';
    }
  }
  const gchar* rdir = g_get_user_runtime_dir();
  if (rdir && rdir[0] != '\0') {
    g_autofree gchar* p =
        g_strdup_printf("%s/kajo-web-%s.sock", rdir, safe.c_str());
    if (p && strlen(p) < sizeof(sockaddr_un::sun_path)) return std::string(p);
  }
  g_autofree gchar* fallback = g_strdup_printf(
      "/tmp/kajo-web-%lu-%s.sock", static_cast<unsigned long>(getuid()), safe.c_str());
  return fallback ? std::string(fallback) : "";
}

bool TryForwardToRunningInstance(const std::string& socket_path,
                                 const std::vector<std::string>& urls,
                                 int timeout_ms) {
  int fd = ConnectWithTimeout(socket_path, timeout_ms);
  if (fd < 0) return false;

  g_autoptr(JsonBuilder) jb = json_builder_new();
  json_builder_begin_object(jb);
  json_builder_set_member_name(jb, "urls");
  json_builder_begin_array(jb);
  for (const auto& u : urls) {
    json_builder_add_string_value(jb, u.c_str());
  }
  json_builder_end_array(jb);
  json_builder_end_object(jb);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(jb);
  json_generator_set_root(gen, root);
  g_autofree gchar* payload = json_generator_to_data(gen, nullptr);
  std::string line = payload ? std::string(payload) : "{}";
  line.push_back('\n');

  bool ok = WriteAllFd(fd, line.data(), line.size(), timeout_ms);
  if (ok) {
    char ack[16] = {};
    struct pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) > 0) {
      ssize_t n = recv(fd, ack, sizeof(ack) - 1, 0);
      ok = (n > 0 && strncmp(ack, "OK", 2) == 0);
    } else {
      ok = false;
    }
  }
  close(fd);
  return ok;
}

bool IsProfileInstanceRunning(const std::string& profile_id, int timeout_ms) {
  const std::string path = ProfileIpcSocketPath(profile_id);
  int fd = ConnectWithTimeout(path, timeout_ms);
  if (fd < 0) return false;
  static constexpr char kPing[] = "{\"ping\":true}\n";
  bool ok = WriteAllFd(fd, kPing, sizeof(kPing) - 1, timeout_ms);
  if (ok) {
    char ack[16] = {};
    struct pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) > 0) {
      ssize_t n = recv(fd, ack, sizeof(ack) - 1, 0);
      ok = (n > 0 && strncmp(ack, "OK", 2) == 0);
    } else {
      ok = false;
    }
  }
  close(fd);
  return ok;
}

ProfileIpcServer::~ProfileIpcServer() {
  Stop();
}

bool ProfileIpcServer::Start(const std::string& socket_path,
                             std::function<void(std::vector<std::string>)> on_open) {
  Stop();
  struct sockaddr_un addr {};
  if (!FillSockAddr(socket_path, &addr)) return false;

  int probe = ConnectWithTimeout(socket_path, 150);
  if (probe >= 0) {
    close(probe);
    return false;
  }
  unlink(socket_path.c_str());

  int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (lfd < 0) return false;

  if (bind(lfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(lfd);
    return false;
  }
  chmod(socket_path.c_str(), 0600);
  if (listen(lfd, 8) < 0) {
    close(lfd);
    unlink(socket_path.c_str());
    return false;
  }

  if (pipe2(wake_pipe_, O_NONBLOCK | O_CLOEXEC) < 0) {
    close(lfd);
    unlink(socket_path.c_str());
    return false;
  }

  socket_path_ = socket_path;
  on_open_ = std::move(on_open);
  listen_fd_ = lfd;
  running_.store(true, std::memory_order_release);
  thread_ = std::thread(&ProfileIpcServer::RunLoop, this);
  return true;
}

void ProfileIpcServer::Stop() {
  if (running_.exchange(false, std::memory_order_acq_rel)) {
    if (wake_pipe_[1] >= 0) {
      const char b = 'q';
      ssize_t ignored = write(wake_pipe_[1], &b, 1);
      (void)ignored;
    }
    if (thread_.joinable()) thread_.join();
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (wake_pipe_[0] >= 0) {
    close(wake_pipe_[0]);
    wake_pipe_[0] = -1;
  }
  if (wake_pipe_[1] >= 0) {
    close(wake_pipe_[1]);
    wake_pipe_[1] = -1;
  }
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
    socket_path_.clear();
  }
  on_open_ = nullptr;
}

void ProfileIpcServer::RunLoop() {
  while (running_.load(std::memory_order_acquire)) {
    struct pollfd pfds[2] = {
        {listen_fd_, POLLIN, 0},
        {wake_pipe_[0], POLLIN, 0},
    };
    int pr = poll(pfds, 2, -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (!running_.load(std::memory_order_acquire) || (pfds[1].revents & POLLIN)) {
      break;
    }
    if (pfds[0].revents & POLLIN) {
      for (;;) {
        int cfd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
          if (errno == EINTR) continue;
          break;
        }
        HandleClient(cfd);
        close(cfd);
      }
    }
  }
}

void ProfileIpcServer::HandleClient(int client_fd) {
  constexpr size_t kMaxReqBytes = 64U * 1024U;
  std::string buf;
  buf.reserve(512);
  char chunk[1024];

  for (;;) {
    struct pollfd pfd{client_fd, POLLIN, 0};
    int pr = poll(&pfd, 1, 250);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (pr == 0) break;
    ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return;
    }
    if (n == 0) break;
    const size_t take = std::min(static_cast<size_t>(n), kMaxReqBytes - buf.size());
    buf.append(chunk, take);
    if (buf.find('\n') != std::string::npos || buf.size() >= kMaxReqBytes) break;
  }

  size_t nl = buf.find('\n');
  if (nl != std::string::npos) buf.resize(nl);

  bool is_ping = false;
  std::vector<std::string> urls;
  if (!buf.empty()) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, buf.data(), static_cast<gssize>(buf.size()), nullptr)) {
      JsonNode* root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject* obj = json_node_get_object(root);
        is_ping = json_object_has_member(obj, "ping") &&
                  json_object_get_boolean_member(obj, "ping");
        if (!is_ping && json_object_has_member(obj, "urls")) {
          JsonArray* arr = json_object_get_array_member(obj, "urls");
          guint n = arr ? json_array_get_length(arr) : 0;
          for (guint i = 0; i < n; ++i) {
            const gchar* s = json_array_get_string_element(arr, i);
            if (s && s[0] != '\0') urls.emplace_back(s);
          }
        }
      }
    }
  }

  static constexpr char kOk[] = "OK\n";
  WriteAllFd(client_fd, kOk, sizeof(kOk) - 1, 200);
  if (!is_ping && on_open_) {
    on_open_(std::move(urls));
  }
}

}  // namespace kajo_web
