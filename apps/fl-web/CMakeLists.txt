# fl-web needs WPE WebKit: with the WPE Platform API (WebKit >= 2.46 built
# with it, e.g. Arch) or with libwpe and WPEBackend-fdo (e.g. Debian 13).
add_library(fl-web-util STATIC browser_util.cpp)   # testable logic, linked by tests
target_include_directories(fl-web-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fl-web-util PUBLIC fld)

if(FLD_WEB_PLATFORM)
  set(_backend backend_platform.cpp)
  set(_libs PkgConfig::WPE_WEBKIT PkgConfig::WPE_PLATFORM)
else()
  set(_backend backend_fdo.cpp)
  set(_libs PkgConfig::WPE_WEBKIT PkgConfig::WPE_FDO)
endif()
# GPU rendering: WebKit's EGL images are read back with GLES 2
pkg_check_modules(WEB_GL REQUIRED IMPORTED_TARGET egl glesv2)
add_executable(fl-web main.cpp webview.cpp glib_loop.cpp gl_readback.cpp ${_backend})
target_link_libraries(fl-web PRIVATE fl-web-util fld-glib ${_libs} PkgConfig::WEB_GL)
install(TARGETS fl-web RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
