#include "greetd_ipc.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using fld::Json;

static bool write_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= n;
  }
  return true;
}

static bool read_all(int fd, void *buf, size_t len) {
  char *p = (char *)buf;
  while (len) {
    ssize_t n = read(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

bool GreetdClient::connect(std::string &err) {
  const char *path = getenv("GREETD_SOCK");
  if (!path || !*path) {
    err = "GREETD_SOCK is not set (is fl-greet running under greetd? Use --demo to test)";
    return false;
  }
  fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    err = strerror(errno);
    return false;
  }
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof addr.sun_path) {
    err = "socket path too long";
    return false;
  }
  strcpy(addr.sun_path, path);
  if (::connect(fd_, (struct sockaddr *)&addr, sizeof addr) != 0) {
    err = std::string("cannot connect to greetd: ") + strerror(errno);
    close(fd_);
    fd_ = -1;
    return false;
  }
  return true;
}

bool GreetdClient::request(const Json &req, Json &reply, std::string &err) {
  if (fd_ < 0) {
    err = "not connected";
    return false;
  }
  std::string body = req.dump();
  uint32_t len = (uint32_t)body.size();
  if (!write_all(fd_, &len, 4) || !write_all(fd_, body.data(), body.size())) {
    err = std::string("write to greetd failed: ") + strerror(errno);
    return false;
  }
  uint32_t rlen;
  if (!read_all(fd_, &rlen, 4) || rlen > (1u << 20)) {
    err = "read from greetd failed";
    return false;
  }
  std::string resp(rlen, '\0');
  if (!read_all(fd_, &resp[0], rlen)) {
    err = "read from greetd failed";
    return false;
  }
  if (!Json::parse(resp, reply, &err)) {
    err = "bad reply from greetd: " + err;
    return false;
  }
  return true;
}

bool GreetdClient::create_session(const std::string &user, Json &reply, std::string &err) {
  Json r = Json::object();
  r.set("type", "create_session").set("username", user);
  return request(r, reply, err);
}

bool GreetdClient::post_response(const std::string *response, Json &reply, std::string &err) {
  Json r = Json::object();
  r.set("type", "post_auth_message_response");
  if (response) r.set("response", *response);
  return request(r, reply, err);
}

bool GreetdClient::start_session(const std::vector<std::string> &cmd, const std::vector<std::string> &env,
                                 Json &reply, std::string &err) {
  Json c = Json::array(), e = Json::array();
  for (auto &s : cmd) c.push(s);
  for (auto &s : env) e.push(s);
  Json r = Json::object();
  r.set("type", "start_session").set("cmd", c).set("env", e);
  return request(r, reply, err);
}

bool GreetdClient::cancel_session(Json &reply, std::string &err) {
  Json r = Json::object();
  r.set("type", "cancel_session");
  return request(r, reply, err);
}

// ---- demo ---------------------------------------------------------------------

bool DemoGreetd::request(const Json &req, Json &reply, std::string &) {
  const std::string &type = req["type"].as_string();
  reply = Json::object();
  if (type == "create_session") {
    user_ = req["username"].as_string();
    step_ = 1;
    reply.set("type", "auth_message").set("auth_message_type", "secret").set("auth_message", "Password: ");
  } else if (type == "post_auth_message_response") {
    if (step_ == 1 && req["response"].as_string() == "demo") {
      step_ = 2;
      reply.set("type", "success");
    } else {
      step_ = 0;
      reply.set("type", "error").set("error_type", "auth_error").set("description", "Authentication failed");
    }
  } else if (type == "start_session") {
    if (step_ != 2) {
      reply.set("type", "error").set("error_type", "error").set("description", "no authenticated session");
    } else {
      fprintf(stdout, "demo: would start");
      for (auto &c : req["cmd"].items()) fprintf(stdout, " %s", c.as_string().c_str());
      fprintf(stdout, " for %s\n", user_.c_str());
      fflush(stdout);
      reply.set("type", "success");
    }
  } else if (type == "cancel_session") {
    step_ = 0;
    reply.set("type", "success");
  } else {
    reply.set("type", "error").set("error_type", "error").set("description", "unknown request");
  }
  return true;
}
