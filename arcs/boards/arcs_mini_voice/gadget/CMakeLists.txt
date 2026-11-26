add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../components/impo_arcs_mini ${CMAKE_BINARY_DIR}/impo_arcs_mini)
listenai_library_named(board_arcs_mini)
listenai_library_sources(board_arcs_mini.c arcs_mini_button.c arcs_mini_display.c arcs_mini_screen_ui.c arcs_mini_wake.c)
listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
