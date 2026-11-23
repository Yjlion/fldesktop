# the Secret Service (passwords): fl-keyring, fl-web
pkg_check_modules(SECRET_GIO QUIET IMPORTED_TARGET gio-2.0)
if(SECRET_GIO_FOUND)
  add_library(fld-secret STATIC secret.cpp)
  target_include_directories(fld-secret PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(fld-secret PUBLIC fld PkgConfig::SECRET_GIO)
endif()
