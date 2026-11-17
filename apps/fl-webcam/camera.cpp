#include "camera.h"

#include <FL/Fl.H>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <cmath>
#include <cstring>

#include "fld/util.h"

static bool want_test_source() {
  const char *s = getenv("FLD_WEBCAM_SOURCE");
  return s && !strcmp(s, "test");
}

Camera::Camera() { Fl::add_timeout(0.05, poll_cb, this); }

Camera::~Camera() {
  Fl::remove_timeout(poll_cb, this);
  stop();
  clear_cams();
}

void Camera::clear_cams() {
  for (auto &c : cams_)
    if (c.device) gst_object_unref(c.device);
  cams_.clear();
}

// ---- devices ----------------------------------------------------------------------------

static void add_modes(GstCaps *caps, std::vector<cam::Mode> &modes) {
  for (guint i = 0; caps && i < gst_caps_get_size(caps); i++) {
    GstStructure *s = gst_caps_get_structure(caps, i);
    const char *name = gst_structure_get_name(s);
    std::string fmt = !strcmp(name, "video/x-raw") ? "raw" : !strcmp(name, "image/jpeg") ? "jpeg" : "";
    if (fmt.empty()) continue;
    int w = 0, h = 0;
    if (!gst_structure_get_int(s, "width", &w) || !gst_structure_get_int(s, "height", &h)) continue;
    double fps = 0;
    const GValue *fr = gst_structure_get_value(s, "framerate");
    auto take = [&](const GValue *v) {
      if (v && GST_VALUE_HOLDS_FRACTION(v) && gst_value_get_fraction_denominator(v) > 0)
        fps = std::max(fps, (double)gst_value_get_fraction_numerator(v) / gst_value_get_fraction_denominator(v));
    };
    if (fr && GST_VALUE_HOLDS_LIST(fr)) {
      for (guint k = 0; k < gst_value_list_get_size(fr); k++) take(gst_value_list_get_value(fr, k));
    } else if (fr && GST_VALUE_HOLDS_FRACTION_RANGE(fr)) {
      take(gst_value_get_fraction_range_max(fr));
    } else {
      take(fr);
    }
    cam::Mode m;
    m.w = w;
    m.h = h;
    m.fps = fps;
    m.format = fmt;
    modes.push_back(m);
  }
}

std::vector<CameraInfo> &Camera::probe(bool test_if_none) {
  clear_cams();
  if (!want_test_source()) {
    GstDeviceMonitor *mon = gst_device_monitor_new();
    gst_device_monitor_add_filter(mon, "Video/Source", nullptr);
    if (gst_device_monitor_start(mon)) {
      GList *devs = gst_device_monitor_get_devices(mon);
      for (GList *l = devs; l; l = l->next) {
        GstDevice *d = GST_DEVICE(l->data);
        CameraInfo c;
        gchar *n = gst_device_get_display_name(d);
        c.name = n ? n : "Camera";
        g_free(n);
        GstStructure *props = gst_device_get_properties(d);
        if (props) {
          const char *path = gst_structure_get_string(props, "device.path");
          if (!path) path = gst_structure_get_string(props, "api.v4l2.path");
          if (!path) path = gst_structure_get_string(props, "object.path");
          c.id = path ? path : c.name;
          gst_structure_free(props);
        }
        GstCaps *caps = gst_device_get_caps(d);
        add_modes(caps, c.modes);
        if (caps) gst_caps_unref(caps);
        c.modes = cam::best_modes(c.modes);
        // the same camera through PipeWire and v4l2: keep the first
        bool dup = false;
        for (auto &o : cams_) dup |= o.name == c.name;
        if (dup || c.modes.empty()) continue;
        c.device = GST_DEVICE(gst_object_ref(d));
        cams_.push_back(c);
      }
      g_list_free_full(devs, gst_object_unref);
      gst_device_monitor_stop(mon);
    }
    gst_object_unref(mon);
  }
  if (cams_.empty() && (test_if_none || want_test_source())) {
    CameraInfo c;
    c.name = "Test pattern";
    c.id = "test";
    for (auto wh : {std::make_pair(1280, 720), std::make_pair(640, 480)}) {
      cam::Mode m;
      m.w = wh.first;
      m.h = wh.second;
      m.fps = 30;
      m.format = "raw";
      c.modes.push_back(m);
    }
    cams_.push_back(c);
  }
  return cams_;
}

// ---- pipeline ---------------------------------------------------------------------------

bool Camera::start(size_t ci, size_t mi, std::string &err) {
  stop();
  if (ci >= cams_.size() || mi >= cams_[ci].modes.size()) {
    err = "No camera";
    return false;
  }
  const CameraInfo &c = cams_[ci];
  const cam::Mode &m = c.modes[mi];
  GstElement *src = c.device ? gst_device_create_element(c.device, "source")
                             : gst_element_factory_make("videotestsrc", "source");
  if (!src) {
    err = "Could not open " + c.name;
    return false;
  }
  test_audio_ = !c.device;
  if (!c.device) g_object_set(src, "is-live", TRUE, "pattern", 18 /* ball */, NULL);
  pipeline_ = gst_pipeline_new("webcam");
  GstElement *filter = gst_element_factory_make("capsfilter", "camcaps");
  GstElement *dec = m.format == "jpeg" ? gst_element_factory_make("jpegdec", "decoder") : nullptr;
  GstElement *conv = gst_element_factory_make("videoconvert", "camconvert");
  tee_ = gst_element_factory_make("tee", "tee");
  GstElement *q = gst_element_factory_make("queue", "previewqueue");
  vsink_ = fgst::make_sink(&slot_, "preview");
  if (!filter || !conv || !tee_ || !q || !vsink_ || (m.format == "jpeg" && !dec)) {
    err = m.format == "jpeg" && !dec ? "The JPEG decoder (GStreamer good plugins) is missing"
                                     : "GStreamer base elements are missing";
    if (pipeline_) gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return false;
  }
  char capsstr[160];
  snprintf(capsstr, sizeof capsstr, "%s,width=%d,height=%d", m.format == "jpeg" ? "image/jpeg" : "video/x-raw", m.w,
           m.h);
  GstCaps *caps = gst_caps_from_string(capsstr);
  if (m.fps > 0) gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, (int)std::lround(m.fps), 1, NULL);
  g_object_set(filter, "caps", caps, NULL);
  gst_caps_unref(caps);
  // the preview drops frames rather than holding up the camera
  g_object_set(q, "leaky", 2 /* downstream */, "max-size-buffers", 2, NULL);
  g_object_set(tee_, "allow-not-linked", TRUE, NULL);
  gst_bin_add_many(GST_BIN(pipeline_), src, filter, conv, tee_, q, vsink_, NULL);
  bool linked;
  if (dec) {
    gst_bin_add(GST_BIN(pipeline_), dec);
    linked = gst_element_link_many(src, filter, dec, conv, tee_, NULL);
  } else {
    linked = gst_element_link_many(src, filter, conv, tee_, NULL);
  }
  linked = linked && gst_element_link_many(tee_, q, vsink_, NULL);
  if (!linked) {
    err = "The camera does not offer " + cam::mode_label(m);
    stop();
    return false;
  }
  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    err = "The camera could not be started (is another program using it?)";
    stop();
    return false;
  }
  return true;
}

void Camera::stop() {
  if (rec_) finish_recording("The camera was stopped");
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
  }
  pipeline_ = nullptr;
  vsink_ = tee_ = nullptr;
  slot_.clear();
}

bool Camera::take_photo(const std::string &path, std::string &err) {
  if (!vsink_) { err = "The camera is not running"; return false; }
  GstElement *appsink = gst_bin_get_by_name(GST_BIN(vsink_), "appsink");
  GstSample *sample = nullptr;
  if (appsink) {
    g_object_get(appsink, "last-sample", &sample, NULL);
    gst_object_unref(appsink);
  }
  if (!sample) { err = "No picture from the camera yet"; return false; }
  bool png = fld::ends_with(fld::lower(path), ".png");
  GstCaps *caps = gst_caps_from_string(png ? "image/png" : "image/jpeg");
  GError *e = nullptr;
  GstSample *out = gst_video_convert_sample(sample, caps, 5 * GST_SECOND, &e);
  gst_caps_unref(caps);
  gst_sample_unref(sample);
  if (!out) {
    err = e ? e->message : "Could not encode the picture";
    if (e) g_error_free(e);
    return false;
  }
  GstBuffer *buf = gst_sample_get_buffer(out);
  GstMapInfo map;
  bool ok = false;
  if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
    ok = fld::write_file(path, std::string((const char *)map.data, map.size));
    gst_buffer_unmap(buf, &map);
  }
  gst_sample_unref(out);
  if (!ok) err = "Could not write " + path;
  return ok;
}

// ---- recording --------------------------------------------------------------------------

// The recording branch is a bin of its own, hanging off the tee:
//   queue ! videoconvert ! vp8enc ! webmmux ! filesink, audio source ! ... ! opusenc ! mux
// Stopping sends EOS into it; the bin forwards the filesink's EOS so we know
// when the file is complete.
bool Camera::start_recording(const std::string &path, bool audio, std::string &err) {
  if (!pipeline_ || rec_) { err = pipeline_ ? "Already recording" : "The camera is not running"; return false; }
  for (const char *f : {"vp8enc", "webmmux"})
    if (!fgst::have_element(f)) {
      err = std::string("The GStreamer element \"") + f + "\" is missing (install the GStreamer good plugins)";
      return false;
    }
  if (audio && (!fgst::have_element("opusenc") || !fgst::have_element("audioconvert"))) audio = false;
  GError *e = nullptr;
  std::string desc =
      "queue name=recq max-size-time=3000000000 ! videoconvert ! "
      "vp8enc deadline=1 cpu-used=8 threads=4 target-bitrate=4000000 keyframe-max-dist=60 ! queue ! "
      "webmmux name=mux ! filesink name=file";
  if (audio) {
    const char *asrc = test_audio_ ? "audiotestsrc is-live=true wave=ticks volume=0.2"
                       : fgst::have_element("pipewiresrc") ? "pipewiresrc"
                       : fgst::have_element("pulsesrc")    ? "pulsesrc"
                                                           : "autoaudiosrc";
    desc += std::string(" ") + asrc + " name=asrc ! queue ! audioconvert ! audioresample ! "
            "opusenc ! queue ! mux.";
  }
  rec_ = gst_parse_bin_from_description(desc.c_str(), FALSE, &e);
  if (!rec_) {
    err = e ? e->message : "Could not set up the recording";
    if (e) g_error_free(e);
    return false;
  }
  gst_object_ref_sink(rec_);
  GstElement *file = gst_bin_get_by_name(GST_BIN(rec_), "file");
  g_object_set(file, "location", path.c_str(), NULL);
  gst_object_unref(file);
  rec_asrc_ = audio ? gst_bin_get_by_name(GST_BIN(rec_), "asrc") : nullptr;
  GstElement *q = gst_bin_get_by_name(GST_BIN(rec_), "recq");
  GstPad *qpad = gst_element_get_static_pad(q, "sink");
  gst_element_add_pad(rec_, gst_ghost_pad_new("sink", qpad));
  gst_object_unref(qpad);
  gst_object_unref(q);
  g_object_set(rec_, "message-forward", TRUE, NULL);
  gst_bin_add(GST_BIN(pipeline_), rec_);
  GstPad *teepad = gst_element_request_pad_simple(tee_, "src_%u");
  GstPad *sink = gst_element_get_static_pad(rec_, "sink");
  bool ok = gst_pad_link(teepad, sink) == GST_PAD_LINK_OK;
  gst_object_unref(sink);
  gst_object_unref(teepad);
  if (!ok || !gst_element_sync_state_with_parent(rec_)) {
    err = "Could not start the recording";
    finish_recording("");
    return false;
  }
  rec_path_ = path;
  rec_start_ = fgst::now();
  rec_stopping_ = false;
  return true;
}

double Camera::recording_time() const { return rec_ ? fgst::now() - rec_start_ : 0; }

void Camera::stop_recording() {
  if (!rec_ || rec_stopping_) return;
  rec_stopping_ = true;
  // unlink from the tee, then end the streams of the branch
  GstPad *sink = gst_element_get_static_pad(rec_, "sink");
  GstPad *teepad = gst_pad_get_peer(sink);
  if (teepad) {
    gst_pad_unlink(teepad, sink);
    gst_element_release_request_pad(tee_, teepad);
    gst_object_unref(teepad);
  }
  gst_pad_send_event(sink, gst_event_new_eos());
  gst_object_unref(sink);
  if (rec_asrc_) gst_element_send_event(rec_asrc_, gst_event_new_eos());
  // safety net: give up after 5 s
  Fl::add_timeout(5.0, [](void *d) {
    Camera *c = static_cast<Camera *>(d);
    if (c->rec_ && c->rec_stopping_) c->finish_recording("");
  }, this);
}

void Camera::finish_recording(const std::string &err) {
  if (!rec_) return;
  GstElement *r = rec_;
  rec_ = nullptr;
  if (rec_asrc_) gst_object_unref(rec_asrc_);
  rec_asrc_ = nullptr;
  gst_element_set_state(r, GST_STATE_NULL);
  if (pipeline_) {
    // still linked when the start failed
    GstPad *sink = gst_element_get_static_pad(r, "sink");
    if (GstPad *teepad = gst_pad_get_peer(sink)) {
      gst_pad_unlink(teepad, sink);
      gst_element_release_request_pad(tee_, teepad);
      gst_object_unref(teepad);
    }
    gst_object_unref(sink);
    gst_bin_remove(GST_BIN(pipeline_), r);
  }
  gst_object_unref(r);
  std::string path = rec_path_;
  rec_path_.clear();
  rec_stopping_ = false;
  if (on_recorded && !path.empty()) on_recorded(path, err);
}

void Camera::handle(GstMessage *m) {
  switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_ELEMENT: {
      // EOS forwarded from the recording bin: the file is complete
      const GstStructure *s = gst_message_get_structure(m);
      if (rec_ && s && gst_structure_has_name(s, "GstBinForwarded")) {
        GstMessage *inner = nullptr;
        gst_structure_get(s, "message", GST_TYPE_MESSAGE, &inner, NULL);
        if (inner && GST_MESSAGE_TYPE(inner) == GST_MESSAGE_EOS && rec_stopping_) finish_recording("");
        if (inner) gst_message_unref(inner);
      }
      break;
    }
    case GST_MESSAGE_ERROR: {
      GError *e = nullptr;
      gchar *dbg = nullptr;
      gst_message_parse_error(m, &e, &dbg);
      std::string msg = e ? e->message : "camera error";
      if (getenv("FLD_WEBCAM_DEBUG") && dbg) fprintf(stderr, "fl-webcam: %s\n%s\n", msg.c_str(), dbg);
      if (e) g_error_free(e);
      g_free(dbg);
      // an error inside the recording branch ends the recording only
      if (rec_ && GST_MESSAGE_SRC(m) && gst_object_has_as_ancestor(GST_MESSAGE_SRC(m), GST_OBJECT(rec_))) {
        finish_recording(msg);
      } else if (on_error) {
        on_error(msg);
      }
      break;
    }
    default:
      break;
  }
}

void Camera::poll_cb(void *d) {
  Camera *c = static_cast<Camera *>(d);
  if (c->pipeline_) {
    GstBus *bus = gst_element_get_bus(c->pipeline_);
    while (c->pipeline_) {
      GstMessage *m = gst_bus_pop(bus);
      if (!m) break;
      c->handle(m);
      gst_message_unref(m);
    }
    gst_object_unref(bus);
  }
  Fl::repeat_timeout(0.05, poll_cb, d);
}
