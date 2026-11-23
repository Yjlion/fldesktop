# fl-welcome: the first-run tour
add_library(fl-welcome-util STATIC welcome_util.cpp)
target_include_directories(fl-welcome-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fl-welcome-util PUBLIC fld)
add_executable(fl-welcome main.cpp)
target_link_libraries(fl-welcome PRIVATE fl-welcome-util fld)
# the keyboard layouts' names (fl-settings' parser of the xkb rules)
if(TARGET fl-settings-xkb)
  target_link_libraries(fl-welcome PRIVATE fl-settings-xkb)
  target_compile_definitions(fl-welcome PRIVATE FLD_HAVE_XKB_RULES=1)
endif()
install(TARGETS fl-welcome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/data/autostart/fl-welcome.desktop
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
