# fl-docs: PDF viewer on poppler-cpp
add_library(fl-docs-util STATIC docs_util.cpp)   # testable logic, linked by tests
target_include_directories(fl-docs-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(fl-docs main.cpp)
target_link_libraries(fl-docs PRIVATE fl-docs-util fld PkgConfig::POPPLER_CPP)
if(TARGET fld-printers)
  target_link_libraries(fl-docs PRIVATE fld-printers)
  target_compile_definitions(fl-docs PRIVATE FLD_HAVE_PRINTERS=1)
endif()
install(TARGETS fl-docs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
