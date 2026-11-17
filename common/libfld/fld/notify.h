// Desktop notifications (org.freedesktop.Notifications) for fldesktop programs.
//
// notify() sends a notification through the session bus; it starts the GLib
// thread when needed. Callbacks run in the FLTK thread, so call Fl::lock()
// once in main() before using them.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fld {

struct Notification {
  std::string app_name;     // default: the program's app id
  std::string summary, body;
  std::string icon;         // theme icon name or absolute path
  std::vector<std::pair<std::string, std::string>> actions; // key, label ("default" = click)
  int urgency = 1;          // 0 low, 1 normal, 2 critical
  int timeout = -1;         // ms; -1 = server default, 0 = never expires
  std::string category;     // e.g. "device.added", "transfer.complete"
  std::string desktop_entry;
  bool transient = false;   // not kept in the history
  uint32_t replaces_id = 0;
};

// Sends n. on_id gets the id the server assigned (0 when no server answered);
// on_action gets the key of an invoked action.
void notify(const Notification &n, std::function<void(const std::string &action)> on_action = nullptr,
            std::function<void(uint32_t id)> on_id = nullptr);
void notify_close(uint32_t id);

// ---- fl-notifyd's history (org.fldesktop.Notifications) -------------------

struct HistoryEntry {
  uint32_t id = 0;
  std::string app, summary, body, icon;
  int64_t time = 0; // unix seconds
};

class NotifyHistory {
public:
  static NotifyHistory &get();
  // Watches the daemon; on_changed runs (FLTK thread) when the history,
  // do-not-disturb or the daemon itself changes.
  void start();
  std::function<void()> on_changed;
  // Fetches the history (newest first) and the do-not-disturb state.
  void fetch(std::function<void(const std::vector<HistoryEntry> &, bool dnd, bool available)> cb);
  void clear();
  void remove(uint32_t id);
  void set_dnd(bool on);
private:
  NotifyHistory() = default;
};

// Helpers shared with fl-notifyd and the tests.
// Removes the simple markup the spec allows (<b>, <i>, <u>, <a href>, <img>)
// and decodes the XML entities.
std::string notify_strip_markup(const std::string &s);
// Converts the spec's "image-data" pixels (rows of rowstride bytes, 3 or 4
// channels of 8 bits) to tightly packed RGBA. Returns false if the sizes do
// not add up.
bool notify_image_to_rgba(int width, int height, int rowstride, bool has_alpha, int bits, int channels,
                          const unsigned char *data, size_t len, std::vector<unsigned char> &rgba);

} // namespace fld
