set(FLD_SHARE ${CMAKE_INSTALL_DATADIR})

install(FILES wayland-sessions/fldesktop.desktop DESTINATION ${FLD_SHARE}/wayland-sessions)
file(GLOB _apps applications/*.desktop)
install(FILES ${_apps} DESTINATION ${FLD_SHARE}/applications)
install(DIRECTORY icons/hicolor DESTINATION ${FLD_SHARE}/icons)
install(FILES backgrounds/default.svg DESTINATION ${FLD_SHARE}/fldesktop/backgrounds)
install(FILES charmap/Blocks.txt charmap/LICENSE.unicode DESTINATION ${FLD_SHARE}/fldesktop/charmap)
install(FILES wm.ini.example greetd/config.toml DESTINATION ${FLD_SHARE}/fldesktop)

# Mirror the data into the build tree so programs can run uninstalled:
#   XDG_DATA_DIRS=build/share:/usr/share FLD_DATADIR=build/share/fldesktop
set(_out ${CMAKE_BINARY_DIR}/share)
file(MAKE_DIRECTORY ${_out}/applications ${_out}/fldesktop/backgrounds ${_out}/wayland-sessions)
file(COPY ${_apps} DESTINATION ${_out}/applications)
file(COPY icons/hicolor DESTINATION ${_out}/icons)
file(COPY backgrounds/default.svg DESTINATION ${_out}/fldesktop/backgrounds)
file(COPY charmap/Blocks.txt DESTINATION ${_out}/fldesktop/charmap)
file(COPY wayland-sessions/fldesktop.desktop DESTINATION ${_out}/wayland-sessions)

file(GLOB _man ${CMAKE_SOURCE_DIR}/doc/man/*.1)
install(FILES ${_man} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
