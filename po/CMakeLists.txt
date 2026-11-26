# Translations: po/<lang>.po for each language in LINGUAS, compiled with
# msgfmt to <localedir>/<lang>/LC_MESSAGES/fldesktop.mo. tools/update-po.py
# updates the template (po/fldesktop.pot) and the catalogs.
find_program(FLD_MSGFMT msgfmt)
if(NOT FLD_MSGFMT)
  message(WARNING "msgfmt not found (gettext): translations are not built")
  return()
endif()
file(STRINGS LINGUAS _langs REGEX "^[^#]")
set(_mos)
foreach(lang ${_langs})
  string(STRIP "${lang}" lang)
  set(_po ${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po)
  # in the build tree too, for tools/run-dev.sh (FLD_LOCALEDIR=build/locale)
  set(_mo ${CMAKE_BINARY_DIR}/locale/${lang}/LC_MESSAGES/fldesktop.mo)
  add_custom_command(OUTPUT ${_mo}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/locale/${lang}/LC_MESSAGES
    COMMAND ${FLD_MSGFMT} --check -o ${_mo} ${_po}
    DEPENDS ${_po} VERBATIM)
  list(APPEND _mos ${_mo})
  install(FILES ${_mo} DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${lang}/LC_MESSAGES)
endforeach()
add_custom_target(translations ALL DEPENDS ${_mos})
