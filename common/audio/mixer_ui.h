// FLTK views for the audio backend, shared by fl-mixer and the Sound page of
// fl-settings.
#pragma once
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <map>

#include "audiobackend.h"

namespace fld {

class ChannelRow;

class MixerView : public Fl_Group {
public:
  enum Mode { PLAYBACK, RECORDING, OUTPUTS, INPUTS, CARDS };
  MixerView(int x, int y, int w, int h, AudioBackend *backend, Mode mode);
  ~MixerView() override;
  // re-reads the backend model (call from the backend's on_changed)
  void refresh();
  void resize(int x, int y, int w, int h) override;
  void draw() override;
  Mode mode() const { return mode_; }
  AudioBackend *backend() const { return backend_; }

private:
  AudioBackend *backend_;
  Mode mode_;
  Fl_Scroll *scroll_;
  std::map<uint32_t, ChannelRow *> rows_;
  void layout_rows();
  static void meter_tick(void *data);
};

} // namespace fld
