# fl-docs: document viewer. PDF through poppler-cpp; EPUB, OpenDocument text
# and comic books with libxml2, libarchive and Pango (DOCS_REFLOW).
add_library(fl-docs-util STATIC docs_util.cpp)   # testable logic, linked by tests
target_include_directories(fl-docs-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set(_sources main.cpp backend.cpp backend_pdf.cpp)
if(DOCS_REFLOW_FOUND)
  list(APPEND _sources archive_reader.cpp images.cpp flow.cpp backend_comic.cpp backend_epub.cpp backend_odt.cpp)
endif()
add_executable(fl-docs ${_sources})
target_link_libraries(fl-docs PRIVATE fl-docs-util fld PkgConfig::POPPLER_CPP)
if(DOCS_REFLOW_FOUND)
  target_link_libraries(fl-docs PRIVATE PkgConfig::DOCS_REFLOW)
  target_compile_definitions(fl-docs PRIVATE FLD_DOCS_REFLOW=1)
endif()
if(TARGET fld-printers)
  target_link_libraries(fl-docs PRIVATE fld-printers)
  target_compile_definitions(fl-docs PRIVATE FLD_HAVE_PRINTERS=1)
endif()
install(TARGETS fl-docs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
