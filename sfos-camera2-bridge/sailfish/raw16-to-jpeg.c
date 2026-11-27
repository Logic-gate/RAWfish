/**
 * @file raw16-to-jpeg.c
 * @brief Minimal RAW16/JSON to JPEG converter.
 *
 * Reads Camera2 RAW16 pixels and bridge JSON metadata, applies black-level and
 * white-level correction, performs simple Bayer demosaic, and writes an sRGB
 * JPEG image.
 */

#define _POSIX_C_SOURCE 200809L

#ifndef RAW16_TO_JPEG
#error This helper only builds the RAW16 to JPEG converter.
#endif

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include <jpeglib.h>

enum pixel_color {
    PIXEL_RED,
    PIXEL_GREEN,
    PIXEL_BLUE,
};

struct conversion_config {
    int width;
    int height;
    int row_stride;
    int white_level;
    int black_level[4];
    float gains[4];
    float matrix[9];
    float exposure;
    int color_temperature;
    int cfa_map;
    char cfa[8];
    char raw_path[4096];
};

static int64_t monotonic_ms(void)
{
    struct timespec time_value;
    if (clock_gettime(CLOCK_MONOTONIC, &time_value) != 0) {
        return 0;
    }
    return (int64_t)time_value.tv_sec * 1000 +
           (int64_t)time_value.tv_nsec / 1000000;
}

static char *read_text_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || length > 1024 * 1024 ||
            fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)length + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    if (fread(text, 1, (size_t)length, file) != (size_t)length) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[length] = '\0';
    fclose(file);
    return text;
}

static const char *json_value(const char *json, const char *key)
{
    char pattern[128];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >=
            (int)sizeof(pattern)) {
        return NULL;
    }
    const char *value = strstr(json, pattern);
    if (!value) {
        return NULL;
    }
    value += strlen(pattern);
    while (isspace((unsigned char)*value)) {
        ++value;
    }
    if (*value++ != ':') {
        return NULL;
    }
    while (isspace((unsigned char)*value)) {
        ++value;
    }
    return value;
}

static int json_int(const char *json, const char *key, int *result)
{
    const char *value = json_value(json, key);
    char *end = NULL;
    if (!value) {
        return -1;
    }
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || parsed < INT32_MIN || parsed > INT32_MAX) {
        return -1;
    }
    *result = (int)parsed;
    return 0;
}

static int json_string(const char *json, const char *key,
                       char *output, size_t output_size)
{
    const char *value = json_value(json, key);
    size_t used = 0;
    if (!value || *value++ != '"' || output_size == 0) {
        return -1;
    }
    while (*value && *value != '"') {
        unsigned char byte = (unsigned char)*value++;
        if (byte == '\\') {
            switch (*value++) {
            case '"': byte = '"'; break;
            case '\\': byte = '\\'; break;
            case '/': byte = '/'; break;
            case 'b': byte = '\b'; break;
            case 'f': byte = '\f'; break;
            case 'n': byte = '\n'; break;
            case 'r': byte = '\r'; break;
            case 't': byte = '\t'; break;
            default: return -1;
            }
        }
        if (used + 1 >= output_size) {
            return -1;
        }
        output[used++] = (char)byte;
    }
    if (*value != '"') {
        return -1;
    }
    output[used] = '\0';
    return 0;
}

static int json_flat_array(const char *json, const char *key,
                           float *output, int wanted)
{
    const char *value = json_value(json, key);
    if (!value || *value++ != '[') {
        return -1;
    }
    for (int index = 0; index < wanted; ++index) {
        while (isspace((unsigned char)*value)) {
            ++value;
        }
        char *end = NULL;
        output[index] = strtof(value, &end);
        if (end == value) {
            return -1;
        }
        value = end;
        while (isspace((unsigned char)*value)) {
            ++value;
        }
        if (index + 1 < wanted) {
            if (*value++ != ',') {
                return -1;
            }
        } else if (*value != ']') {
            return -1;
        }
    }
    return 0;
}

static int json_rational_matrix(const char *json, const char *key,
                                float *output)
{
    const char *value = json_value(json, key);
    if (!value || *value++ != '[') {
        return -1;
    }
    for (int index = 0; index < 9; ++index) {
        while (isspace((unsigned char)*value)) {
            ++value;
        }
        if (*value++ != '[') {
            return -1;
        }
        char *end = NULL;
        float numerator = strtof(value, &end);
        if (end == value) {
            return -1;
        }
        value = end;
        while (isspace((unsigned char)*value)) {
            ++value;
        }
        if (*value++ != ',') {
            return -1;
        }
        float denominator = strtof(value, &end);
        if (end == value || denominator == 0.0f) {
            return -1;
        }
        value = end;
        while (isspace((unsigned char)*value)) {
            ++value;
        }
        if (*value++ != ']') {
            return -1;
        }
        output[index] = numerator / denominator;
        while (isspace((unsigned char)*value)) {
            ++value;
        }
        if (index + 1 < 9) {
            if (*value++ != ',') {
                return -1;
            }
        } else if (*value != ']') {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Load conversion settings from a bridge JSON metadata file.
 *
 * @return 0 on success, -1 when required metadata is absent or invalid.
 */
static int load_config(const char *metadata_path,
                       struct conversion_config *config)
{
    char *json = read_text_file(metadata_path);
    if (!json) {
        fprintf(stderr, "Cannot read metadata: %s\n", metadata_path);
        return -1;
    }

    float black[4];
    int ok = json_int(json, "width", &config->width) == 0 &&
        json_int(json, "height", &config->height) == 0 &&
        json_int(json, "row_stride", &config->row_stride) == 0 &&
        json_int(json, "white_level", &config->white_level) == 0 &&
        json_string(json, "cfa", config->cfa, sizeof(config->cfa)) == 0 &&
        json_string(json, "raw_path", config->raw_path,
                    sizeof(config->raw_path)) == 0 &&
        json_flat_array(json, "black_level_pattern", black, 4) == 0 &&
        json_flat_array(json, "color_correction_gains", config->gains, 4) == 0;

    for (int index = 0; index < 4; ++index) {
        config->black_level[index] = (int)black[index];
    }
    if (json_int(json, "color_temperature_requested_kelvin",
                 &config->color_temperature) != 0) {
        config->color_temperature = 0;
    }
    if (json_rational_matrix(json, "capture_color_transform",
                             config->matrix) != 0) {
        memset(config->matrix, 0, sizeof(config->matrix));
        config->matrix[0] = 1.0f;
        config->matrix[4] = 1.0f;
        config->matrix[8] = 1.0f;
        fprintf(stderr, "Warning: using identity color transform\n");
    }
    free(json);

    config->cfa_map = !strcmp(config->cfa, "RGGB") ? 0 :
                      !strcmp(config->cfa, "GRBG") ? 1 :
                      !strcmp(config->cfa, "GBRG") ? 2 : 3;
    if (!ok || config->width <= 1 || config->height <= 1 ||
            config->row_stride < config->width * 2 ||
            config->white_level <= 0 ||
            config->white_level <= config->black_level[0] ||
            config->white_level <= config->black_level[1] ||
            config->white_level <= config->black_level[2] ||
            config->white_level <= config->black_level[3] ||
            (strcmp(config->cfa, "RGGB") && strcmp(config->cfa, "GRBG") &&
             strcmp(config->cfa, "GBRG") && strcmp(config->cfa, "BGGR"))) {
        fprintf(stderr, "Unsupported or incomplete metadata\n");
        return -1;
    }
    if (config->color_temperature > 0) {
        float normalized = ((float)config->color_temperature - 6500.0f) /
                           4500.0f;
        if (normalized < -1.0f) {
            normalized = -1.0f;
        } else if (normalized > 1.0f) {
            normalized = 1.0f;
        }
        float red_scale = 1.0f + normalized * 0.45f;
        float blue_scale = 1.0f - normalized * 0.45f;
        if (red_scale < 0.25f) {
            red_scale = 0.25f;
        }
        if (blue_scale < 0.25f) {
            blue_scale = 0.25f;
        }
        config->gains[0] *= red_scale;
        config->gains[3] *= blue_scale;
    }
    return 0;
}

static enum pixel_color pixel_color_at(const struct conversion_config *config,
                                       int x, int y)
{
    static const enum pixel_color maps[4][4] = {
        { PIXEL_RED, PIXEL_GREEN, PIXEL_GREEN, PIXEL_BLUE },
        { PIXEL_GREEN, PIXEL_RED, PIXEL_BLUE, PIXEL_GREEN },
        { PIXEL_GREEN, PIXEL_BLUE, PIXEL_RED, PIXEL_GREEN },
        { PIXEL_BLUE, PIXEL_GREEN, PIXEL_GREEN, PIXEL_RED },
    };
    return maps[config->cfa_map][(y & 1) * 2 + (x & 1)];
}

static float corrected_sample(const struct conversion_config *config,
                              const uint16_t *pixels, int x, int y)
{
    int pattern_index = (y & 1) * 2 + (x & 1);
    int black = config->black_level[pattern_index];
    int value = pixels[(size_t)y * (size_t)config->width + (size_t)x];
    float normalized = value > black ?
        (float)(value - black) / (float)(config->white_level - black) : 0.0f;
    enum pixel_color color = pixel_color_at(config, x, y);
    int gain_index = color == PIXEL_RED ? 0 :
                     color == PIXEL_BLUE ? 3 : ((y & 1) ? 2 : 1);
    return normalized * config->gains[gain_index];
}

/**
 * @brief Run fn over [0, total) in chunks, spread across the online cores.
 *
 * Chunks are handed out dynamically so the slower cores of a big.LITTLE SoC do
 * not hold the others up. RAW16_JPEG_THREADS overrides the thread count.
 */
typedef void (*chunk_fn)(void *context, int start, int end);

struct parallel_job {
    chunk_fn fn;
    void *context;
    int total;
    int chunk;
    int next;
};

static void *parallel_worker(void *argument)
{
    struct parallel_job *job = argument;
    for (;;) {
        int start = __atomic_fetch_add(&job->next, job->chunk,
                                       __ATOMIC_RELAXED);
        if (start >= job->total) {
            break;
        }
        int end = start + job->chunk;
        if (end > job->total) {
            end = job->total;
        }
        job->fn(job->context, start, end);
    }
    return NULL;
}

static void run_parallel(chunk_fn fn, void *context, int total, int chunk)
{
    long threads = sysconf(_SC_NPROCESSORS_ONLN);
    const char *override = getenv("RAW16_JPEG_THREADS");
    if (override && atoi(override) > 0) {
        threads = atoi(override);
    }
    if (threads < 1) {
        threads = 1;
    } else if (threads > 8) {
        threads = 8;
    }
    struct parallel_job job = { fn, context, total, chunk, 0 };
    pthread_t workers[7];
    int started = 0;
    for (int index = 0; index < threads - 1; ++index) {
        if (pthread_create(&workers[started], NULL, parallel_worker, &job) == 0) {
            ++started;
        }
    }
    parallel_worker(&job);
    for (int index = 0; index < started; ++index) {
        pthread_join(workers[index], NULL);
    }
}

struct mosaic_job {
    const struct conversion_config *config;
    const uint16_t *pixels;
    float *corrected;
};

static void build_mosaic_rows(void *context, int start, int end)
{
    struct mosaic_job *job = context;
    for (int y = start; y < end; ++y) {
        for (int x = 0; x < job->config->width; ++x) {
            job->corrected[(size_t)y * (size_t)job->config->width + (size_t)x] =
                corrected_sample(job->config, job->pixels, x, y);
        }
    }
}

/**
 * @brief Precompute black-level corrected and white-balanced mosaic samples.
 *
 * Fast JPEG visits each Bayer sample several times while demosaicing. Keeping
 * the corrected mosaic separate avoids repeated raw normalization in the hot
 * loop.
 */
static float *build_corrected_mosaic(const struct conversion_config *config,
                                     const uint16_t *pixels)
{
    float *corrected = malloc((size_t)config->width *
                              (size_t)config->height * sizeof(*corrected));
    if (!corrected) {
        return NULL;
    }
    struct mosaic_job job = { config, pixels, corrected };
    run_parallel(build_mosaic_rows, &job, config->height, 16);
    return corrected;
}

static float demosaic_corrected_channel(const struct conversion_config *config,
                                        const float *corrected, int x, int y,
                                        enum pixel_color wanted)
{
    if (pixel_color_at(config, x, y) == wanted) {
        return corrected[(size_t)y * (size_t)config->width + (size_t)x];
    }
    float sum = 0.0f;
    int count = 0;
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        int sample_y = y + offset_y;
        if (sample_y < 0 || sample_y >= config->height) {
            continue;
        }
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            int sample_x = x + offset_x;
            if (sample_x < 0 || sample_x >= config->width ||
                    pixel_color_at(config, sample_x, sample_y) != wanted) {
                continue;
            }
            sum += corrected[(size_t)sample_y * (size_t)config->width +
                             (size_t)sample_x];
            ++count;
        }
    }
    return count ? sum / (float)count : 0.0f;
}

static float clamp_unit(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

static unsigned char srgb_lut[65536];

static void initialize_srgb_lut(void)
{
    for (int index = 0; index < 65536; ++index) {
        float linear = (float)index / 65535.0f;
        float encoded = linear <= 0.0031308f ?
            12.92f * linear :
            1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
        int value = (int)(encoded * 255.0f + 0.5f);
        srgb_lut[index] = (unsigned char)(value < 0 ? 0 :
                                          value > 255 ? 255 : value);
    }
}

static unsigned char srgb_byte(float linear)
{
    linear = clamp_unit(linear);
    int index = (int)(linear * 65535.0f + 0.5f);
    return srgb_lut[index];
}

static uint16_t *read_raw(const struct conversion_config *config)
{
    FILE *file = fopen(config->raw_path, "rb");
    if (!file) {
        fprintf(stderr, "Cannot read RAW16 file: %s\n", config->raw_path);
        return NULL;
    }
    uint16_t *pixels = malloc((size_t)config->width *
                              (size_t)config->height * sizeof(*pixels));
    unsigned char *row = malloc((size_t)config->row_stride);
    if (!pixels || !row) {
        fprintf(stderr, "Not enough memory for RAW16 image\n");
        free(pixels);
        free(row);
        fclose(file);
        return NULL;
    }
    for (int y = 0; y < config->height; ++y) {
        if (fread(row, 1, (size_t)config->row_stride, file) !=
                (size_t)config->row_stride) {
            fprintf(stderr, "RAW16 file ended at row %d\n", y);
            free(pixels);
            free(row);
            fclose(file);
            return NULL;
        }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        memcpy(pixels + (size_t)y * (size_t)config->width, row,
               (size_t)config->width * 2);
#else
        for (int x = 0; x < config->width; ++x) {
            pixels[(size_t)y * (size_t)config->width + (size_t)x] =
                (uint16_t)row[x * 2] | (uint16_t)((uint16_t)row[x * 2 + 1] << 8);
        }
#endif
    }
    free(row);
    fclose(file);
    return pixels;
}

static void render_rgb_row_generic(const struct conversion_config *config,
                                   const float *corrected, int y,
                                   unsigned char *row, int x_start, int x_end)
{
    for (int x = x_start; x < x_end; ++x) {
        float sensor[3] = {
            demosaic_corrected_channel(config, corrected, x, y, PIXEL_RED),
            demosaic_corrected_channel(config, corrected, x, y, PIXEL_GREEN),
            demosaic_corrected_channel(config, corrected, x, y, PIXEL_BLUE),
        };
        for (int channel = 0; channel < 3; ++channel) {
            float output = config->exposure *
                (config->matrix[channel * 3] * sensor[0] +
                 config->matrix[channel * 3 + 1] * sensor[1] +
                 config->matrix[channel * 3 + 2] * sensor[2]);
            row[x * 3 + channel] = srgb_byte(output);
        }
    }
}

static inline void store_pixel(const struct conversion_config *config,
                               unsigned char *out, float red, float green,
                               float blue)
{
    for (int channel = 0; channel < 3; ++channel) {
        float output = config->exposure *
            (config->matrix[channel * 3] * red +
             config->matrix[channel * 3 + 1] * green +
             config->matrix[channel * 3 + 2] * blue);
        out[channel] = srgb_byte(output);
    }
}

/*
 * Interior pixels have all eight neighbours, and a Bayer pattern fixes which of
 * them carry the missing colours: a red or blue site takes green from the four
 * edge neighbours and the opposite colour from the four diagonals; a green site
 * takes red and blue from its two horizontal and two vertical neighbours. The
 * additions keep the order of the generic 3x3 walk, so the result is identical.
 */
static void render_rgb_row_fast(const struct conversion_config *config,
                                const float *corrected, int y,
                                unsigned char *row)
{
    const int width = config->width;
    if (y == 0 || y == config->height - 1 || width < 3) {
        render_rgb_row_generic(config, corrected, y, row, 0, width);
        return;
    }
    render_rgb_row_generic(config, corrected, y, row, 0, 1);
    render_rgb_row_generic(config, corrected, y, row, width - 1, width);

    const enum pixel_color even = pixel_color_at(config, 0, y);
    const enum pixel_color odd = pixel_color_at(config, 1, y);
    const float *up = corrected + (size_t)(y - 1) * (size_t)width;
    const float *mid = corrected + (size_t)y * (size_t)width;
    const float *down = corrected + (size_t)(y + 1) * (size_t)width;

    for (int x = 1; x < width - 1; ++x) {
        const enum pixel_color color = (x & 1) ? odd : even;
        const float value = mid[x];
        unsigned char *out = row + x * 3;
        if (color == PIXEL_GREEN) {
            const float horizontal = (mid[x - 1] + mid[x + 1]) / 2.0f;
            const float vertical = (up[x] + down[x]) / 2.0f;
            const enum pixel_color side = (x & 1) ? even : odd;
            if (side == PIXEL_RED) {
                store_pixel(config, out, horizontal, value, vertical);
            } else {
                store_pixel(config, out, vertical, value, horizontal);
            }
            continue;
        }
        const float cross = (((up[x] + mid[x - 1]) + mid[x + 1]) + down[x]) / 4.0f;
        const float diagonal = (((up[x - 1] + up[x + 1]) + down[x - 1]) +
                                down[x + 1]) / 4.0f;
        if (color == PIXEL_RED) {
            store_pixel(config, out, value, cross, diagonal);
        } else {
            store_pixel(config, out, diagonal, cross, value);
        }
    }
}

struct render_job {
    const struct conversion_config *config;
    const float *corrected;
    unsigned char *image;
};

static void render_rows(void *context, int start, int end)
{
    struct render_job *job = context;
    for (int y = start; y < end; ++y) {
        render_rgb_row_fast(job->config, job->corrected, y,
                            job->image + (size_t)y * (size_t)job->config->width * 3);
    }
}

struct rotate_job {
    const struct conversion_config *config;
    const unsigned char *image;
    unsigned char *rotated;
    int rotation;
    int output_width;
    int output_height;
};

/* Rotate a block of output rows; the inner loop walks the source in short
 * contiguous runs so a 90/270 degree turn does not miss the cache per pixel. */
static void rotate_rows(void *context, int start, int end)
{
    struct rotate_job *job = context;
    const int width = job->config->width;
    const int height = job->config->height;
    for (int output_x = 0; output_x < job->output_width; ++output_x) {
        for (int output_y = start; output_y < end; ++output_y) {
            int source_x = output_x;
            int source_y = output_y;
            switch (job->rotation) {
            case 90:
                source_x = output_y;
                source_y = height - 1 - output_x;
                break;
            case 180:
                source_x = width - 1 - output_x;
                source_y = height - 1 - output_y;
                break;
            case 270:
                source_x = width - 1 - output_y;
                source_y = output_x;
                break;
            default:
                break;
            }
            const unsigned char *source =
                job->image + ((size_t)source_y * (size_t)width +
                              (size_t)source_x) * 3;
            unsigned char *target = job->rotated +
                ((size_t)output_y * (size_t)job->output_width +
                 (size_t)output_x) * 3;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
        }
    }
}

/**
 * @brief Convert RAW16 pixels directly to a JPEG file.
 *
 * @return 0 on success, -1 on write/conversion failure.
 */
static int write_jpeg(const struct conversion_config *config,
                      const uint16_t *pixels, const char *output_path,
                      int quality, int progressive, int rotation_degrees,
                      int64_t *prepare_ms, int64_t *render_ms,
                      int64_t *encode_ms, int64_t *rotate_ms)
{
    const int rotation = ((rotation_degrees % 360) + 360) % 360;
    const int rotated = rotation == 90 || rotation == 270;
    const int output_width = rotated ? config->height : config->width;
    const int output_height = rotated ? config->width : config->height;
    const int64_t prepare_start = monotonic_ms();

    float *corrected = build_corrected_mosaic(config, pixels);
    if (!corrected) {
        fprintf(stderr, "Not enough memory for corrected RAW16 image\n");
        return -1;
    }
    initialize_srgb_lut();
    if (prepare_ms) {
        *prepare_ms = monotonic_ms() - prepare_start;
    }

    unsigned char *image = malloc((size_t)config->width *
                                  (size_t)config->height * 3);
    if (!image) {
        free(corrected);
        return -1;
    }
    const int64_t render_start = monotonic_ms();
    struct render_job render = { config, corrected, image };
    run_parallel(render_rows, &render, config->height, 16);
    free(corrected);
    if (render_ms) {
        *render_ms = monotonic_ms() - render_start;
    }

    unsigned char *output = image;
    if (rotation != 0) {
        const int64_t rotate_start = monotonic_ms();
        output = malloc((size_t)output_width * (size_t)output_height * 3);
        if (!output) {
            free(image);
            return -1;
        }
        struct rotate_job turn = { config, image, output, rotation,
                                   output_width, output_height };
        run_parallel(rotate_rows, &turn, output_height, 16);
        free(image);
        image = output;
        if (rotate_ms) {
            *rotate_ms = monotonic_ms() - rotate_start;
        }
    }

    FILE *file = fopen(output_path, "wb");
    if (!file) {
        fprintf(stderr, "Cannot create JPEG: %s\n", output_path);
        free(image);
        return -1;
    }

    struct jpeg_compress_struct jpeg;
    struct jpeg_error_mgr error;
    jpeg.err = jpeg_std_error(&error);
    jpeg_create_compress(&jpeg);
    jpeg_stdio_dest(&jpeg, file);

    jpeg.image_width = (JDIMENSION)output_width;
    jpeg.image_height = (JDIMENSION)output_height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, quality, TRUE);
    if (progressive) {
        jpeg_simple_progression(&jpeg);
    }

    const int64_t encode_start = monotonic_ms();
    jpeg_start_compress(&jpeg, TRUE);
    while (jpeg.next_scanline < jpeg.image_height) {
        JSAMPROW row_pointer[1];
        row_pointer[0] = output + (size_t)jpeg.next_scanline *
                                  (size_t)output_width * 3;
        if (jpeg_write_scanlines(&jpeg, row_pointer, 1) != 1) {
            free(image);
            jpeg_destroy_compress(&jpeg);
            fclose(file);
            return -1;
        }
    }
    if (encode_ms) {
        *encode_ms = monotonic_ms() - encode_start;
    }
    free(image);
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    return fclose(file) == 0 ? 0 : -1;
}

/**
 * @brief Command-line entry point for RAW16/JSON to JPEG conversion.
 */
int main(int argc, char **argv)
{
    if (argc < 3 || argc > 7) {
        fprintf(stderr,
                "Usage: %s METADATA.json OUTPUT.jpg [EXPOSURE] [QUALITY] [PROGRESSIVE] [ROTATION]\n",
                argv[0]);
        return 2;
    }
    struct conversion_config config = { .exposure = 1.0f };
    int quality = 92;
    int progressive = 0;
    int rotation_degrees = 0;
    if (argc >= 4) {
        char *end = NULL;
        config.exposure = strtof(argv[3], &end);
        if (!end || *end || config.exposure <= 0.0f ||
                config.exposure > 32.0f) {
            fprintf(stderr, "EXPOSURE must be greater than 0 and at most 32\n");
            return 2;
        }
    }
    if (argc >= 5) {
        char *end = NULL;
        long parsed = strtol(argv[4], &end, 10);
        if (!end || *end || parsed < 1 || parsed > 100) {
            fprintf(stderr, "QUALITY must be from 1 to 100\n");
            return 2;
        }
        quality = (int)parsed;
    }
    if (argc >= 6) {
        progressive = strcmp(argv[5], "0") != 0 &&
                      strcmp(argv[5], "false") != 0 &&
                      strcmp(argv[5], "no") != 0;
    }
    if (argc >= 7) {
        char *end = NULL;
        long parsed = strtol(argv[6], &end, 10);
        if (!end || *end || parsed < 0 || parsed >= 360 ||
                parsed % 90 != 0) {
            fprintf(stderr, "ROTATION must be 0, 90, 180, or 270\n");
            return 2;
        }
        rotation_degrees = (int)parsed;
    }
    const int64_t total_start = monotonic_ms();
    const int64_t metadata_start = monotonic_ms();
    if (load_config(argv[1], &config) != 0) {
        return 3;
    }
    const int64_t metadata_ms = monotonic_ms() - metadata_start;
    const int64_t read_start = monotonic_ms();
    uint16_t *pixels = read_raw(&config);
    if (!pixels) {
        return 4;
    }
    const int64_t read_ms = monotonic_ms() - read_start;
    fprintf(stderr,
            "Demosaicing %dx%d %s RAW16, exposure %.3g, WB %dK...\n",
            config.width, config.height, config.cfa, config.exposure,
            config.color_temperature);
    int64_t prepare_ms = 0;
    int64_t render_ms = 0;
    int64_t encode_ms = 0;
    int64_t rotate_ms = 0;
    int result = write_jpeg(&config, pixels, argv[2], quality, progressive,
                            rotation_degrees, &prepare_ms, &render_ms,
                            &encode_ms, &rotate_ms);
    const int64_t total_ms = monotonic_ms() - total_start;
    free(pixels);
    if (result != 0) {
        fprintf(stderr, "Failed while writing output image\n");
        return 5;
    }
    fprintf(stderr,
            "capture-timing rawjpeg metadata=%lld read=%lld prepare=%lld render=%lld rotate=%lld encode=%lld total=%lld rotation=%d progressive=%d\n",
            (long long)metadata_ms, (long long)read_ms,
            (long long)prepare_ms, (long long)render_ms,
            (long long)rotate_ms, (long long)encode_ms, (long long)total_ms, rotation_degrees,
            progressive);
    return 0;
}
