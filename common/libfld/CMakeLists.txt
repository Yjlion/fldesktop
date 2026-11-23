add_library(fld STATIC
  util.cpp
  accents.cpp
  ini.cpp
  desktop_entry.cpp
  icons.cpp
  mime.cpp
  styles.cpp
  theme.cpp
  json.cpp
  trash.cpp
  widgets.cpp
  pickers.cpp
  config_watch.cpp
  osk.cpp
  file_dialog.cpp
  fileops.cpp
  fileops_ui.cpp
  menus.cpp
  session.cpp
  tab_bar.cpp
  drag.cpp
  tab_move.cpp
  atspi_model.cpp
)
target_include_directories(fld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
pkg_check_modules(FLD_CAIRO REQUIRED IMPORTED_TARGET cairo)
target_link_libraries(fld PUBLIC fltk::images fltk::fltk Threads::Threads PkgConfig::FLD_CAIRO PkgConfig::WAYLAND_CLIENT)

# screen readers: an AT-SPI2 bridge over GDBus (GLib comes with Pango anyway)
pkg_check_modules(FLD_ATSPI_GIO IMPORTED_TARGET gio-2.0)
if(FLD_ATSPI_GIO_FOUND)
  target_sources(fld PRIVATE atspi.cpp)
  target_compile_definitions(fld PRIVATE FLD_HAVE_ATSPI=1)
  target_link_libraries(fld PUBLIC PkgConfig::FLD_ATSPI_GIO)
endif()

# GLib main loop bridge (only for programs using GObject libraries)
pkg_check_modules(FLD_GLIB IMPORTED_TARGET glib-2.0 gio-2.0)
if(FLD_GLIB_FOUND)
  # GLib bridge plus the D-Bus clients every program may use (notifications)
  add_library(fld-glib STATIC glib_bridge.cpp notify.cpp)
  target_link_libraries(fld-glib PUBLIC fld PkgConfig::FLD_GLIB)
endif()
