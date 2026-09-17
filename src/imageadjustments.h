// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef IMAGEADJUSTMENTS_H
#define IMAGEADJUSTMENTS_H

#include <QImage>
#include <QtGlobal>

namespace ImageAdjustments {

/**
 * Applies simple post-capture exposure, temperature and tint adjustments.
 */
void apply(QImage *image, qreal exposure, int colorTemperature, int colorTint);

/**
 * Returns true when any post-capture image adjustment is requested.
 */
bool requested(qreal exposure, int colorTemperature, int colorTint);

}

#endif
