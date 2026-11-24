# fl-media-player (fl-video before 0.1.4): video and music on GStreamer
set(MEDIA_PROTO)
fld_wayland_protocol(MEDIA_PROTO ${WAYLAND_PROTOCOLS_DIR}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml client)
add_library(fl-media-player-util STATIC video_util.cpp playlist_util.cpp)
target_include_directories(fl-media-player-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fl-media-player-util PUBLIC fld)
add_executable(fl-media-player main.cpp gst_player.cpp idle_inhibit.cpp ${MEDIA_PROTO})
target_include_directories(fl-media-player PRIVATE ${FLD_PROTO_OUT})
target_link_libraries(fl-media-player PRIVATE fl-media-player-util fld-gst fld PkgConfig::WAYLAND_CLIENT)
# MPRIS: the panel and the media keys control it
pkg_check_modules(MEDIA_GIO_UNIX QUIET IMPORTED_TARGET gio-unix-2.0)
if(TARGET fld-glib AND MEDIA_GIO_UNIX_FOUND)
  target_sources(fl-media-player PRIVATE mpris_server.cpp)
  target_link_libraries(fl-media-player PRIVATE fld-glib PkgConfig::MEDIA_GIO_UNIX)
  target_compile_definitions(fl-media-player PRIVATE FLD_HAVE_MPRIS=1)
endif()
install(TARGETS fl-media-player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# builds GStreamer's plugin registry after login, before the first use
install(FILES ${CMAKE_SOURCE_DIR}/data/autostart/fl-media-player-warmup.desktop
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
# the old name still works
add_custom_command(TARGET fl-media-player POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink fl-media-player $<TARGET_FILE_DIR:fl-media-player>/fl-video)
install(CODE "file(CREATE_LINK fl-media-player \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/fl-video\" SYMBOLIC)")
