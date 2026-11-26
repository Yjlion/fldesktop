/* fl-panel applet plugins (ABI version 1).
 *
 * An applet is a shared library in ~/.local/lib/fldesktop/applets or
 * <libdir>/fldesktop/applets (or a directory of $FLD_APPLET_PATH) that
 * exports
 *
 *     const struct fld_applet *fld_applet_v1(void);
 *
 * The panel calls create() with its window as the current FLTK group; the
 * applet makes one widget (usually a button) panel-height high, at any x,
 * and returns it as an Fl_Widget*. The panel places it on the right, before
 * the system indicators, and keeps its width. A hidden widget takes no room;
 * call host->relayout() after showing, hiding or resizing it.
 *
 * Applets are C++ code using FLTK: build them against the FLTK 1.4 the panel
 * uses (fltk-config --cxxflags --ldflags). This header is plain C so that
 * the ABI is the structs below and nothing else; the version only changes
 * when they do. See doc/APPLETS.md and tests/applet_example.cpp.
 *
 * [Panel] DisabledApplets in panel.ini lists ids that are not loaded. */
#ifndef FLD_APPLET_H
#define FLD_APPLET_H

#ifdef __cplusplus
extern "C" {
#endif

#define FLD_APPLET_ABI_VERSION 1u

struct fld_applet_host {
  unsigned abi_version;  /* FLD_APPLET_ABI_VERSION of the panel */
  int height;            /* the panel's height in pixels */
  unsigned bg, fg, hover; /* the panel's colours (Fl_Color) */
  /* places the panel's widgets again */
  void (*relayout)(void);
  /* a value of the applet's section [Applet:<id>] in panel.ini; def when
   * there is none. The string stays valid until the next call. */
  const char *(*config)(const char *applet_id, const char *key, const char *def);
  /* starts a program (argv ends with NULL), without a shell */
  void (*spawn)(const char *const *argv);
};

struct fld_applet {
  unsigned abi_version;  /* FLD_APPLET_ABI_VERSION the applet was built for */
  const char *id;        /* unique, [a-z0-9-]: "uptime" */
  const char *name;      /* shown to people: "Uptime" */
  const char *description;
  /* makes the widget in the current group; NULL when it cannot run */
  void *(*create)(const struct fld_applet_host *host);
  /* optional: called before the panel deletes the widget */
  void (*destroy)(void *widget);
};

typedef const struct fld_applet *(*fld_applet_entry_fn)(void);
#define FLD_APPLET_ENTRY "fld_applet_v1"

#ifdef __cplusplus
}
#endif
#endif
