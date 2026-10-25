/*
 * LevelSensor.qml -- reads the accelerometer for the level (roll and tilt).
 * Loaded through a Loader by indraw.qml so that a device without the QtSensors
 * QML module still runs the app (the level is then simply unavailable).
 *
 * Gravity in the device frame (Qt: x right, y up, z out of the screen; at rest
 * the accelerometer reads the reaction to gravity, i.e. "up"):
 *   roll  = -atan2(x, y)     0 when the phone stands upright, in degrees
 *                            (sign checked on the device: the first,
 *                            unsigned version turned the line the wrong way)
 *   pitch = atan2(z, sqrt(x² + y²))
 *                            0 when the phone is vertical, positive when the
 *                            screen faces up (the back camera points down)
 * A low-pass filter and a small dead band steady the readings.
 */
import QtQuick 2.6
import QtSensors 5.0

Item {
    id: level
    property bool active: false
    property real roll: 0          // degrees
    property real pitch: 0         // degrees
    property bool valid: false
    property int readings: 0       // readings received since activation (diagnostics)
    property real fRoll: 0         // the filtered readings (roll / pitch show them
    property real fPitch: 0        // through the dead band)

    Accelerometer {
        id: acc
        active: level.active
        dataRate: 25
        onReadingChanged: {
            var x = reading.x, y = reading.y, z = reading.z
            // Sign measured on the JP2: atan2(x, y) turned the line the wrong way.
            var r = -Math.atan2(x, y) * 180 / Math.PI
            var p = Math.atan2(z, Math.sqrt(x * x + y * y)) * 180 / Math.PI
            // Low-pass (0.12 of the new reading at 25 Hz: about a third of a
            // second to follow the hand, steady against the sensor's noise --
            // 0.25 trembled on the JP2), then a dead band of 0.05 degrees on
            // what is shown, so the residual noise moves neither the cross nor
            // the degrees (ui 176). Roll wraps at +-180.
            var dr = r - level.fRoll
            if (dr > 180) dr -= 360
            if (dr < -180) dr += 360
            level.fRoll = level.valid ? level.fRoll + 0.12 * dr : r
            level.fPitch = level.valid ? level.fPitch + 0.12 * (p - level.fPitch) : p
            if (!level.valid || Math.abs(level.fRoll - level.roll) > 0.05) level.roll = level.fRoll
            if (!level.valid || Math.abs(level.fPitch - level.pitch) > 0.05) level.pitch = level.fPitch
            level.valid = true
            level.readings += 1
        }
    }
    onActiveChanged: { if (!active) valid = false; readings = 0 }
}
