// SPDX-License-Identifier: BSD-3-Clause
.pragma library

// External key: [settings property, value type, minimum, maximum].
var schema = {
    capture_format: ["camera2CaptureFormat", "string"],
    scene: ["rawCaptureScene", "string"],
    capture_size: ["rawCaptureSize", "string"],
    speed_mode: ["rawCaptureSpeedMode", "string"],
    focus_mode: ["rawCaptureFocusMode", "string"],
    focus_distance_diopters: ["rawCaptureFocusDistance", "decimal", 0],
    capture_timeout: ["rawCaptureTimeout", "integer", 1, 3600],
    focus_timeout: ["rawCaptureFocusTimeout", "integer", 0, 3600],
    focus_failure: ["rawCaptureFocusFailure", "string"],
    exposure_multiplier: ["rawCaptureExposure", "decimal", 0],
    iso: ["rawCaptureIso", "integer", 0, 2147483647],
    shutter_ns: ["rawCaptureShutterNs", "decimal", 0],
    aperture_tenths: ["rawCaptureAperture", "integer", 0, 255],
    noise_reduction: ["rawCaptureNoiseReduction", "integer", 0, 2147483647],
    bracket: ["rawCaptureBracket", "string"],
    jpeg_quality: ["rawCaptureJpegQuality", "integer", 1, 100],
    progressive_jpeg: ["rawCaptureProgressiveJpeg", "boolean"],
    render_engine: ["rawRenderEngine", "string"],
    raw_format: ["rawCaptureRawFormat", "string"],
    color_temperature: ["rawCaptureColorTemperature", "integer", 0, 50000],
    color_tint: ["rawCaptureColorTint", "integer", -1000, 1000]
}

function typedValue(key, value) {
    var rule = schema[key]
    if (rule[1] === "string" || rule[1] === "boolean")
        return typeof value === rule[1] ? value : undefined
    if (rule[1] === "integer" && typeof value !== "number") return undefined
    if (rule[1] === "decimal" && typeof value !== "number" && typeof value !== "string") return undefined
    if (typeof value === "string" && !/^(?:\d+\.?\d*|\.\d+)$/.test(value)) return undefined
    var numeric = Number(value)
    if (!isFinite(numeric) || numeric < rule[2] || (rule.length > 3 && numeric > rule[3])) return undefined
    if ((rule[1] === "integer" || key === "shutter_ns") && Math.floor(numeric) !== numeric) return undefined
    if (key === "shutter_ns" && numeric > 9007199254740991) return undefined
    if (key === "exposure_multiplier" && numeric <= 0) return undefined
    return rule[1] === "decimal" ? String(numeric) : numeric
}

function matchedValue(key, value, choices) {
    value = typedValue(key, value)
    if (value === undefined || choices === undefined) return value
    for (var i = 0; i < choices.length; ++i) {
        if (schema[key][1] === "decimal" ? Number(choices[i]) === Number(value) : choices[i] === value)
            return choices[i]
    }
    return undefined
}

function select(key, overrides, fallback, choices) {
    var value = matchedValue(key, overrides[key], choices)
    if (value !== undefined) return value
    value = matchedValue(key, fallback, choices)
    return value !== undefined ? value : choices && choices.length ? choices[0] : fallback
}

function resolve(defaults, overrides, choices) {
    var result = {}
    for (var key in schema) {
        var property = schema[key][0]
        result[property] = select(key, overrides, defaults[property], choices[key])
    }
    return result
}
