// The Assuan protocol as pinentry speaks it (what gpg-agent sends), without
// libassuan: one command per line, replies "OK", "ERR <code> <text>",
// "D <data>" and "S <status>". Plain logic, unit tested.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace assuan {

// libgpg-error codes with the pinentry source (GPG_ERR_SOURCE_PINENTRY = 5)
constexpr unsigned kCanceled = (5u << 24) | 99;      // GPG_ERR_CANCELED
constexpr unsigned kNotConfirmed = (5u << 24) | 114; // GPG_ERR_NOT_CONFIRMED
constexpr unsigned kTimeout = (5u << 24) | 62;       // GPG_ERR_TIMEOUT
constexpr unsigned kUnknownCommand = (5u << 24) | 275; // GPG_ERR_ASS_UNKNOWN_CMD
constexpr unsigned kParameter = (5u << 24) | 280;    // GPG_ERR_ASS_PARAMETER

std::string unescape(const std::string &s); // %XX -> byte
std::string escape(const std::string &s);   // %, CR, LF and controls -> %XX (for D lines)

// What the dialog shows; set by the SET* commands and OPTIONs
struct Request {
  std::string title, description, prompt = "PIN:", ok, cancel, notok, error;
  std::string repeat, repeat_error = "Does not match", repeat_ok;
  std::string quality_bar, quality_tooltip, genpin, genpin_tooltip, keyinfo;
  int timeout = 0;              // seconds, 0: none
  bool allow_external_cache = false;
  bool one_button = false;      // CONFIRM --one-button
  std::map<std::string, std::string> options;
};

// what the dialog answered
struct Answer {
  enum Kind { Ok, Cancel, NotOk, Timeout } kind = Cancel;
  std::string pin;
  bool repeated = false;        // the PIN was asked twice (SETREPEAT)
  bool from_cache = false;      // came from the password manager
};

// The parts that need a person (or a password manager)
struct Ui {
  std::function<Answer(const Request &)> get_pin;
  std::function<Answer(const Request &)> confirm; // also MESSAGE (one button)
};

class Session {
public:
  explicit Session(Ui ui) : ui_(std::move(ui)) {}
  // the greeting to send first
  static std::string greeting() { return "OK Pleased to meet you"; }
  // handles one command line; returns the reply lines (each without "\n")
  std::vector<std::string> handle(const std::string &line);
  bool finished() const { return bye_; }
  const Request &request() const { return req_; }

private:
  Ui ui_;
  Request req_;
  bool bye_ = false;
};

// strength of a passphrase, 0..100 (length and kinds of characters)
int quality(const std::string &pin);

} // namespace assuan
