// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#ifndef EXIFUTILS_H
#define EXIFUTILS_H

#include <QJsonObject>
#include <QVariantMap>
#include <tiffio.h>

namespace ExifUtils {
int orientation(const QJsonObject &metadata, bool dng);
QJsonObject captureContext(const QVariantMap &values);
QJsonObject mergeCaptureMetadata(const QJsonObject &result, const QJsonObject &context);
bool enrichSidecar(const QString &path, const QJsonObject &context, QString *error);
QByteArray originalJpegExif(const QString &path);
bool writeJpegExifFromJsonFile(const QString &jpegPath, const QString &metadataPath, QString *error);
bool writeDngMetadata(TIFF *tiff, const QJsonObject &metadata, QString *error);
}
#endif
