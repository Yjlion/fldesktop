add_library(fld STATIC
  util.cpp
  ini.cpp
  desktop_entry.cpp
  icons.cpp
  mime.cpp
  theme.cpp
  json.cpp
  trash.cpp
  widgets.cpp
)
target_include_directories(fld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fld PUBLIC fltk::images fltk::fltk)
