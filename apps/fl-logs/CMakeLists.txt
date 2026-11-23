# fl-logs: the systemd journal and the syslog / OpenRC log files
add_library(fl-logs-util STATIC logs_util.cpp)
target_include_directories(fl-logs-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fl-logs-util PUBLIC fld)
add_executable(fl-logs main.cpp)
target_link_libraries(fl-logs PRIVATE fl-logs-util fld)
install(TARGETS fl-logs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
