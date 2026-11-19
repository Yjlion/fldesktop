// greetd IPC client: native-endian u32 length + JSON payload over $GREETD_SOCK
#pragma once
#include <string>
#include <vector>
#include "fld/json.h"

class GreetdClient {
public:
  virtual ~GreetdClient() = default;
  virtual bool connect(std::string &err);
  // Sends a request and waits for the reply.
  virtual bool request(const fld::Json &req, fld::Json &reply, std::string &err);

  // Convenience wrappers
  bool create_session(const std::string &user, fld::Json &reply, std::string &err);
  bool post_response(const std::string *response, fld::Json &reply, std::string &err);
  bool start_session(const std::vector<std::string> &cmd, const std::vector<std::string> &env,
                     fld::Json &reply, std::string &err);
  bool cancel_session(fld::Json &reply, std::string &err);

protected:
  int fd_ = -1;
};

// Fake greetd for --demo: user "demo"/any user with password "demo" works.
class DemoGreetd : public GreetdClient {
public:
  bool connect(std::string &) override { return true; }
  bool request(const fld::Json &req, fld::Json &reply, std::string &err) override;
private:
  std::string user_;
  int step_ = 0;
};

// A message for the user from a greetd "error" reply. greetd passes PAM's
// error codes through ("pam_authenticate: AUTH_ERR"); those become plain
// sentences, anything unknown becomes a generic "Login failed" text.
std::string greetd_error_text(const fld::Json &reply);
