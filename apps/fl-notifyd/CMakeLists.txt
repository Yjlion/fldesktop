if(TARGET fld-glib)
  add_executable(fl-notifyd main.cpp)
  target_link_libraries(fl-notifyd PRIVATE fld-glib)
  install(TARGETS fl-notifyd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  configure_file(${CMAKE_SOURCE_DIR}/data/dbus-1/services/org.fldesktop.Notifyd.service.in
                 ${CMAKE_CURRENT_BINARY_DIR}/org.fldesktop.Notifyd.service @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.fldesktop.Notifyd.service
          DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
  install(FILES ${CMAKE_SOURCE_DIR}/data/autostart/fl-notifyd.desktop
          DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
else()
  message(STATUS "glib/gio not found: not building fl-notifyd")
endif()
