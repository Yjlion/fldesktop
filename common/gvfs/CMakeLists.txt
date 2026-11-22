# GVfs locations (network shares, phones, cameras) through GIO and gvfs-fuse
if(TARGET fld-glib)
  add_library(fld-gvfs STATIC gvclient.cpp gvfs_util.cpp)
  target_include_directories(fld-gvfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(fld-gvfs PUBLIC fld-glib)
endif()
