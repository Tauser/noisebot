idf_component_register(
    SRCS
        "voice_capture_session_v2.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
    # PRIV_REQUIRES infra intentionally omitted: declaring it creates the CMake
    # cycle infra->audio_service->voice_capture_session_v2->infra, which causes
    # the linker to place voice_capture_session_v2.a BEFORE audio_service.a —
    # wrong order for single-pass GNU ld without --start-group.
    # The include path for bridge_service.h is added manually below.
    # At link time, infra.a is processed before voice_capture_session_v2.a
    # (infra REQUIRES voice_capture_session_v2 in its own CMakeLists), so
    # bridge_service_* symbols are already defined when vcapv2.c.obj is extracted.
)

idf_component_get_property(infra_dir infra COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${infra_dir}")
