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
add_executable(fl-web main.cpp webview.cpp glib_loop.cpp ${_backend})
target_link_libraries(fl-web PRIVATE fl-web-util fld-glib ${_libs})
install(TARGETS fl-web RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
