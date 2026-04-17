idf_component_register(
    SRCS
        "behavior_engine.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        infra
        state_machine
        emotion_model
        conductor
        idle_service
        long_term_memory
        persona_service
)
