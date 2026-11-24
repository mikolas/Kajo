#ifndef KAJO_WEB_CEF_IPC_H
#define KAJO_WEB_CEF_IPC_H

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace kajo_web {

/* Returns the per-profile Unix domain socket path for single-instance forwarding. */
std::string ProfileIpcSocketPath(const std::string& profile_id);

/* Unified single-instance forwarding payload (urls and/or web app launch). */
struct IpcOpenRequest {
  std::string app_id;
  std::string app_url;
  std::vector<std::string> urls;
};

/* Attempts to forward |urls| to an existing listener on |socket_path|.
 * Returns true if the message was accepted by a running instance. */
bool TryForwardToRunningInstance(const std::string& socket_path,
                                 const std::vector<std::string>& urls,
                                 int timeout_ms = 500);

/* Attempts to forward a web app or URL open request to a running profile instance. */
bool TryForwardAppToRunningInstance(const std::string& socket_path,
                                    const IpcOpenRequest& req,
                                    int timeout_ms = 500);

/* Returns true if an instance for |profile_id| responds on its IPC socket. */
bool IsProfileInstanceRunning(const std::string& profile_id, int timeout_ms = 200);

class ProfileIpcServer {
public:
  ProfileIpcServer() = default;
  ~ProfileIpcServer();

  ProfileIpcServer(const ProfileIpcServer&) = delete;
  ProfileIpcServer& operator=(const ProfileIpcServer&) = delete;

  bool Start(const std::string& socket_path,
             std::function<void(std::vector<std::string>)> on_open);
  bool StartEx(const std::string& socket_path,
               std::function<void(IpcOpenRequest)> on_open_req);
  void Stop();

private:
  void RunLoop();
  void HandleClient(int client_fd);

  std::string socket_path_;
  std::function<void(IpcOpenRequest)> on_open_req_;
  int listen_fd_ = -1;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_IPC_H
