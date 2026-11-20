#include "gl_readback.h"

#include <FL/Fl.H>
#include <FL/platform.H>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <glib.h>

#include <cstdlib>
#include <cstring>

namespace web {
namespace {

bool has_ext(const char *list, const char *name) {
  if (!list) return false;
  size_t n = strlen(name);
  for (const char *p = list; (p = strstr(p, name)); p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
  return false;
}

bool software_requested() {
  const char *e = getenv("FL_WEB_SOFTWARE");
  return e && *e && strcmp(e, "0") != 0;
}

// Makes a context current and restores the previous one when done.
struct CurrentContext {
  EGLDisplay old_dpy = eglGetCurrentDisplay();
  EGLContext old_ctx = eglGetCurrentContext();
  EGLSurface old_draw = eglGetCurrentSurface(EGL_DRAW), old_read = eglGetCurrentSurface(EGL_READ);
  bool ok;
  CurrentContext(EGLDisplay dpy, EGLSurface s, EGLContext ctx) { ok = eglMakeCurrent(dpy, s, s, ctx); }
  ~CurrentContext() {
    if (old_ctx != EGL_NO_CONTEXT) eglMakeCurrent(old_dpy, old_draw, old_read, old_ctx);
    else eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
};

DrmNodes g_nodes;

DrmNodes query_nodes(EGLDisplay dpy) {
  DrmNodes n;
  const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!has_ext(client, "EGL_EXT_device_query") && !has_ext(client, "EGL_EXT_device_base")) return n;
  auto query_display = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
  auto query_string = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
  if (!query_display || !query_string) return n;
  EGLAttrib dev = 0;
  if (!query_display(dpy, EGL_DEVICE_EXT, &dev) || !dev) return n;
  EGLDeviceEXT device = (EGLDeviceEXT)dev;
  const char *ext = query_string(device, EGL_EXTENSIONS);
  if (has_ext(ext, "EGL_EXT_device_drm"))
    if (const char *s = query_string(device, EGL_DRM_DEVICE_FILE_EXT)) n.primary = s;
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
  if (has_ext(ext, "EGL_EXT_device_drm_render_node"))
    if (const char *s = query_string(device, EGL_DRM_RENDER_NODE_FILE_EXT)) n.render = s;
#endif
  return n;
}

EGLDisplay open_display() {
  if (software_requested()) return EGL_NO_DISPLAY;
  fl_open_display();
  wl_display *wl = fl_wl_display();
  if (!wl) return EGL_NO_DISPLAY;
  auto get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = get_platform_display ? get_platform_display(EGL_PLATFORM_WAYLAND_EXT, wl, nullptr)
                                        : eglGetDisplay((EGLNativeDisplayType)wl);
  if (dpy == EGL_NO_DISPLAY) return EGL_NO_DISPLAY;
  if (!eglInitialize(dpy, nullptr, nullptr)) return EGL_NO_DISPLAY;
  // a software renderer has no DRM render node: it would be no faster than
  // WebKit's own shared memory path
  g_nodes = query_nodes(dpy);
  const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
  if (g_nodes.render.empty() || !has_ext(ext, "EGL_KHR_image_base")) {
    if (g_nodes.render.empty()) g_message("fl-web: no GPU render node, rendering with the CPU");
    eglTerminate(dpy);
    g_nodes = {};
    return EGL_NO_DISPLAY;
  }
  return dpy;
}

EGLDisplay g_dpy = EGL_NO_DISPLAY;
bool g_decided = false;

} // namespace

EGLDisplay gpu_display() {
  if (!g_decided) {
    g_decided = true;
    g_dpy = open_display();
    // the readback needs a context of its own: without one, no GPU path
    if (g_dpy != EGL_NO_DISPLAY && !GLReadback::get().ok()) {
      g_message("fl-web: no GLES 2 context for EGL images, rendering with the CPU");
      g_dpy = EGL_NO_DISPLAY;
      g_nodes = {};
    }
  }
  return g_dpy;
}

DrmNodes gpu_drm_nodes() {
  gpu_display();
  return g_nodes;
}

const char *render_mode() { return gpu_display() != EGL_NO_DISPLAY ? "GPU" : "software"; }

GLReadback &GLReadback::get() {
  // g_dpy is set before the first get() (from gpu_display())
  // never destroyed: at exit the Wayland connection may be gone already
  static GLReadback *r = new GLReadback(g_dpy);
  return *r;
}

GLReadback::~GLReadback() {
  if (!ok()) return;
  {
    CurrentContext cur(dpy_, surface_, ctx_);
    if (cur.ok && fbo_) glDeleteFramebuffers(1, &fbo_);
  }
  if (surface_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surface_);
  eglDestroyContext(dpy_, ctx_);
}

GLReadback::GLReadback(EGLDisplay dpy) {
  if (dpy == EGL_NO_DISPLAY) return;
  dpy_ = dpy;
  const char *ext = eglQueryString(dpy_, EGL_EXTENSIONS);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return;
  EGLConfig config = nullptr;
  bool no_config = has_ext(ext, "EGL_KHR_no_config_context") || has_ext(ext, "EGL_MESA_configless_context");
  bool surfaceless = has_ext(ext, "EGL_KHR_surfaceless_context");
  if (!no_config || !surfaceless) {
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(dpy_, attrs, &config, 1, &n) || n < 1) return;
  }
  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy_, no_config && surfaceless ? EGL_NO_CONFIG_KHR : config, EGL_NO_CONTEXT, ctx_attrs);
  if (ctx == EGL_NO_CONTEXT) return;
  if (!surfaceless) {
    const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface_ = eglCreatePbufferSurface(dpy_, config, pb);
    if (surface_ == EGL_NO_SURFACE) {
      eglDestroyContext(dpy_, ctx);
      return;
    }
  }
  {
    CurrentContext cur(dpy_, surface_, ctx);
    const char *gl_ext = cur.ok ? (const char *)glGetString(GL_EXTENSIONS) : nullptr;
    image_target_ = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!cur.ok || !image_target_ || !has_ext(gl_ext, "GL_OES_EGL_image")) {
      image_target_ = nullptr;
    } else {
      bgra_ = has_ext(gl_ext, "GL_EXT_read_format_bgra");
      glGenFramebuffers(1, &fbo_);
    }
  }
  if (!image_target_) {
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surface_);
    surface_ = EGL_NO_SURFACE;
    eglDestroyContext(dpy_, ctx);
    return;
  }
  ctx_ = ctx;
}

bool GLReadback::read(EGLImageKHR image, int width, int height, Frame &out) {
  if (!ok() || !image || width <= 0 || height <= 0) return false;
  CurrentContext cur(dpy_, surface_, ctx_);
  if (!cur.ok) return false;
  while (glGetError() != GL_NO_ERROR) {}
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  ((PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)image_target_)(GL_TEXTURE_2D, (GLeglImageOES)image);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (ok) {
    // Row 0 of the image is its top (the memory layout of buffers for a
    // compositor), and glReadPixels returns row 0 first: no flip.
    out.width = width;
    out.height = height;
    out.stride = width * 4;
    out.pixels.resize((size_t)out.stride * (size_t)height);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (bgra_) {
      glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, out.pixels.data());
    } else {
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out.pixels.data());
      uint8_t *p = out.pixels.data(), *end = p + out.pixels.size();
      for (; p < end; p += 4) std::swap(p[0], p[2]);
    }
    ok = glGetError() == GL_NO_ERROR;
  }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteTextures(1, &tex);
  return ok;
}

} // namespace web
