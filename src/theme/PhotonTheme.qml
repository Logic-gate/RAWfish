// SPDX-License-Identifier: BSD-3-Clause
//
// Design tokens that follow PhotonCamera's look (bjzhou/PhotonCamera): near-black
// surfaces, white text at three emphasis levels, orange for selected state and
// yellow for live HUD values. Values come from its theme/Color.kt and
// CameraControlsLayoutDefaults, and from colours sampled off its store screenshots.
pragma Singleton
import QtQuick 2.0
import QtQuick.Window 2.0

QtObject {
    // Surfaces
    readonly property color background: "#0D0D0D"
    readonly property color surface: "#1A1A1A"
    readonly property color surfaceVariant: "#2D2D2D"
    readonly property color scrim: "#99000000"
    readonly property color overlayPill: "#66000000"

    // Text
    readonly property color textPrimary: "#FFFFFF"
    readonly property color textSecondary: "#B3FFFFFF"   // 70% white
    readonly property color textTertiary: "#80FFFFFF"    // 50% white

    // Accents
    readonly property color accent: "#F86830"            // selected state, primary buttons
    readonly property color textOnAccent: "#FFFFFF"
    readonly property color hud: "#FFD700"               // active HUD values, histogram, EV tick
    readonly property color hudDim: "#99B89A00"
    readonly property color premiumGold: "#FFD700"
    readonly property color success: "#4CAF50"
    readonly property color error: "#E53935"
    readonly property color warning: "#FFC107"

    // Shape and spacing (device-independent pixels from Photon, scaled by `dp`)
    readonly property real dp: Math.max(1.0, Screen.pixelDensity * 25.4 / 160.0)
    readonly property real radiusSmall: 8 * dp
    readonly property real radiusMedium: 12 * dp
    readonly property real radiusLarge: 20 * dp
    readonly property real spacingSmall: 4 * dp
    readonly property real spacing: 8 * dp
    readonly property real spacingLarge: 16 * dp

    // Camera control sizes from CameraControlsLayoutDefaults
    readonly property real parameterBarHeight: 48 * dp
    readonly property real zoomBarHeight: 32 * dp
    readonly property real captureButtonSize: 72 * dp
    readonly property real sideButtonSize: 48 * dp
    readonly property real modeSwitcherHeight: 36 * dp

    // Typography
    readonly property int fontSizeCaption: Math.round(11 * dp)
    readonly property int fontSizeBody: Math.round(14 * dp)
    readonly property int fontSizeTitle: Math.round(18 * dp)
}
