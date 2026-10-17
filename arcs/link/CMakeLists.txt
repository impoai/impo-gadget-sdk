listenai_library_named(gadget_link)

listenai_library_sources(
    impo_link_glue.c
    impo_link_streams.c
    impo_glue_commands.c
    link_glue_logic.c
    link_cmds.c
    link_creds.c
    link_identity.c
)

listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# noise_control.h, vm_api.h, vm_connect.h of the shared link. After this library's own
# directories, so link/impo_link.h stays ours.
target_include_directories(${LISTENAI_CURRENT_LIBRARY} PRIVATE ${UPSTREAM_MAIN})

# impo_glue_commands.c implements upstream's impo_commands.h / impo_state.h, which
# noise_control.cpp calls.
set_source_files_properties(impo_glue_commands.c PROPERTIES INCLUDE_DIRECTORIES ${UPSTREAM_IMPO})
