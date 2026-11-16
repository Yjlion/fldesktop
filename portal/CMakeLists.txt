# xdg-desktop-portal-fl: the fldesktop backend of xdg-desktop-portal
if(NOT TARGET fld-glib)
  message(STATUS "glib/gio not found: not building xdg-desktop-portal-fl")
  return()
endif()

add_library(fld-portal-util STATIC portal_util.cpp)
target_include_directories(fld-portal-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fld-portal-util PUBLIC fld-glib)

add_executable(xdg-desktop-portal-fl main.cpp filechooser.cpp appchooser.cpp settings.cpp dialogs.cpp
               notification.cpp)
target_link_libraries(xdg-desktop-portal-fl PRIVATE fld-portal-util)
install(TARGETS xdg-desktop-portal-fl RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

set(FLD_PORTAL_EXEC ${CMAKE_INSTALL_FULL_LIBEXECDIR}/xdg-desktop-portal-fl)
configure_file(${CMAKE_SOURCE_DIR}/data/portal/org.freedesktop.impl.portal.desktop.fl.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.fl.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.fl.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${CMAKE_SOURCE_DIR}/data/portal/fl.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES ${CMAKE_SOURCE_DIR}/data/portal/fldesktop-portals.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
