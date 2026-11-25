// GPU rendering for fl-web: WebKit renders with the GPU into buffers that
// are handed over as EGL images or DMA-BUFs. They go to the compositor as
// they are (wl_present.h); when it cannot take them they are copied back
// into memory here for FLTK to draw. Without a GPU (or with FL_WEB_SOFTWARE=1 in the environment)
// WebKit renders with the CPU into shared memory instead, which on a slow
// machine costs most of a core (Mesa's llvmpipe).
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <string>

#include "backend.h"

namespace web {

// The EGL display for GPU rendering, on FLTK's Wayland connection (opened
// here if needed), or EGL_NO_DISPLAY: FL_WEB_SOFTWARE is set, there is no
// hardware renderer (no DRM render node), or a GLES 2 context with EGL images
// is not available. Decided once.
EGLDisplay gpu_display();

// The DRM nodes of the display's device ("" when unknown).
struct DrmNodes {
  std::string primary, render;
};
DrmNodes gpu_drm_nodes();

// "GPU" or "software", for --version
const char *render_mode();

// Copies EGL images on gpu_display() into Frames (premultiplied BGRA). Used
// on the main thread only; the previous EGL context is restored afterwards.
class GLReadback {
public:
  static GLReadback &get(); // on gpu_display()
  explicit GLReadback(EGLDisplay dpy);
  ~GLReadback();
  GLReadback(const GLReadback &) = delete;
  GLReadback &operator=(const GLReadback &) = delete;
  bool ok() const { return ctx_ != EGL_NO_CONTEXT; }
  bool read(EGLImageKHR image, int width, int height, Frame &out);
  // only `rects` of the image, into out's pixels (already width x height)
  bool read_rects(EGLImageKHR image, const std::vector<Rect> &rects, Frame &out);

private:
  EGLDisplay dpy_ = EGL_NO_DISPLAY;
  EGLContext ctx_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  unsigned fbo_ = 0;
  bool bgra_ = false;
  void *image_target_ = nullptr; // glEGLImageTargetTexture2DOES
  std::vector<uint8_t> scratch_;
};

} // namespace web
