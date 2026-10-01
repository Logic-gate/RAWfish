// SPDX-License-Identifier: BSD-3-Clause
.pragma library

function stops(ratio) { return Math.log(ratio) / Math.LN2 }
function clamp(value, low, high) { return Math.max(low, Math.min(high, value)) }
function compensation(ev, caps) {
    var step = Number(caps.compensation_step_ev)
    var range = caps.compensation_range || [0, 0]
    if (!(step > 0)) return { steps: 0, ev: 0 }
    var steps = clamp(Math.round(ev / step), range[0], range[1])
    return { steps: steps, ev: steps * step }
}
function limits(caps) {
    var iso = caps.iso_range || [0, 0]
    var shutter = caps.shutter_ns_range || [0, 0]
    var maximum = Number(shutter[1])
    if (caps.max_frame_duration_ns > 0)
        maximum = Math.min(maximum, Number(caps.max_frame_duration_ns) - 1000000)
    return { isoMin: Number(iso[0]), isoMax: Number(iso[1]),
             timeMin: Number(shutter[0]), timeMax: maximum }
}
function validLimits(caps) {
    var l = limits(caps)
    return l.isoMin > 0 && l.isoMax >= l.isoMin && l.timeMin > 0 && l.timeMax >= l.timeMin
}
// ISO*time is an exposure-product approximation, not photons collected.
function solve(product, isoLock, timeLock, caps, flicker) {
    var l = limits(caps)
    var iso = isoLock, time = timeLock
    var parameter = timeLock > 0 ? "ISO" : "Shutter"
    var wanted = timeLock > 0 ? product / timeLock : product / isoLock
    var low = timeLock > 0 ? l.isoMin : l.timeMin
    var high = timeLock > 0 ? l.isoMax : l.timeMax
    var atLimit = wanted < low ? "minimum" : wanted > high ? "maximum" : ""
    var value = clamp(wanted, low, high)
    if (timeLock > 0) iso = Math.round(value)
    else {
        var period = flicker === 1 ? 10000000 : flicker === 2 ? 1000000000 / 120 : 0
        if (period && value >= period && high >= period) {
            var first = Math.max(1, Math.ceil(low / period))
            var last = Math.floor(high / period)
            if (first <= last) value = clamp(Math.round(value / period), first, last) * period
        }
        time = Math.round(value)
    }
    return { iso: iso, time: time, limit: atLimit ? parameter + " " + atLimit : "",
             residual: stops(product / (iso * time)) }
}
function samePair(iso, time, actualIso, actualTime) {
    return iso > 0 && time > 0 && actualIso > 0 && actualTime > 0 &&
            Math.abs(stops(actualIso / iso)) < 0.08 && Math.abs(stops(actualTime / time)) < 0.08
}
