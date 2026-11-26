add_executable(fl-displays main.cpp)
target_link_libraries(fl-displays PRIVATE fld-displays fld)
install(TARGETS fl-displays RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
