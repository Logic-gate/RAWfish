// Standalone control protocol definitions and capture parser (no Android dependencies).
#ifndef RAWFISH_PREVIEW_COMMANDS_H
#define RAWFISH_PREVIEW_COMMANDS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define PREVIEW_MAX_RAW_BRACKET 3

enum preview_command_type {
    PREVIEW_COMMAND_NONE = 0,
    PREVIEW_COMMAND_CAPTURE_INVALID,
    PREVIEW_COMMAND_FOCUS,
    PREVIEW_COMMAND_CAPTURE_JPEG,
    PREVIEW_COMMAND_CAPTURE_RAW,
    PREVIEW_COMMAND_CAPTURE_RAW_BRACKET,
    PREVIEW_COMMAND_ZOOM,
    PREVIEW_COMMAND_SETTINGS,
    PREVIEW_COMMAND_EXPOSURE_SETTINGS,
    PREVIEW_COMMAND_FOCUS_HOLD,
    PREVIEW_COMMAND_FOCUS_HOLD_RELEASE,
};

struct preview_command {
    enum preview_command_type type;
    float focus_x;
    float focus_y;
    float zoom_ratio;
    int focus_mode;
    float focus_distance;
    int exposure_compensation;
    int scene_mode;
    int color_temperature_kelvin;
    int color_tint;
    int32_t sensor_sensitivity;
    int64_t exposure_time_ns;
    int aperture;
    int noise_reduction;
    char path[4096];
    char metadata_path[4096];
    int bracket_count;
    char bracket_paths[PREVIEW_MAX_RAW_BRACKET][4096];
    char bracket_metadata_paths[PREVIEW_MAX_RAW_BRACKET][4096];
    int32_t bracket_sensor_sensitivity[PREVIEW_MAX_RAW_BRACKET];
    int64_t bracket_exposure_time_ns[PREVIEW_MAX_RAW_BRACKET];
};

struct preview_command_buffer {
    char data[8192];
    size_t length;
    bool discarding;
};


// Strict positive decimal integers: reject signs, suffixes and overflow.
static bool preview_capture_integer(const char *text, int64_t maximum, int64_t *value)
{
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || *end || parsed <= 0 || parsed > maximum) return false;
    *value = parsed;
    return true;
}

// Returns true for capture commands, including malformed ones. Callers must
// acknowledge CAPTURE_INVALID so the host doesn't wait for a nonexistent file.
static bool preview_parse_capture_command(const char *line, struct preview_command *command)
{
    const char *start = line;
    while (*start == ' ' || *start == '\t' || *start == '\r') ++start;
    if (strncmp(start, "capture-", 8)) return false;
    command->type = PREVIEW_COMMAND_CAPTURE_INVALID;
    if (strlen(start) >= sizeof(((struct preview_command_buffer *)0)->data)) return true;
    char copy[sizeof(((struct preview_command_buffer *)0)->data)];
    strcpy(copy, start);
    char *save = NULL;
    char *name = strtok_r(copy, " \t\r", &save);
    char *args[11];
    int n = 0;
    char *token;
    while ((token = strtok_r(NULL, " \t\r", &save))) {
        if (n == 11) return true;
        args[n++] = token;
    }
    if (!strcmp(name, "capture-jpeg") && n == 1) {
        if (strlen(args[0]) >= sizeof(command->path)) return true;
        strcpy(command->path, args[0]);
        command->type = PREVIEW_COMMAND_CAPTURE_JPEG;
    } else if (!strcmp(name, "capture-raw") && n == 2) {
        if (strlen(args[0]) >= sizeof(command->path) ||
                strlen(args[1]) >= sizeof(command->metadata_path)) return true;
        strcpy(command->path, args[0]);
        strcpy(command->metadata_path, args[1]);
        command->type = PREVIEW_COMMAND_CAPTURE_RAW;
    } else if (!strcmp(name, "capture-raw-bracket") && n == 9 && !strcmp(args[0], "2")) {
        for (int i = 0; i < 2; ++i) {
            char *raw = args[1+i*4], *metadata = args[2+i*4];
            int64_t iso, exposure;
            if (strlen(raw) >= sizeof(command->bracket_paths[i]) ||
                    strlen(metadata) >= sizeof(command->bracket_metadata_paths[i]) ||
                    !preview_capture_integer(args[3+i*4], INT32_MAX, &iso) ||
                    !preview_capture_integer(args[4+i*4], INT64_MAX, &exposure)) return true;
            strcpy(command->bracket_paths[i], raw);
            strcpy(command->bracket_metadata_paths[i], metadata);
            command->bracket_sensor_sensitivity[i] = (int32_t)iso;
            command->bracket_exposure_time_ns[i] = exposure;
        }
        // RAW pair contract: fixed ISO, long followed by distinct short, unique files.
        if (command->bracket_sensor_sensitivity[0] != command->bracket_sensor_sensitivity[1] ||
                command->bracket_exposure_time_ns[0] <= command->bracket_exposure_time_ns[1]) return true;
        const char *paths[] = {args[1], args[2], args[5], args[6]};
        for (int i=0; i<4; ++i) for (int j=i+1; j<4; ++j)
            if (!strcmp(paths[i], paths[j])) return true;
        command->bracket_count = 2;
        command->type = PREVIEW_COMMAND_CAPTURE_RAW_BRACKET;
    }
    return true;
}
#endif
