// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0

// One zoom writer for button motion and direct viewfinder gestures.
Timer {
    id: controller

    property real zoom: 1.0
    property real maximumZoom: 1.0
    property bool allowed: true
    readonly property real limit: isFinite(maximumZoom) ? Math.max(1, maximumZoom) : 1
    property int _direction: 0
    property bool _held: false
    property real _started: 0
    property real _from: 1
    property real _target: 1

    signal zoomRequested(real value)

    interval: 33
    repeat: true
    onTriggered: advance(Date.now())
    onAllowedChanged: if (!allowed) cancel()
    onLimitChanged: {
        cancel()
        if (zoom > limit) applyZoom(limit)
    }

    function applyZoom(value) {
        if (isFinite(value)) zoomRequested(Math.max(1, Math.min(limit, value)))
    }

    function setDirect(value) {
        cancel()
        applyZoom(value)
    }

    function press(direction) {
        if (!allowed || limit <= 1) return
        // Accumulate quick taps without discarding their unfinished steps.
        var base = running && direction === _direction ? _target : zoom
        cancel()
        _direction = direction
        _held = true
        _from = Math.max(1, zoom)
        _target = Math.max(1, Math.min(limit, base * Math.pow(1.1, direction)))
        _started = Date.now()
        start()
    }

    function release() {
        if (!_held) return
        _held = false
        if (Date.now() - _started >= 350) cancel()
    }

    function cancel() {
        stop()
        _held = false
        _direction = 0
    }

    function advance(now) {
        if (!running) return
        var elapsed = Math.max(0, now - _started)
        if (_held && elapsed >= 350) {
            applyZoom(_target * Math.pow(2, _direction * (elapsed - 350) / 1000))
            if ((_direction > 0 && zoom >= limit) || (_direction < 0 && zoom <= 1)) cancel()
        } else {
            var progress = Math.min(1, elapsed / 180)
            var eased = progress * progress * (3 - 2 * progress)
            applyZoom(progress === 1 ? _target : _from * Math.pow(_target / _from, eased))
            if (progress === 1 && !_held) cancel()
        }
    }
}
