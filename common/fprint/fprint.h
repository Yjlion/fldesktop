// Fingerprints through fprintd (net.reactivated.Fprint on the system bus):
// checking a finger (fl-lock) and enrolling fingers (Settings > Users).
// The D-Bus work runs in a thread of its own; callbacks come in the FLTK
// thread (call Fl::lock() once in main()).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fprint {

// fprintd runs and has a reader (asks the bus; quick)
bool available();
// the user's enrolled fingers ("right-index-finger", ...)
std::vector<std::string> enrolled(const std::string &user);
// "right-index-finger" -> "Right index finger"
std::string finger_label(const std::string &finger);
// the fingers fprintd knows, in the order to offer them
const std::vector<std::string> &all_fingers();
// fprintd's result names to a sentence ("verify-retry-scan" -> "Try again")
std::string status_text(const std::string &result);

// Checking: waits for a finger until stopped, retries after a mismatch.
class Verify {
public:
  enum Event { READY, MATCH, NO_MATCH, RETRY, UNAVAILABLE };
  // on_event(event, text) in the FLTK thread
  Verify(const std::string &user, std::function<void(Event, const std::string &)> on_event);
  ~Verify();
  void stop();
  struct Impl;

private:
  std::shared_ptr<Impl> d;
};

// Enrolling one finger: on_progress(stage, stages, text, done, ok).
class Enroll {
public:
  Enroll(const std::string &user, const std::string &finger,
         std::function<void(int stage, int stages, const std::string &text, bool done, bool ok)> on_progress);
  ~Enroll();
  void stop();
  struct Impl;

private:
  std::shared_ptr<Impl> d;
};

// Deletes all the user's fingerprints (fprintd asks polkit for others).
bool delete_all(const std::string &user, std::string &err);

} // namespace fprint
