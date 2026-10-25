/*
 * OrientationSensor.qml -- the phone's PHYSICAL rotation, from gravity, for
 * the photos' orientation (engine "orient"). Loaded through a Loader like
 * LevelSensor.qml, so a device without QtSensors still runs the app (the
 * photos then keep the sensor's mounting orientation only).
 *
 * Independent of the screen: the OS may lock its orientation, the page may
 * stay in portrait; a photo must still come out upright on the ground.
 * Qt's reading: TopUp = natural portrait (0), LeftUp = the left edge is up,
 * i.e. the phone turned 90 degrees clockwise (90), TopDown (180), RightUp
 * (270); FaceUp / FaceDown keep the last known rotation.
 */
import QtQuick 2.6
import QtSensors 5.0

Item {
    id: sensor
    property bool active: false
    property int degrees: 0                 // clockwise from natural portrait
    property bool valid: false

    OrientationSensor {
        id: os
        active: sensor.active
        onReadingChanged: {
            var o = reading.orientation
            var d = o === OrientationReading.TopUp ? 0
                  : o === OrientationReading.LeftUp ? 90
                  : o === OrientationReading.TopDown ? 180
                  : o === OrientationReading.RightUp ? 270 : -1
            if (d < 0) return                           // face up/down: keep the last
            sensor.degrees = d
            sensor.valid = true
        }
    }
}
