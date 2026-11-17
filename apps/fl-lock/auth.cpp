#include "auth.h"

#include <cstdlib>
#include <cstring>
#include <security/pam_appl.h>

namespace {

struct ConvData {
  const std::string *password;
  std::string message;
};

int conversation(int n, const struct pam_message **msg, struct pam_response **resp, void *appdata) {
  ConvData *d = (ConvData *)appdata;
  if (n <= 0 || n > PAM_MAX_NUM_MSG) return PAM_CONV_ERR;
  struct pam_response *r = (struct pam_response *)calloc((size_t)n, sizeof *r);
  if (!r) return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    switch (msg[i]->msg_style) {
      case PAM_PROMPT_ECHO_OFF:
      case PAM_PROMPT_ECHO_ON:
        // every prompt gets the password (a second factor is not supported)
        r[i].resp = strdup(d->password->c_str());
        break;
      case PAM_ERROR_MSG:
      case PAM_TEXT_INFO:
        if (msg[i]->msg && *msg[i]->msg) {
          if (!d->message.empty()) d->message += "\n";
          d->message += msg[i]->msg;
        }
        break;
      default:
        for (int k = 0; k < i; k++) free(r[k].resp);
        free(r);
        return PAM_CONV_ERR;
    }
  }
  *resp = r;
  return PAM_SUCCESS;
}

}  // namespace

AuthResult authenticate(const std::string &user, const std::string &password) {
  AuthResult res;
  ConvData data{&password, ""};
  struct pam_conv conv = {conversation, &data};
  pam_handle_t *pamh = nullptr;
  int rc = pam_start("fl-lock", user.c_str(), &conv, &pamh);
  if (rc != PAM_SUCCESS) {
    res.message = "The PAM service fl-lock cannot be started.";
    return res;
  }
  rc = pam_authenticate(pamh, 0);
  if (rc == PAM_SUCCESS) {
    // refresh credentials such as Kerberos tickets; failure is not fatal
    pam_setcred(pamh, PAM_REFRESH_CRED);
    res.ok = true;
  } else if (rc == PAM_MAXTRIES || rc == PAM_PERM_DENIED) {
    if (data.message.empty()) data.message = "Too many failed attempts. Wait a while, then try again.";
  }
  if (!res.ok && data.message.empty() && rc != PAM_AUTH_ERR) data.message = pam_strerror(pamh, rc);
  res.message = data.message;
  pam_end(pamh, rc);
  return res;
}
