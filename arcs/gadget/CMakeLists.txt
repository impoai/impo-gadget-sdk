listenai_library_named(gadget_core)

listenai_library_sources(
    gadget_core.c
    gadget_shell.c
    gadget_input.c
    gadget_sound.c
    gadget_speech.c
    gadget_voice.c
    gadget_voice_logic.c
    gadget_pcm.c
    gadget_params.c
    gadget_policy.c
    gadget_status.c
    gadget_play_url.c
)

listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
