add_library(fld STATIC
  util.cpp
  text.cpp
  frame_watch.cpp
  window_size.cpp
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
  avatar.cpp
  toolkit_theme.cpp
)
target_include_directories(fld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
pkg_check_modules(FLD_CAIRO REQUIRED IMPORTED_TARGET cairo)
target_link_libraries(fld PUBLIC fltk::images fltk::fltk Threads::Threads PkgConfig::FLD_CAIRO PkgConfig::WAYLAND_CLIENT)

# the primary selection (middle-click paste) over Wayland, which FLTK lacks.
# Generated here: libfld comes before the top-level protocol helper.
target_sources(fld PRIVATE primary.cpp)
pkg_get_variable(FLD_LIBFLD_WP_DIR wayland-protocols pkgdatadir)
find_program(FLD_LIBFLD_SCANNER wayland-scanner)
if(FLD_LIBFLD_WP_DIR AND FLD_LIBFLD_SCANNER)
  set(_ps_xml ${FLD_LIBFLD_WP_DIR}/unstable/primary-selection/primary-selection-unstable-v1.xml)
  set(_ps_out ${CMAKE_CURRENT_BINARY_DIR}/proto)
  file(MAKE_DIRECTORY ${_ps_out})
  execute_process(COMMAND ${FLD_LIBFLD_SCANNER} client-header ${_ps_xml}
                  ${_ps_out}/primary-selection-unstable-v1-client-protocol.h COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND ${FLD_LIBFLD_SCANNER} private-code ${_ps_xml}
                  ${_ps_out}/primary-selection-unstable-v1-protocol.c COMMAND_ERROR_IS_FATAL ANY)
  target_sources(fld PRIVATE ${_ps_out}/primary-selection-unstable-v1-protocol.c)
  target_include_directories(fld PRIVATE ${_ps_out})
  target_compile_definitions(fld PRIVATE FLD_HAVE_PRIMARY_PROTO=1)
  # fl-wm's own protocol: moving a window from a toolbar or menu bar
  set(_fw_xml ${CMAKE_SOURCE_DIR}/protocols/fldesktop-window-v1.xml)
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fw_xml})
  execute_process(COMMAND ${FLD_LIBFLD_SCANNER} client-header ${_fw_xml}
                  ${_ps_out}/fldesktop-window-v1-client-protocol.h COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND ${FLD_LIBFLD_SCANNER} private-code ${_fw_xml}
                  ${_ps_out}/fldesktop-window-v1-protocol.c COMMAND_ERROR_IS_FATAL ANY)
  target_sources(fld PRIVATE ${_ps_out}/fldesktop-window-v1-protocol.c)
  target_compile_definitions(fld PRIVATE FLD_HAVE_WINDOW_PROTO=1)
endif()
target_sources(fld PRIVATE window_move.cpp)

# screen readers: an AT-SPI2 bridge over GDBus (GLib comes with Pango anyway);
# power actions ask logind over GDBus too
pkg_check_modules(FLD_ATSPI_GIO IMPORTED_TARGET gio-2.0)
if(FLD_ATSPI_GIO_FOUND)
  target_sources(fld PRIVATE atspi.cpp)
  target_compile_definitions(fld PRIVATE FLD_HAVE_ATSPI=1 FLD_HAVE_GIO=1)
  target_link_libraries(fld PUBLIC PkgConfig::FLD_ATSPI_GIO)
endif()

# GLib main loop bridge (only for programs using GObject libraries)
pkg_check_modules(FLD_GLIB IMPORTED_TARGET glib-2.0 gio-2.0)
if(FLD_GLIB_FOUND)
  # GLib bridge plus the D-Bus clients every program may use (notifications)
  add_library(fld-glib STATIC glib_bridge.cpp notify.cpp)
  target_link_libraries(fld-glib PUBLIC fld PkgConfig::FLD_GLIB)
endif()
