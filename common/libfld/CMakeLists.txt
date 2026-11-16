add_library(fld STATIC
  util.cpp
  ini.cpp
  desktop_entry.cpp
  icons.cpp
  mime.cpp
  theme.cpp
  json.cpp
  trash.cpp
  widgets.cpp
  config_watch.cpp
)
target_include_directories(fld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fld PUBLIC fltk::images fltk::fltk)

# GLib main loop bridge (only for programs using GObject libraries)
pkg_check_modules(FLD_GLIB IMPORTED_TARGET glib-2.0)
if(FLD_GLIB_FOUND)
  add_library(fld-glib STATIC glib_bridge.cpp)
  target_link_libraries(fld-glib PUBLIC fld PkgConfig::FLD_GLIB)
endif()
