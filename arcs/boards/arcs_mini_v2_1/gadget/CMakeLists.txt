# The robot dog's gadget board file and helpers (the vendor's part).
listenai_library_named(gadget_board_dog)

listenai_library_sources(
    board_robot_dog.c
    dog_commands.c
    dog_logic.c
    dog_light.c
    dog_power.c
    dog_motion.c
    dog_actuators.c
    dog_gait.c
    dog_gait_preflight_policy.c
    dog_sensors.c
    dog_imu_interaction.c
    dog_shake_detector.c
)

listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
