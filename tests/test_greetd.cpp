// GreetdClient against a fake greetd server on a unix socket
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "greetd_ipc.h"
#include "test.h"

using fld::Json;

static bool rd(int fd, void *b, size_t n) {
  char *p = (char *)b;
  while (n) { ssize_t r = read(fd, p, n); if (r <= 0) return false; p += r; n -= r; }
  return true;
}

static void send_json(int fd, const Json &j) {
  std::string s = j.dump();
  uint32_t len = (uint32_t)s.size();
  if (write(fd, &len, 4) != 4 || write(fd, s.data(), s.size()) != (ssize_t)s.size()) abort();
}

static Json recv_json(int fd) {
  uint32_t len;
  Json j;
  if (!rd(fd, &len, 4)) return j;
  std::string s(len, 0);
  rd(fd, &s[0], len);
  Json::parse(s, j);
  return j;
}

int main() {
  std::string path = "/tmp/fld-greetd-test-" + std::to_string(getpid()) + ".sock";
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path.c_str());
  unlink(path.c_str());
  CHECK(bind(srv, (sockaddr *)&addr, sizeof addr) == 0);
  CHECK(listen(srv, 1) == 0);
  std::vector<Json> seen;
  std::thread server([&] {
    int c = accept(srv, nullptr, nullptr);
    // create_session -> secret prompt -> success -> start_session -> success
    seen.push_back(recv_json(c));
    Json r = Json::object();
    r.set("type", "auth_message").set("auth_message_type", "secret").set("auth_message", "Password:");
    send_json(c, r);
    seen.push_back(recv_json(c));
    bool good = seen.back()["response"].as_string() == "hunter2";
    Json ok = Json::object();
    ok.set("type", good ? "success" : "error");
    send_json(c, ok);
    seen.push_back(recv_json(c));
    send_json(c, ok);
    close(c);
  });
  setenv("GREETD_SOCK", path.c_str(), 1);
  GreetdClient cl;
  std::string err;
  CHECK(cl.connect(err));
  Json reply;
  CHECK(cl.create_session("alice", reply, err));
  CHECK_EQ(reply["type"].as_string(), std::string("auth_message"));
  std::string pw = "hunter2";
  CHECK(cl.post_response(&pw, reply, err));
  CHECK_EQ(reply["type"].as_string(), std::string("success"));
  CHECK(cl.start_session({"fl-session"}, {"XDG_SESSION_TYPE=wayland"}, reply, err));
  CHECK_EQ(reply["type"].as_string(), std::string("success"));
  server.join();
  CHECK_EQ(seen.size(), 3u);
  CHECK_EQ(seen[0]["type"].as_string(), std::string("create_session"));
  CHECK_EQ(seen[0]["username"].as_string(), std::string("alice"));
  CHECK_EQ(seen[1]["type"].as_string(), std::string("post_auth_message_response"));
  CHECK_EQ(seen[2]["type"].as_string(), std::string("start_session"));
  CHECK_EQ(seen[2]["cmd"].items()[0].as_string(), std::string("fl-session"));
  close(srv);
  unlink(path.c_str());

  // demo backend
  DemoGreetd demo;
  CHECK(demo.create_session("x", reply, err));
  std::string bad = "nope";
  CHECK(demo.post_response(&bad, reply, err));
  CHECK_EQ(reply["type"].as_string(), std::string("error"));
  return test_result("greetd");
}
