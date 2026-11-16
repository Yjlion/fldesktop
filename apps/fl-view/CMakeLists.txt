add_executable(fl-view main.cpp)
target_link_libraries(fl-view PRIVATE fld)
install(TARGETS fl-view RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
