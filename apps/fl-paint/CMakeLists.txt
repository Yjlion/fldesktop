# fl-paint: a raster drawing program
add_library(fl-paint-util STATIC paint_util.cpp ora.cpp)
find_package(ZLIB REQUIRED)
target_link_libraries(fl-paint-util PUBLIC ZLIB::ZLIB)
target_include_directories(fl-paint-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(fl-paint main.cpp)
target_link_libraries(fl-paint PRIVATE fl-paint-util fld)
install(TARGETS fl-paint RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
