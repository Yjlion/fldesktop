add_library(fl-snip-util STATIC snip_util.cpp)   # testable logic, linked by tests
target_include_directories(fl-snip-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT TARGET fld-glib)
  message(WARNING "GLib not found: fl-snip is not built")
  return()
endif()
add_executable(fl-snip main.cpp)
target_link_libraries(fl-snip PRIVATE fl-snip-util fld fld-glib)
install(TARGETS fl-snip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
