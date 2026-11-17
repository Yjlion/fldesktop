add_executable(fl-greet main.cpp greetd_ipc.cpp)
target_link_libraries(fl-greet PRIVATE fld)
install(TARGETS fl-greet RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS fl-greet-session DESTINATION ${CMAKE_INSTALL_BINDIR})
