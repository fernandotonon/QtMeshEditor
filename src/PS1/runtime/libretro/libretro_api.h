/* Minimal libretro API subset for QtMeshEditor PS1 rip plugins.
 * Derived from libretro.h (MIT). See https://www.libretro.com/ */
#ifndef LIBRETRO_API_H
#define LIBRETRO_API_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETRO_API_VERSION 1

typedef bool (*retro_environment_t)(unsigned cmd, void *data);
typedef void (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565 = 2,
};

enum retro_language {
    RETRO_LANGUAGE_ENGLISH = 0,
};

enum retro_key {
    RETRO_KEY_UNKNOWN = 0,
};

#define RETRO_DEVICE_JOYPAD 1
#define RETRO_DEVICE_ANALOG 5

#define RETRO_DEVICE_ID_JOYPAD_B 0
#define RETRO_DEVICE_ID_JOYPAD_Y 1
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START 3
#define RETRO_DEVICE_ID_JOYPAD_UP 4
#define RETRO_DEVICE_ID_JOYPAD_DOWN 5
#define RETRO_DEVICE_ID_JOYPAD_LEFT 6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT 7
#define RETRO_DEVICE_ID_JOYPAD_A 8
#define RETRO_DEVICE_ID_JOYPAD_X 9
#define RETRO_DEVICE_ID_JOYPAD_L 10
#define RETRO_DEVICE_ID_JOYPAD_R 11
#define RETRO_DEVICE_ID_JOYPAD_L2 12
#define RETRO_DEVICE_ID_JOYPAD_R2 13
#define RETRO_DEVICE_ID_JOYPAD_L3 14
#define RETRO_DEVICE_ID_JOYPAD_R3 15

#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000

#define RETRO_ENVIRONMENT_GET_OVERSCAN 2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_SET_HW_RENDER 14
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_SET_VARIABLES 16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME 18
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_GET_LANGUAGE 39
#define RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE (41 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE (43 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_MEMORY_MAPS (36 | RETRO_ENVIRONMENT_EXPERIMENTAL)

#define RETRO_MEMORY_SAVE_RAM 0
#define RETRO_MEMORY_RTC 1
#define RETRO_MEMORY_SYSTEM_RAM 2

#define RETRO_MEMORY_ACCESS_WRITE (1 << 0)
#define RETRO_MEMORY_ACCESS_READ (1 << 1)
#define RETRO_MEMORY_ACCESS_READ_WRITE (RETRO_MEMORY_ACCESS_READ | RETRO_MEMORY_ACCESS_WRITE)

struct retro_variable {
    const char *key;
    const char *value;
};

struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
};

struct retro_log_callback {
    void (*log)(enum retro_log_level level, const char *fmt, ...);
};

struct retro_memory_descriptor {
    uintptr_t flags;
    uint8_t *ptr;
    size_t offset;
    size_t start;
    size_t select;
    size_t disconnect;
    size_t len;
    const char *addrspace;
};

struct retro_memory_map {
    const struct retro_memory_descriptor *descriptors;
    unsigned num_descriptors;
};

typedef void (*retro_init_t)(void);
typedef void (*retro_deinit_t)(void);
typedef unsigned (*retro_api_version_t)(void);
typedef void (*retro_get_system_info_t)(struct retro_system_info *info);
typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info *info);
typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);
typedef bool (*retro_load_game_t)(const struct retro_game_info *game);
typedef void (*retro_unload_game_t)(void);
typedef void (*retro_run_t)(void);
typedef void (*retro_reset_t)(void);
typedef void *(*retro_get_memory_data_t)(unsigned id);
typedef size_t (*retro_get_memory_size_t)(unsigned id);

#ifdef __cplusplus
}
#endif

#endif // LIBRETRO_API_H
