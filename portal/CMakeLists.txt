# xdg-desktop-portal-fl: the fldesktop backend of xdg-desktop-portal
if(NOT TARGET fld-glib)
  message(STATUS "glib/gio not found: not building xdg-desktop-portal-fl")
  return()
endif()

add_library(fld-portal-util STATIC portal_util.cpp cast_util.cpp)
target_include_directories(fld-portal-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fld-portal-util PUBLIC fld-glib)

add_executable(xdg-desktop-portal-fl main.cpp filechooser.cpp appchooser.cpp settings.cpp dialogs.cpp
               notification.cpp)
target_link_libraries(xdg-desktop-portal-fl PRIVATE fld-portal-util)

# ScreenCast: Wayland capture into PipeWire
pkg_check_modules(PORTAL_PW IMPORTED_TARGET libpipewire-0.3)
if(PORTAL_PW_FOUND)
  set(CAST_PROTO)
  fld_wayland_protocol(CAST_PROTO ${CMAKE_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml client)
  set(_cast_ext TRUE)
  foreach(x ext-image-capture-source/ext-image-capture-source-v1 ext-image-copy-capture/ext-image-copy-capture-v1
            ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1)
    if(NOT EXISTS ${WAYLAND_PROTOCOLS_DIR}/staging/${x}.xml)
      set(_cast_ext FALSE)
    endif()
  endforeach()
  if(_cast_ext)
    foreach(x ext-image-capture-source/ext-image-capture-source-v1 ext-image-copy-capture/ext-image-copy-capture-v1
              ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1)
      fld_wayland_protocol(CAST_PROTO ${WAYLAND_PROTOCOLS_DIR}/staging/${x}.xml client)
    endforeach()
    target_compile_definitions(xdg-desktop-portal-fl PRIVATE FLCAST_HAVE_EXT=1)
  else()
    message(STATUS "xdg-desktop-portal-fl: wayland-protocols without ext-image-copy-capture, screens only")
  endif()
  target_sources(xdg-desktop-portal-fl PRIVATE screencast.cpp cast_capture.c ${CAST_PROTO})
  target_include_directories(xdg-desktop-portal-fl PRIVATE ${FLD_PROTO_OUT})
  set_source_files_properties(cast_capture.c PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers;-Wno-pedantic")
  target_link_libraries(xdg-desktop-portal-fl PRIVATE PkgConfig::PORTAL_PW PkgConfig::WAYLAND_CLIENT)
  target_compile_definitions(xdg-desktop-portal-fl PRIVATE FLD_HAVE_SCREENCAST=1)
  message(STATUS "xdg-desktop-portal-fl: ScreenCast through PipeWire")
else()
  # fl.portal lists ScreenCast: without it xdg-desktop-portal would ask us
  message(WARNING "libpipewire-0.3 not found: xdg-desktop-portal-fl without ScreenCast")
endif()
install(TARGETS xdg-desktop-portal-fl RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

set(FLD_PORTAL_EXEC ${CMAKE_INSTALL_FULL_LIBEXECDIR}/xdg-desktop-portal-fl)
configure_file(${CMAKE_SOURCE_DIR}/data/portal/org.freedesktop.impl.portal.desktop.fl.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.fl.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.fl.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${CMAKE_SOURCE_DIR}/data/portal/fl.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES ${CMAKE_SOURCE_DIR}/data/portal/fldesktop-portals.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
