add_library(fl-snip-util STATIC snip_util.cpp)   # testable logic, linked by tests
target_include_directories(fl-snip-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT TARGET fld-glib)
  message(WARNING "GLib not found: fl-snip is not built")
  return()
endif()
add_executable(fl-snip main.cpp)
target_link_libraries(fl-snip PRIVATE fl-snip-util fld fld-glib)
install(TARGETS fl-snip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# screen recording: GStreamer and the ScreenCast portal
pkg_check_modules(SNIP_GST QUIET IMPORTED_TARGET gstreamer-1.0 gio-unix-2.0)
if(SNIP_GST_FOUND)
  target_sources(fl-snip PRIVATE record.cpp)
  target_link_libraries(fl-snip PRIVATE PkgConfig::SNIP_GST)
  target_compile_definitions(fl-snip PRIVATE FLD_HAVE_RECORD=1)
else()
  message(STATUS "GStreamer not found: fl-snip without screen recording")
endif()
