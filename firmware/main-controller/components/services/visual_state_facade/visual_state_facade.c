#include "visual_state_facade.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define NB_VISUAL_PUBLISH_MS 125U
#define NB_VISUAL_TASK_STACK 3072U
#define NB_VISUAL_TASK_PRIORITY 6U

static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static nb_visual_state_sink_fn s_sink;
static nb_display_command_t s_scene;
static uint32_t s_next_generation;
static bool s_dirty;
static bool s_initialized;

static int16_t gaze_to_milli(float value)
{
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return (int16_t)lroundf(value * 1000.0f);
}

static void facade_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NB_VISUAL_PUBLISH_MS));

        nb_display_command_t command;
        bool publish = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_dirty) {
            command = s_scene;
            command.generation = ++s_next_generation;
            s_dirty = false;
            publish = true;
        }
        xSemaphoreGive(s_mutex);

        if (publish && s_sink(&command) != ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_dirty = true;
            xSemaphoreGive(s_mutex);
        }
    }
}

esp_err_t visual_state_facade_init(nb_visual_state_sink_fn sink)
{
    if (sink == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_scene, 0, sizeof(s_scene));
    s_scene.version = NB_DISPLAY_COMMAND_VERSION;
    s_scene.opcode = NB_DISPLAY_OP_SET_SCENE;
    s_scene.expression = 0U;
    s_scene.brightness = 255U;
    s_sink = sink;
    s_dirty = true;

    const BaseType_t created = xTaskCreate(
        facade_task,
        "nb_visual_state",
        NB_VISUAL_TASK_STACK,
        NULL,
        NB_VISUAL_TASK_PRIORITY,
        NULL);
    if (created != pdPASS) {
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

void visual_state_facade_set_expression(uint8_t expression)
{
    if (!s_initialized || expression >= NB_DISPLAY_EXPRESSION_COUNT) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_scene.expression != expression) {
        s_scene.expression = expression;
        s_dirty = true;
    }
    xSemaphoreGive(s_mutex);
}

void visual_state_facade_set_gaze(float x, float y)
{
    if (!s_initialized) {
        return;
    }
    const int16_t x_milli = gaze_to_milli(x);
    const int16_t y_milli = gaze_to_milli(y);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_scene.gaze_x_milli != x_milli ||
        s_scene.gaze_y_milli != y_milli) {
        s_scene.gaze_x_milli = x_milli;
        s_scene.gaze_y_milli = y_milli;
        s_dirty = true;
    }
    xSemaphoreGive(s_mutex);
}

void visual_state_facade_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_scene.brightness != brightness) {
        s_scene.brightness = brightness;
        s_dirty = true;
    }
    xSemaphoreGive(s_mutex);
}

void visual_state_facade_set_overlay(uint16_t flag, bool enabled)
{
    if (!s_initialized || (flag & ~NB_DISPLAY_OVERLAY_ALL) != 0U) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const uint16_t previous = s_scene.overlay_flags;
    if (enabled) {
        s_scene.overlay_flags |= flag;
    } else {
        s_scene.overlay_flags &= (uint16_t)~flag;
    }
    if (previous != s_scene.overlay_flags) {
        s_dirty = true;
    }
    xSemaphoreGive(s_mutex);
}

void visual_state_facade_force_publish(void)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}
