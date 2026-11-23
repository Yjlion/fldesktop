#include "rfb.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rfb {

// ---- DES (one block, for VNC authentication) ------------------------------------------

namespace {

const uint8_t PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                         35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                         46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
const uint8_t PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                         26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                         51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
const uint8_t SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
const uint8_t IP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22,
                        14, 6,  64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35,
                        27, 19, 11, 3,  61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
const uint8_t FP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22,
                        62, 30, 37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11,
                        51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
const uint8_t E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                       16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
const uint8_t P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                       2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
const uint8_t S[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7, 0, 15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11, 9,  5,  3,  8,
     4,  1,  14, 8, 13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5, 0, 15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0, 6, 13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4,  7,  15, 2,  8,  14, 12, 0, 1,  10, 6,  9,  11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,  9,  3, 2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6, 7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,  6,  10, 2,  8, 5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8, 15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2, 12},
    {7,  13, 14, 3, 0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15, 0,  3,  4,  7, 2,  12, 1,  10, 14, 9,
     10, 6,  9,  0, 12, 11, 7,  13, 15, 1, 3,  14, 5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,  9,  4, 5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
     4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3, 0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
     9,  14, 15, 5,  2, 8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9, 7,  5,  10, 6, 1,  13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
     1,  4,  11, 13, 12, 3, 7,  14, 10, 15, 6, 8,  0,  5,  9, 2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

uint64_t permute(uint64_t in, const uint8_t *table, int n, int in_bits) {
  uint64_t out = 0;
  for (int i = 0; i < n; i++) out = (out << 1) | ((in >> (in_bits - table[i])) & 1);
  return out;
}

} // namespace

void des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]) {
  uint64_t k = 0, block = 0;
  for (int i = 0; i < 8; i++) {
    k = (k << 8) | key[i];
    block = (block << 8) | in[i];
  }
  uint64_t cd = permute(k, PC1, 56, 64);
  uint32_t c = (uint32_t)(cd >> 28) & 0x0fffffff, d = (uint32_t)cd & 0x0fffffff;
  uint64_t sub[16];
  for (int r = 0; r < 16; r++) {
    for (int s = 0; s < SHIFTS[r]; s++) {
      c = ((c << 1) | (c >> 27)) & 0x0fffffff;
      d = ((d << 1) | (d >> 27)) & 0x0fffffff;
    }
    sub[r] = permute(((uint64_t)c << 28) | d, PC2, 48, 56);
  }
  uint64_t ip = permute(block, IP, 64, 64);
  uint32_t l = (uint32_t)(ip >> 32), rr = (uint32_t)ip;
  for (int r = 0; r < 16; r++) {
    uint64_t e = permute(rr, E, 48, 32) ^ sub[r];
    uint32_t f = 0;
    for (int s = 0; s < 8; s++) {
      int six = (int)((e >> (42 - 6 * s)) & 0x3f);
      int row = ((six & 0x20) >> 4) | (six & 1), col = (six >> 1) & 0xf;
      f = (f << 4) | S[s][row * 16 + col];
    }
    f = (uint32_t)permute(f, P, 32, 32);
    uint32_t t = rr;
    rr = l ^ f;
    l = t;
  }
  uint64_t pre = ((uint64_t)rr << 32) | l;
  uint64_t o = permute(pre, FP, 64, 64);
  for (int i = 7; i >= 0; i--) {
    out[i] = (uint8_t)(o & 0xff);
    o >>= 8;
  }
}

std::string vnc_auth_response(const std::string &challenge, const std::string &password) {
  uint8_t key[8] = {0};
  for (size_t i = 0; i < 8 && i < password.size(); i++) {
    uint8_t b = (uint8_t)password[i], r = 0;
    for (int j = 0; j < 8; j++)
      if (b & (1 << j)) r |= (uint8_t)(0x80 >> j);
    key[i] = r;
  }
  std::string out(16, '\0');
  for (int blk = 0; blk < 2 && challenge.size() >= 16; blk++)
    des_encrypt(key, (const uint8_t *)challenge.data() + blk * 8, (uint8_t *)&out[(size_t)blk * 8]);
  return out;
}

// ---- the protocol ---------------------------------------------------------------------

namespace {

uint16_t u16(const std::string &s, size_t i) { return (uint16_t)(((uint8_t)s[i] << 8) | (uint8_t)s[i + 1]); }
uint32_t u32(const std::string &s, size_t i) { return ((uint32_t)u16(s, i) << 16) | u16(s, i + 2); }
void put16(std::string &o, uint16_t v) {
  o += (char)(v >> 8);
  o += (char)(v & 0xff);
}
void put32(std::string &o, uint32_t v) {
  put16(o, (uint16_t)(v >> 16));
  put16(o, (uint16_t)(v & 0xffff));
}

} // namespace

bool Client::fail(const std::string &why) {
  error_ = why;
  state_ = FAILED;
  return false;
}

std::string Client::take_output() {
  std::string o;
  o.swap(out_);
  return o;
}

void Client::resize(int w, int h) {
  w_ = w;
  h_ = h;
  fb_.assign((size_t)w * h, 0);
  if (on_resize) on_resize();
}

void Client::client_init_messages() {
  // SetPixelFormat: 32 bpp, depth 24, little endian, true colour, 8 bits each
  std::string m;
  m += (char)0;
  m.append(3, '\0');
  m += (char)32;
  m += (char)24;
  m += (char)0;
  m += (char)1;
  put16(m, 255);
  put16(m, 255);
  put16(m, 255);
  m += (char)16;
  m += (char)8;
  m += (char)0;
  m.append(3, '\0');
  // SetEncodings: CopyRect, Raw, DesktopSize
  m += (char)2;
  m += (char)0;
  put16(m, 3);
  put32(m, 1);
  put32(m, 0);
  put32(m, (uint32_t)-223);
  out_ += m;
  request_update(false);
}

void Client::request_update(bool incremental) {
  if (state_ != NORMAL) return;
  out_ += (char)3;
  out_ += (char)(incremental ? 1 : 0);
  put16(out_, 0);
  put16(out_, 0);
  put16(out_, (uint16_t)w_);
  put16(out_, (uint16_t)h_);
}

void Client::pointer(int x, int y, int buttons) {
  if (state_ != NORMAL) return;
  out_ += (char)5;
  out_ += (char)buttons;
  put16(out_, (uint16_t)std::max(0, std::min(w_ - 1, x)));
  put16(out_, (uint16_t)std::max(0, std::min(h_ - 1, y)));
}

void Client::key(uint32_t keysym, bool down) {
  if (state_ != NORMAL || !keysym) return;
  out_ += (char)4;
  out_ += (char)(down ? 1 : 0);
  put16(out_, 0);
  put32(out_, keysym);
}

void Client::cut_text(const std::string &t) {
  if (state_ != NORMAL) return;
  out_ += (char)6;
  out_.append(3, '\0');
  put32(out_, (uint32_t)t.size());
  out_ += t;
}

void Client::password_given() {
  if (!need_password_) return;
  need_password_ = false;
  out_ += vnc_auth_response(challenge_, password_);
  state_ = SECURITY_RESULT;
}

bool Client::feed(const char *data, size_t n) {
  if (state_ == FAILED) return false;
  in_.append(data, n);
  while (step()) {}
  return state_ != FAILED;
}

// one step of the protocol on the buffered bytes; false when more are needed
bool Client::step() {
  switch (state_) {
    case VERSION: {
      if (in_.size() < 12) return false;
      int major = 0, minor = 0;
      if (sscanf(in_.substr(0, 12).c_str(), "RFB %3d.%3d\n", &major, &minor) != 2 || major != 3)
        return fail("This is not a VNC server.");
      in_.erase(0, 12);
      minor_ = minor >= 8 ? 8 : minor >= 7 ? 7 : 3;
      char v[16];
      snprintf(v, sizeof v, "RFB 003.%03d\n", minor_);
      out_ += v;
      state_ = minor_ == 3 ? SECURITY_33 : SECURITY_TYPES;
      return true;
    }
    case SECURITY_TYPES: {
      if (in_.empty()) return false;
      int n = (uint8_t)in_[0];
      if (n == 0) {
        state_ = FAIL_REASON;
        in_.erase(0, 1);
        return true;
      }
      if ((int)in_.size() < 1 + n) return false;
      bool has_none = false, has_vnc = false;
      for (int i = 0; i < n; i++) {
        int t = (uint8_t)in_[(size_t)1 + i];
        has_none |= t == 1;
        has_vnc |= t == 2;
      }
      in_.erase(0, (size_t)1 + n);
      if (has_none) {
        out_ += (char)1;
        // 3.7 sends no result for "none"
        state_ = minor_ == 8 ? SECURITY_RESULT : SERVER_INIT;
        if (state_ == SERVER_INIT) out_ += (char)1; // ClientInit: shared
      } else if (has_vnc) {
        out_ += (char)2;
        state_ = CHALLENGE;
      } else {
        return fail("The server wants a kind of authentication fl-remote does not know (only none and VNC passwords).");
      }
      return true;
    }
    case SECURITY_33: {
      if (in_.size() < 4) return false;
      uint32_t t = u32(in_, 0);
      in_.erase(0, 4);
      if (t == 0) {
        state_ = FAIL_REASON;
        return true;
      }
      if (t == 1) {
        out_ += (char)1;
        state_ = SERVER_INIT;
      } else if (t == 2) {
        state_ = CHALLENGE;
      } else {
        return fail("Unknown security type.");
      }
      return true;
    }
    case CHALLENGE: {
      if (need_password_) return false;
      if (in_.size() < 16) return false;
      challenge_ = in_.substr(0, 16);
      in_.erase(0, 16);
      if (password_.empty() && on_need_password) {
        need_password_ = true;
        on_need_password();
        return false;
      }
      out_ += vnc_auth_response(challenge_, password_);
      state_ = SECURITY_RESULT;
      return true;
    }
    case SECURITY_RESULT: {
      if (in_.size() < 4) return false;
      uint32_t r = u32(in_, 0);
      in_.erase(0, 4);
      if (r != 0) {
        if (minor_ == 8) {
          state_ = FAIL_REASON;
          error_ = "The password is wrong.";
          return true;
        }
        return fail("The password is wrong.");
      }
      out_ += (char)1; // ClientInit: shared
      state_ = SERVER_INIT;
      return true;
    }
    case FAIL_REASON: {
      if (in_.size() < 4) return false;
      uint32_t len = u32(in_, 0);
      if (in_.size() < 4 + (size_t)len) return false;
      std::string why = in_.substr(4, len);
      in_.erase(0, 4 + (size_t)len);
      return fail(why.empty() ? (error_.empty() ? "The server refused the connection." : error_) : why);
    }
    case SERVER_INIT: {
      if (in_.size() < 24) return false;
      uint32_t nl = u32(in_, 20);
      if (in_.size() < 24 + (size_t)nl) return false;
      int w = u16(in_, 0), h = u16(in_, 2);
      name_ = in_.substr(24, nl);
      in_.erase(0, 24 + (size_t)nl);
      state_ = NORMAL;
      resize(w, h);
      client_init_messages();
      if (on_connected) on_connected();
      return true;
    }
    case NORMAL: {
      if (rects_left_ > 0) {
        if (in_.size() < 12) return false;
        int x = u16(in_, 0), y = u16(in_, 2), w = u16(in_, 4), h = u16(in_, 6);
        int32_t enc = (int32_t)u32(in_, 8);
        if (enc == 0) { // Raw
          size_t need = 12 + (size_t)w * h * 4;
          if (in_.size() < need) return false;
          const uint8_t *p = (const uint8_t *)in_.data() + 12;
          for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
              const uint8_t *q = p + ((size_t)r * w + c) * 4;
              int X = x + c, Y = y + r;
              if (X < w_ && Y < h_) fb_[(size_t)Y * w_ + X] = ((uint32_t)q[2] << 16) | ((uint32_t)q[1] << 8) | q[0];
            }
          in_.erase(0, need);
        } else if (enc == 1) { // CopyRect
          if (in_.size() < 16) return false;
          int sx = u16(in_, 12), sy = u16(in_, 14);
          std::vector<uint32_t> tmp((size_t)w * h);
          for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++)
              tmp[(size_t)r * w + c] = sx + c < w_ && sy + r < h_ ? fb_[(size_t)(sy + r) * w_ + sx + c] : 0;
          for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++)
              if (x + c < w_ && y + r < h_) fb_[(size_t)(y + r) * w_ + x + c] = tmp[(size_t)r * w + c];
          in_.erase(0, 16);
        } else if (enc == -223) { // DesktopSize
          in_.erase(0, 12);
          resize(w, h);
          rects_left_--;
          if (rects_left_ == 0) request_update(false);
          return true;
        } else {
          return fail("The server sent an encoding fl-remote did not ask for.");
        }
        rects_left_--;
        if (on_update) on_update(x, y, w, h);
        if (rects_left_ == 0) request_update(true);
        return true;
      }
      if (in_.empty()) return false;
      int type = (uint8_t)in_[0];
      if (type == 0) { // FramebufferUpdate
        if (in_.size() < 4) return false;
        rects_left_ = u16(in_, 2);
        in_.erase(0, 4);
        if (rects_left_ == 0) request_update(true);
        return true;
      }
      if (type == 1) { // SetColourMapEntries (not used with true colour)
        if (in_.size() < 6) return false;
        size_t need = 6 + (size_t)u16(in_, 4) * 6;
        if (in_.size() < need) return false;
        in_.erase(0, need);
        return true;
      }
      if (type == 2) { // Bell
        in_.erase(0, 1);
        if (on_bell) on_bell();
        return true;
      }
      if (type == 3) { // ServerCutText
        if (in_.size() < 8) return false;
        uint32_t len = u32(in_, 4);
        if (in_.size() < 8 + (size_t)len) return false;
        std::string t = in_.substr(8, len);
        in_.erase(0, 8 + (size_t)len);
        if (on_cut_text) on_cut_text(t);
        return true;
      }
      return fail("The server sent a message fl-remote does not know.");
    }
    case FAILED: return false;
  }
  return false;
}

uint32_t keysym_for(int k, const std::string &text) {
  // FLTK's special keys are X keysyms (0xff00 and up)
  if (k >= 0xfe00 && k <= 0xffff) {
    // FLTK's keypad keys are FL_KP + the ASCII key: X has them at 0xff80 + key too
    if (k == 0xff00 + 0x8d) return 0xff8d; // KP_Enter
    if (k == 0xffe7) return 0xffeb;        // FLTK's Meta is the Super key
    if (k == 0xffe8) return 0xffec;
    return (uint32_t)k;
  }
  if (!text.empty()) {
    // the first character of the typed text
    unsigned char c = (unsigned char)text[0];
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xf0) cp = c & 0x07, extra = 3;
    else if (c >= 0xe0) cp = c & 0x0f, extra = 2;
    else if (c >= 0xc0) cp = c & 0x1f, extra = 1;
    for (int i = 1; i <= extra && i < (int)text.size(); i++) cp = (cp << 6) | ((unsigned char)text[(size_t)i] & 0x3f);
    if (cp >= 0x20 && cp <= 0xff) return cp;
    if (cp > 0xff) return 0x01000000 + cp;
  }
  if (k >= 0x20 && k < 0x7f) return (uint32_t)k;
  return 0;
}

} // namespace rfb
