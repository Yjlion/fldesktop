# The screens through wlr-output-management: the model (outputs) and the
# arrangement editor (displays_ui), for fl-settings' Displays page and
# fl-displays.
set(OUTPUTS_PROTO)
fld_wayland_protocol(OUTPUTS_PROTO ${CMAKE_SOURCE_DIR}/protocols/wlr-output-management-unstable-v1.xml client)
add_library(fld-displays STATIC outputs.cpp displays_ui.cpp ${OUTPUTS_PROTO})
target_include_directories(fld-displays PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${FLD_PROTO_OUT})
target_link_libraries(fld-displays PUBLIC fld PkgConfig::WAYLAND_CLIENT)
