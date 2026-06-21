/*
 * board_caps.c - Capacidades fixas da placa NoiseBot atual (Layer 1)
 */

#include "board_caps.h"
#include "nb_hw_config_profile.h"
#include "sdkconfig.h"

const nb_board_caps_t *nb_board_caps_get(void)
{
    static const nb_board_caps_t caps = {
        .board_name = NB_BOARD_PROFILE_NAME,
        .audio_sample_rate_hz = NB_AUDIO_SAMPLE_RATE,
        .mic_channels = 1,
        .speaker_channels = 1,
        .audio_full_duplex = true,
#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
        .audio_input_reference = false,
        .supports_device_aec = false,
#elif CONFIG_NB_BOARD_HAS_AUDIO_REFERENCE
        .audio_input_reference = true,
        .supports_device_aec = true,
#else
        .audio_input_reference = false,
        .supports_device_aec = false,
#endif
        .supports_wake_word = true,
#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
        .supports_camera = false,
#elif CONFIG_NB_CAMERA_DIAG_ENABLED
        .supports_camera = true,
#else
        .supports_camera = false,
#endif
        .supports_i2c_bus = true,
    };

    return &caps;
}
