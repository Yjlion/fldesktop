# fl-remote: VNC (its own RFB client) and RDP (through FreeRDP) connections
add_library(fl-remote-util STATIC remote_util.cpp rfb.cpp)
target_include_directories(fl-remote-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fl-remote-util PUBLIC fld)
add_executable(fl-remote main.cpp)
target_link_libraries(fl-remote PRIVATE fl-remote-util fld)
if(TARGET fld-mdns) # computers on the network that offer a remote desktop
  target_link_libraries(fl-remote PRIVATE fld-mdns)
  target_compile_definitions(fl-remote PRIVATE FLD_HAVE_MDNS=1)
endif()
install(TARGETS fl-remote RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
