/**
 * @file jpeg_capture.c
 * @brief Direct Camera2 JPEG still capture implementation.
 *
 * Opens a Camera2 device, configures a JPEG ImageReader, submits a single still
 * request, writes the JPEG payload to disk, and reports timing/status as JSON.
 */

#include "camera2_bridge.h"
#include "camera2_common.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum jpeg_error {
    JPEG_INVALID_ARGUMENT = -40,
    JPEG_UNSUPPORTED_SIZE = -41,
    JPEG_MANAGER_ERROR = -42,
    JPEG_CHARACTERISTICS_ERROR = -43,
    JPEG_READER_ERROR = -44,
    JPEG_OPEN_ERROR = -45,
    JPEG_CONFIGURATION_ERROR = -46,
    JPEG_SUBMIT_ERROR = -47,
    JPEG_TIMEOUT = -48,
    JPEG_WRITE_ERROR = -49,
};

struct jpeg_context {
    struct sfos_camera2_status status;
    atomic_int result_status;
    int width;
    int height;
    int data_length;
    const char *jpeg_path;
    int64_t started_ms;
    int64_t characteristics_ms;
    int64_t reader_ms;
    int64_t camera_open_ms;
    int64_t session_ms;
    int64_t capture_submit_ms;
    int64_t image_available_ms;
    int64_t file_written_ms;
    int32_t requested_sensitivity;
    int64_t requested_exposure_time_ns;
    int64_t exposure_time_range[2];
    int32_t requested_orientation_degrees;
    int32_t requested_aperture;
    int32_t requested_scene_mode_original;
    int32_t requested_scene_mode;
    int32_t scene_mode_supported;
    int32_t hdr_scene_supported;
    int32_t control_mode_applied;
    int32_t scene_mode_applied;
    float focal_length;
    int32_t actual_sensitivity;
    int64_t actual_exposure_time_ns;
    int64_t actual_frame_duration_ns;
};

static bool jpeg_hal_exposure_suspect(int64_t hal_exposure_time_ns,
                                      int64_t elapsed_ms)
{
    if (hal_exposure_time_ns <= 0 || elapsed_ms <= 0) {
        return false;
    }
    return hal_exposure_time_ns > ((elapsed_ms + 100) * 1000000LL);
}

static void jpeg_log_effective_capture(const struct jpeg_context *context)
{
    if (!context || context->capture_submit_ms <= 0 ||
            context->image_available_ms <= 0) {
        return;
    }
    int64_t submit_to_image_ms = context->image_available_ms -
                                 context->capture_submit_ms;
    int64_t submit_to_file_ms = context->file_written_ms > 0
        ? context->file_written_ms - context->capture_submit_ms : -1;
    fprintf(stderr,
            "capture-effective jpeg requested_iso=%d "
            "requested_shutter=%lld hal_iso=%d hal_shutter=%lld "
            "hal_frame=%lld submit_to_image_ms=%lld "
            "submit_to_file_ms=%lld suspect_hal_exposure=%d\n",
            context->requested_sensitivity,
            (long long)context->requested_exposure_time_ns,
            context->actual_sensitivity,
            (long long)context->actual_exposure_time_ns,
            (long long)context->actual_frame_duration_ns,
            (long long)submit_to_image_ms,
            (long long)submit_to_file_ms,
            jpeg_hal_exposure_suspect(context->actual_exposure_time_ns,
                                      submit_to_image_ms) ? 1 : 0);
}

static const char *jpeg_scene_mode_name(int scene_mode)
{
    switch (scene_mode) {
    case SFOS_CAMERA2_SCENE_PORTRAIT: return "portrait";
    case SFOS_CAMERA2_SCENE_LANDSCAPE: return "landscape";
    case SFOS_CAMERA2_SCENE_SPORT: return "sport";
    case SFOS_CAMERA2_SCENE_NIGHT: return "night";
    case SFOS_CAMERA2_SCENE_AUTO: return "auto";
    case SFOS_CAMERA2_SCENE_ACTION: return "action";
    case SFOS_CAMERA2_SCENE_NIGHT_PORTRAIT: return "night-portrait";
    case SFOS_CAMERA2_SCENE_THEATRE: return "theatre";
    case SFOS_CAMERA2_SCENE_BEACH: return "beach";
    case SFOS_CAMERA2_SCENE_SNOW: return "snow";
    case SFOS_CAMERA2_SCENE_SUNSET: return "sunset";
    case SFOS_CAMERA2_SCENE_STEADY_PHOTO: return "steady-photo";
    case SFOS_CAMERA2_SCENE_FIREWORKS: return "fireworks";
    case SFOS_CAMERA2_SCENE_PARTY: return "party";
    case SFOS_CAMERA2_SCENE_CANDLELIGHT: return "candlelight";
    case SFOS_CAMERA2_SCENE_BARCODE: return "barcode";
    case SFOS_CAMERA2_SCENE_BACKLIGHT: return "backlight";
    case SFOS_CAMERA2_SCENE_FLOWERS: return "flowers";
    case SFOS_CAMERA2_SCENE_AR: return "ar";
    case SFOS_CAMERA2_SCENE_HDR: return "hdr";
    default: return "manual";
    }
}

static const char *jpeg_control_mode_name(int control_mode)
{
    switch (control_mode) {
    case ACAMERA_CONTROL_MODE_OFF: return "off";
    case ACAMERA_CONTROL_MODE_AUTO: return "auto";
    case ACAMERA_CONTROL_MODE_USE_SCENE_MODE: return "use-scene-mode";
#ifdef ACAMERA_CONTROL_MODE_OFF_KEEP_STATE
    case ACAMERA_CONTROL_MODE_OFF_KEEP_STATE: return "off-keep-state";
#endif
    default: return "unknown";
    }
}

static const char *jpeg_android_scene_mode_name(int scene_mode)
{
    switch (scene_mode) {
    case ACAMERA_CONTROL_SCENE_MODE_ACTION: return "action";
    case ACAMERA_CONTROL_SCENE_MODE_PORTRAIT: return "portrait";
    case ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE: return "landscape";
    case ACAMERA_CONTROL_SCENE_MODE_NIGHT: return "night";
    case ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT: return "night-portrait";
    case ACAMERA_CONTROL_SCENE_MODE_THEATRE: return "theatre";
    case ACAMERA_CONTROL_SCENE_MODE_BEACH: return "beach";
    case ACAMERA_CONTROL_SCENE_MODE_SNOW: return "snow";
    case ACAMERA_CONTROL_SCENE_MODE_SUNSET: return "sunset";
    case ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO: return "steady-photo";
    case ACAMERA_CONTROL_SCENE_MODE_FIREWORKS: return "fireworks";
    case ACAMERA_CONTROL_SCENE_MODE_SPORTS: return "sport";
    case ACAMERA_CONTROL_SCENE_MODE_PARTY: return "party";
    case ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT: return "candlelight";
    case ACAMERA_CONTROL_SCENE_MODE_BARCODE: return "barcode";
    case ACAMERA_CONTROL_SCENE_MODE_HDR: return "hdr";
    default: return "unknown";
    }
}

static void jpeg_device_disconnected(void *opaque, ACameraDevice *device)
{
    (void)device;
    struct jpeg_context *context = opaque;
    atomic_store_explicit(&context->status.device_error, -1,
                          memory_order_release);
}

static void jpeg_device_error(void *opaque, ACameraDevice *device, int error)
{
    (void)device;
    struct jpeg_context *context = opaque;
    atomic_store_explicit(&context->status.device_error,
                          error > 0 ? error : -1, memory_order_release);
}

static void jpeg_image_available(void *opaque, AImageReader *reader)
{
    struct jpeg_context *context = opaque;
    if (atomic_load_explicit(&context->status.image_status,
                             memory_order_acquire) != 0) {
        return;
    }

    AImage *image = NULL;
    media_status_t status = AImageReader_acquireNextImage(reader, &image);
    atomic_store_explicit(&context->status.last_media_status, status,
                          memory_order_release);
    if (status != AMEDIA_OK || !image) {
        return;
    }

    uint8_t *data = NULL;
    int data_length = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    context->image_available_ms = sfos_camera2_now_ms() - context->started_ms;
    if (AImage_getWidth(image, &width) != AMEDIA_OK ||
            AImage_getHeight(image, &height) != AMEDIA_OK ||
            AImage_getFormat(image, &format) != AMEDIA_OK ||
            AImage_getPlaneData(image, 0, &data, &data_length) != AMEDIA_OK ||
            width != context->width || height != context->height ||
            format != AIMAGE_FORMAT_JPEG || !data || data_length <= 0 ||
            sfos_camera2_write_file(context->jpeg_path, data,
                                    (size_t)data_length) != 0) {
        atomic_store_explicit(&context->status.image_status, -1,
                              memory_order_release);
    } else {
        context->data_length = data_length;
        context->file_written_ms = sfos_camera2_now_ms() - context->started_ms;
        atomic_store_explicit(&context->status.image_status, 1,
                              memory_order_release);
    }
    AImage_delete(image);
}

static void jpeg_capture_completed(void *opaque,
                                   ACameraCaptureSession *session,
                                   ACaptureRequest *request,
                                   const ACameraMetadata *result)
{
    (void)session;
    (void)request;
    struct jpeg_context *context = opaque;
    if (!context || !result) {
        return;
    }

    context->actual_sensitivity = sfos_camera2_first_i32(
        result, ACAMERA_SENSOR_SENSITIVITY, -1);
    context->actual_exposure_time_ns = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_EXPOSURE_TIME, -1);
    context->actual_frame_duration_ns = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_FRAME_DURATION, -1);
    context->control_mode_applied = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_MODE, -1);
    context->scene_mode_applied = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_SCENE_MODE, -1);
    atomic_store_explicit(&context->result_status, 1, memory_order_release);
    fprintf(stderr,
            "capture-exposure jpeg requested_iso=%d requested_shutter=%lld "
            "actual_iso=%d actual_shutter=%lld actual_frame=%lld\n",
            context->requested_sensitivity,
            (long long)context->requested_exposure_time_ns,
            context->actual_sensitivity,
            (long long)context->actual_exposure_time_ns,
            (long long)context->actual_frame_duration_ns);
}

static void jpeg_status_json(char *out, size_t out_size, bool success,
                             const char *stage, int code,
                             const struct jpeg_context *context)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size,
             "{\"status\":\"%s\",\"stage\":\"%s\",\"code\":%d,"
             "\"jpeg_path\":\"%s\",\"width\":%d,\"height\":%d,"
             "\"data_length\":%d,"
             "\"sensor_sensitivity_requested\":%d,"
             "\"exposure_time_requested_ns\":%lld,"
             "\"shutter_ns_range\":[%lld,%lld],"
             "\"jpeg_orientation\":%d,"
             "\"aperture_requested\":%d,"
             "\"focal_length_mm\":%.9g,"
             "\"iso\":%d,\"exposure_time_ns\":%lld,"
             "\"frame_duration_ns\":%lld,"
             "\"scene_mode_requested_original\":\"%s\","
             "\"scene_mode_requested\":\"%s\","
             "\"scene_mode_supported\":%s,"
             "\"hdr_scene_supported\":%s,"
             "\"hdr_scene_requested\":%s,"
             "\"hdr_scene_applied\":%s,"
             "\"dol_supported\":false,"
             "\"dol_source\":\"%s\","
             "\"dol_requested\":%s,"
             "\"dol_applied\":false,"
             "\"control_mode_applied\":\"%s\","
             "\"control_mode_applied_value\":%d,"
             "\"scene_mode_applied\":\"%s\","
             "\"scene_mode_applied_value\":%d,"
             "\"scene\":{\"requested_original\":\"%s\","
             "\"requested\":\"%s\",\"supported\":%s,"
             "\"hdr_supported\":%s,\"hdr_requested\":%s,"
             "\"hdr_applied\":%s,"
             "\"control_mode_applied\":\"%s\","
             "\"control_mode_applied_value\":%d,"
             "\"scene_mode_applied\":\"%s\","
             "\"scene_mode_applied_value\":%d},"
             "\"timing_ms\":{\"characteristics\":%lld,"
             "\"reader\":%lld,\"camera_open\":%lld,"
             "\"session\":%lld,\"capture_submit\":%lld,"
             "\"image_available\":%lld,\"file_written\":%lld,"
             "\"total\":%lld},"
             "\"diagnostics\":{\"camera_status\":%d,"
             "\"media_status\":%d,\"device_error\":%d,"
             "\"image_status\":%d}}",
             success ? "ok" : "error", stage ? stage : "unknown", code,
             context && context->jpeg_path ? context->jpeg_path : "",
             context ? context->width : 0, context ? context->height : 0,
             context ? context->data_length : 0,
             context ? context->requested_sensitivity : 0,
             context ? (long long)context->requested_exposure_time_ns : 0,
             context ? (long long)context->exposure_time_range[0] : 0,
             context ? (long long)context->exposure_time_range[1] : 0,
             context ? context->requested_orientation_degrees : 0,
             context ? context->requested_aperture : 0,
             context ? context->focal_length : 0.0f,
             context ? context->actual_sensitivity : 0,
             context ? (long long)context->actual_exposure_time_ns : 0,
             context ? (long long)context->actual_frame_duration_ns : 0,
             context ? jpeg_scene_mode_name(context->requested_scene_mode_original) : "manual",
             context ? jpeg_scene_mode_name(context->requested_scene_mode) : "manual",
             context && context->scene_mode_supported ? "true" : "false",
             context && context->hdr_scene_supported ? "true" : "false",
             context && context->requested_scene_mode_original == SFOS_CAMERA2_SCENE_HDR
                ? "true" : "false",
             context && context->control_mode_applied == ACAMERA_CONTROL_MODE_USE_SCENE_MODE &&
                     context->scene_mode_applied == ACAMERA_CONTROL_SCENE_MODE_HDR
                ? "true" : "false",
             context && context->hdr_scene_supported ? "scene-hdr" : "none",
             context && context->requested_scene_mode_original == SFOS_CAMERA2_SCENE_HDR
                ? "true" : "false",
             context ? jpeg_control_mode_name(context->control_mode_applied) : "unknown",
             context ? context->control_mode_applied : -1,
             context && context->control_mode_applied == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
                ? jpeg_android_scene_mode_name(context->scene_mode_applied) : "none",
             context && context->control_mode_applied == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
                ? context->scene_mode_applied : -1,
             context ? jpeg_scene_mode_name(context->requested_scene_mode_original) : "manual",
             context ? jpeg_scene_mode_name(context->requested_scene_mode) : "manual",
             context && context->scene_mode_supported ? "true" : "false",
             context && context->hdr_scene_supported ? "true" : "false",
             context && context->requested_scene_mode_original == SFOS_CAMERA2_SCENE_HDR
                ? "true" : "false",
             context && context->control_mode_applied == ACAMERA_CONTROL_MODE_USE_SCENE_MODE &&
                     context->scene_mode_applied == ACAMERA_CONTROL_SCENE_MODE_HDR
                ? "true" : "false",
             context ? jpeg_control_mode_name(context->control_mode_applied) : "unknown",
             context ? context->control_mode_applied : -1,
             context && context->control_mode_applied == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
                ? jpeg_android_scene_mode_name(context->scene_mode_applied) : "none",
             context && context->control_mode_applied == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
                ? context->scene_mode_applied : -1,
             context ? (long long)context->characteristics_ms : 0,
             context ? (long long)context->reader_ms : 0,
             context ? (long long)context->camera_open_ms : 0,
             context ? (long long)context->session_ms : 0,
             context ? (long long)context->capture_submit_ms : 0,
             context ? (long long)context->image_available_ms : 0,
             context ? (long long)context->file_written_ms : 0,
             context ? (long long)(sfos_camera2_now_ms() - context->started_ms) : 0,
             context ? atomic_load_explicit(&context->status.last_camera_status,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->status.last_media_status,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->status.device_error,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->status.image_status,
                                            memory_order_acquire) : 0);
}

/**
 * @brief Capture one JPEG image through a short-lived Camera2 session.
 *
 * The function owns the full Camera2 lifecycle for direct JPEG capture:
 * manager, device, ImageReader, session, request, and output file write.
 */
SFOS_CAMERA2_EXPORT int sfos_camera2_capture_jpeg(
    const char *camera_id, int width, int height, const char *jpeg_path,
    int timeout_ms, int jpeg_quality, int orientation_degrees,
    int scene_mode, int sensor_sensitivity, long long exposure_time_ns,
    int aperture, int noise_reduction, float zoom_ratio, char *out,
    size_t out_size)
{
    int result_code = 0;
    const char *stage = "complete";
    ACameraManager *manager = NULL;
    ACameraMetadata *characteristics = NULL;
    ACameraDevice *device = NULL;
    AImageReader *reader = NULL;
    ANativeWindow *window = NULL;
    ACameraOutputTarget *target = NULL;
    ACaptureRequest *request = NULL;
    ACaptureSessionOutput *output = NULL;
    ACaptureSessionOutputContainer *container = NULL;
    ACameraCaptureSession *session = NULL;

    struct jpeg_context context;
    memset(&context, 0, sizeof(context));
    sfos_camera2_status_init(&context.status);
    context.width = width;
    context.height = height;
    context.jpeg_path = jpeg_path;
    context.started_ms = sfos_camera2_now_ms();
    context.requested_sensitivity = sensor_sensitivity;
    context.requested_exposure_time_ns = exposure_time_ns;
    context.exposure_time_range[0] = 0;
    context.exposure_time_range[1] = 0;
    context.requested_orientation_degrees = orientation_degrees;
    context.requested_aperture = aperture;
    context.requested_scene_mode_original = scene_mode;
    context.requested_scene_mode = scene_mode;
    context.scene_mode_supported = 1;
    context.hdr_scene_supported = 0;
    context.control_mode_applied = -1;
    context.scene_mode_applied = -1;

    if (!camera_id || !*camera_id || width <= 0 || height <= 0 ||
            !jpeg_path || !*jpeg_path || timeout_ms < 1000 ||
            jpeg_quality < 1 || jpeg_quality > 100 ||
            scene_mode < SFOS_CAMERA2_SCENE_NONE ||
            scene_mode > SFOS_CAMERA2_SCENE_HDR ||
            sensor_sensitivity < 0 ||
            exposure_time_ns < 0 ||
            aperture < 0 || aperture > 255 ||
            noise_reduction < 0 ||
            !out || out_size < 2) {
        jpeg_status_json(out, out_size, false, "arguments",
                         JPEG_INVALID_ARGUMENT, &context);
        return JPEG_INVALID_ARGUMENT;
    }

    manager = ACameraManager_create();
    if (!manager) {
        result_code = JPEG_MANAGER_ERROR;
        stage = "manager_create";
        goto cleanup;
    }
    camera_status_t camera_status = ACameraManager_getCameraCharacteristics(
        manager, camera_id, &characteristics);
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK || !characteristics) {
        result_code = JPEG_CHARACTERISTICS_ERROR;
        stage = "characteristics";
        goto cleanup;
    }
    context.characteristics_ms = sfos_camera2_now_ms() - context.started_ms;
    context.focal_length = sfos_camera2_first_float(
        characteristics, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, 0.0f);
    sfos_camera2_copy_i64_array(
        characteristics, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE,
        context.exposure_time_range, 2);
    if (!sfos_camera2_has_output_size(
            characteristics, AIMAGE_FORMAT_JPEG, width, height)) {
        result_code = JPEG_UNSUPPORTED_SIZE;
        stage = "jpeg_size";
        goto cleanup;
    }
    context.scene_mode_supported =
        sfos_camera2_scene_mode_supported(characteristics, scene_mode);
    context.hdr_scene_supported = sfos_camera2_metadata_has_u8(
        characteristics, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES,
        ACAMERA_CONTROL_SCENE_MODE_HDR);
    if (!context.scene_mode_supported) {
        scene_mode = SFOS_CAMERA2_SCENE_NONE;
    }
    context.requested_scene_mode = scene_mode;

    media_status_t media_status = AImageReader_new(
        width, height, AIMAGE_FORMAT_JPEG, 2, &reader);
    atomic_store_explicit(&context.status.last_media_status, media_status,
                          memory_order_release);
    if (media_status != AMEDIA_OK || !reader) {
        result_code = JPEG_READER_ERROR;
        stage = "reader_create";
        goto cleanup;
    }
    AImageReader_ImageListener listener = {
        .context = &context,
        .onImageAvailable = jpeg_image_available,
    };
    media_status = AImageReader_setImageListener(reader, &listener);
    atomic_store_explicit(&context.status.last_media_status, media_status,
                          memory_order_release);
    if (media_status == AMEDIA_OK) {
        media_status = AImageReader_getWindow(reader, &window);
        atomic_store_explicit(&context.status.last_media_status, media_status,
                              memory_order_release);
    }
    if (media_status != AMEDIA_OK || !window) {
        result_code = JPEG_READER_ERROR;
        stage = "reader_configure";
        goto cleanup;
    }
    context.reader_ms = sfos_camera2_now_ms() - context.started_ms;

    ACameraDevice_StateCallbacks device_callbacks = {
        .context = &context,
        .onDisconnected = jpeg_device_disconnected,
        .onError = jpeg_device_error,
    };
    camera_status = ACameraManager_openCamera(
        manager, camera_id, &device_callbacks, &device);
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK || !device) {
        result_code = JPEG_OPEN_ERROR;
        stage = "camera_open";
        goto cleanup;
    }
    context.camera_open_ms = sfos_camera2_now_ms() - context.started_ms;

    if (ACameraOutputTarget_create(window, &target) != ACAMERA_OK ||
            ACameraDevice_createCaptureRequest(device, TEMPLATE_STILL_CAPTURE,
                                               &request) != ACAMERA_OK ||
            ACaptureRequest_addTarget(request, target) != ACAMERA_OK ||
            ACaptureSessionOutput_create(window, &output) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_create(&container) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_add(container, output) !=
                ACAMERA_OK) {
        result_code = JPEG_CONFIGURATION_ERROR;
        stage = "configure";
        goto cleanup;
    }

    uint8_t quality = (uint8_t)jpeg_quality;
    int32_t orientation = orientation_degrees;
    uint8_t af_mode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    if (!sfos_camera2_metadata_has_u8(
            characteristics, ACAMERA_CONTROL_AF_AVAILABLE_MODES, af_mode)) {
        af_mode = ACAMERA_CONTROL_AF_MODE_OFF;
    }
    ACaptureRequest_setEntry_u8(request, ACAMERA_JPEG_QUALITY, 1, &quality);
    ACaptureRequest_setEntry_i32(request, ACAMERA_JPEG_ORIENTATION, 1,
                                 &orientation);
    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1,
                                &af_mode);
    sfos_camera2_set_scene_mode(request, scene_mode);
    if (!sfos_camera2_set_manual_sensor(characteristics, request,
                                       sensor_sensitivity, exposure_time_ns)) {
        result_code = JPEG_CONFIGURATION_ERROR;
        stage = "exposure_controls";
        goto cleanup;
    }
    sfos_camera2_set_aperture(characteristics, request, aperture);
    sfos_camera2_set_noise_reduction(characteristics, request,
                                     noise_reduction);
    sfos_camera2_set_zoom_ratio(characteristics, request, zoom_ratio);

    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = &context,
        .onClosed = NULL,
        .onReady = NULL,
        .onActive = NULL,
    };
    camera_status = ACameraDevice_createCaptureSession(
        device, container, &session_callbacks, &session);
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK || !session) {
        result_code = JPEG_CONFIGURATION_ERROR;
        stage = "session_create";
        goto cleanup;
    }
    context.session_ms = sfos_camera2_now_ms() - context.started_ms;

    ACameraCaptureSession_captureCallbacks capture_callbacks = {
        .context = &context,
        .onCaptureStarted = NULL,
        .onCaptureProgressed = NULL,
        .onCaptureCompleted = jpeg_capture_completed,
        .onCaptureFailed = NULL,
        .onCaptureSequenceCompleted = NULL,
        .onCaptureSequenceAborted = NULL,
        .onCaptureBufferLost = NULL,
    };
    ACaptureRequest *requests[] = { request };
    camera_status = ACameraCaptureSession_capture(
        session, &capture_callbacks, 1, requests, NULL);
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK) {
        result_code = JPEG_SUBMIT_ERROR;
        stage = "capture_submit";
        goto cleanup;
    }
    context.capture_submit_ms = sfos_camera2_now_ms() - context.started_ms;

    int64_t deadline = sfos_camera2_now_ms() + timeout_ms;
    while (sfos_camera2_now_ms() < deadline &&
            atomic_load_explicit(&context.status.device_error,
                                 memory_order_acquire) == 0 &&
            atomic_load_explicit(&context.status.image_status,
                                 memory_order_acquire) == 0) {
        sfos_camera2_sleep_10_ms();
    }
    if (atomic_load_explicit(&context.status.image_status,
                             memory_order_acquire) < 0) {
        result_code = JPEG_WRITE_ERROR;
        stage = "image_write";
        goto cleanup;
    }
    if (atomic_load_explicit(&context.status.image_status,
                             memory_order_acquire) == 0) {
        result_code = JPEG_TIMEOUT;
        stage = "image_wait";
        goto cleanup;
    }
    int64_t result_deadline = sfos_camera2_now_ms() + 500;
    while (atomic_load_explicit(&context.result_status,
                                memory_order_acquire) == 0 &&
            sfos_camera2_now_ms() < result_deadline) {
        sfos_camera2_sleep_10_ms();
    }
    jpeg_log_effective_capture(&context);

cleanup:
    jpeg_status_json(out, out_size, result_code == 0, stage, result_code,
                     &context);
    if (session) {
        ACameraCaptureSession_close(session);
    }
    if (target) {
        ACameraOutputTarget_free(target);
    }
    if (output) {
        ACaptureSessionOutput_free(output);
    }
    if (container) {
        ACaptureSessionOutputContainer_free(container);
    }
    if (request) {
        ACaptureRequest_free(request);
    }
    if (reader) {
        AImageReader_setImageListener(reader, NULL);
        AImageReader_delete(reader);
    }
    if (device) {
        ACameraDevice_close(device);
    }
    if (characteristics) {
        ACameraMetadata_free(characteristics);
    }
    if (manager) {
        ACameraManager_delete(manager);
    }
    return result_code;
}
