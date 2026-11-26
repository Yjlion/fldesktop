// fl-remote: a VNC (RFB 3.3 / 3.7 / 3.8) client without a library.
// Security: none and VNC password; encodings: Raw, CopyRect and DesktopSize.
// The protocol works on byte buffers (feed what the server sent, send what
// out() has), so it is tested without a network.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rfb {

// VNC authentication: the 16-byte challenge encrypted with DES, the key
// being the password (8 bytes, bits reversed)
std::string vnc_auth_response(const std::string &challenge, const std::string &password);
// one DES block (exposed for the test vectors)
void des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

class Client {
public:
  enum State { VERSION, SECURITY_TYPES, SECURITY_33, CHALLENGE, SECURITY_RESULT, FAIL_REASON, SERVER_INIT, NORMAL, FAILED };

  explicit Client(const std::string &password = "") : password_(password) {}
  // bytes from the server; returns false once the connection failed
  bool feed(const char *data, size_t n);
  // bytes to send (taken)
  std::string take_output();

  State state() const { return state_; }
  const std::string &error() const { return error_; }
  int width() const { return w_; }
  int height() const { return h_; }
  const std::string &name() const { return name_; }
  // the screen: 0x00RRGGBB per pixel, rows of width()
  const std::vector<uint32_t> &pixels() const { return fb_; }

  // input; x, y in screen pixels, buttons a mask (1 left, 2 middle, 4 right, 8/16 wheel)
  void pointer(int x, int y, int buttons);
  void key(uint32_t keysym, bool down);
  void cut_text(const std::string &latin1);
  void request_update(bool incremental);

  std::function<void(int x, int y, int w, int h)> on_update; // pixels changed
  std::function<void()> on_resize, on_connected, on_bell, on_need_password;
  std::function<void(const std::string &)> on_cut_text;

  // password asked for later (on_need_password)
  void set_password(const std::string &p) { password_ = p; }
  bool waiting_for_password() const { return need_password_; }
  void password_given();

private:
  State state_ = VERSION;
  std::string in_, out_, error_, password_, name_, challenge_;
  int minor_ = 8;
  int w_ = 0, h_ = 0;
  std::vector<uint32_t> fb_;
  bool need_password_ = false;
  // a framebuffer update in progress
  int rects_left_ = 0;
  bool step();
  bool fail(const std::string &why);
  bool resize(int w, int h); // false: too large (the connection failed)
  void client_init_messages();
};

// FLTK key to an X keysym: special keys are keysyms already; text keys come
// from the typed text (Latin-1 as is, other Unicode as 0x01000000 + code).
uint32_t keysym_for(int fltk_key, const std::string &text);

} // namespace rfb
