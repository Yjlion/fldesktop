find_package(Threads REQUIRED)
add_executable(fl-files main.cpp fileops.cpp)
target_link_libraries(fl-files PRIVATE fld Threads::Threads)
install(TARGETS fl-files RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
