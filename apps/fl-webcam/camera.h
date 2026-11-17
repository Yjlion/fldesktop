// fl-webcam: cameras (GstDeviceMonitor) and the capture pipeline
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "cam_util.h"
#include "frame_view.h"

typedef struct _GstElement GstElement;
typedef struct _GstDevice GstDevice;
typedef struct _GstMessage GstMessage;

struct CameraInfo {
  std::string name;   // shown to the user
  std::string id;     // device path or PipeWire serial, for remembering the choice
  GstDevice *device = nullptr; // nullptr: the test pattern
  std::vector<cam::Mode> modes;
};

class Camera {
public:
  Camera();
  ~Camera();
  fgst::FrameSlot &frames() { return slot_; }
  GstElement *video_sink() const { return vsink_; }

  // Finds cameras. With FLD_WEBCAM_SOURCE=test (or none found and
  // `test_if_none`) a test pattern is listed.
  std::vector<CameraInfo> &probe(bool test_if_none);
  std::vector<CameraInfo> &cameras() { return cams_; }

  bool start(size_t camera, size_t mode, std::string &err);
  void stop();
  bool running() const { return pipeline_ != nullptr; }

  bool take_photo(const std::string &path, std::string &err);

  bool start_recording(const std::string &path, bool audio, std::string &err);
  void stop_recording(); // finishes the file asynchronously: on_recorded
  bool recording() const { return rec_ != nullptr; }
  double recording_time() const;

  std::function<void(const std::string &path, const std::string &err)> on_recorded;
  std::function<void(const std::string &)> on_error;

private:
  fgst::FrameSlot slot_;
  GstElement *pipeline_ = nullptr, *vsink_ = nullptr, *tee_ = nullptr;
  GstElement *rec_ = nullptr, *rec_asrc_ = nullptr;
  std::string rec_path_;
  double rec_start_ = 0;
  bool rec_stopping_ = false;
  std::vector<CameraInfo> cams_;
  bool test_audio_ = false;

  void handle(GstMessage *m);
  void finish_recording(const std::string &err);
  static void poll_cb(void *);
  void clear_cams();
};
