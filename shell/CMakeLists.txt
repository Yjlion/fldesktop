set(PANEL_PROTO)
fld_wayland_protocol(PANEL_PROTO ${CMAKE_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml client)

add_executable(fl-panel panel.cpp ${PANEL_PROTO})
target_include_directories(fl-panel PRIVATE ${FLD_PROTO_OUT})
target_link_libraries(fl-panel PRIVATE fld PkgConfig::WAYLAND_CLIENT)

add_executable(fl-launcher launcher.cpp)
target_link_libraries(fl-launcher PRIVATE fld)

add_executable(fl-desktop desktop.cpp)
target_link_libraries(fl-desktop PRIVATE fld)

add_executable(fl-autostart autostart.cpp)
target_link_libraries(fl-autostart PRIVATE fld)

# scripts are copied next to the binaries so a build tree works without installing
foreach(s fl-session fl-screenshot)
  configure_file(${s} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${s} COPYONLY)
endforeach()

install(TARGETS fl-panel fl-launcher fl-desktop fl-autostart RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS fl-session fl-screenshot DESTINATION ${CMAKE_INSTALL_BINDIR})
