pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)
add_executable(fl-term main.cpp term_widget.cpp)
target_link_libraries(fl-term PRIVATE fld PkgConfig::VTERM util)
install(TARGETS fl-term RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
