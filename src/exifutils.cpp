// SPDX-FileCopyrightText: 2026 RAWfish Contributors
//
// SPDX-License-Identifier: BSD-3-Clause

#include "exifutils.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QtGlobal>

#include <climits>
#include <cmath>

namespace {

struct ExifEntry {
    quint16 tag = 0;
    quint16 type = 0;
    quint32 count = 0;
    QByteArray value;
};

void appendU16(QByteArray *data, quint16 value)
{
    data->append(char(value & 0xff));
    data->append(char((value >> 8) & 0xff));
}

void appendU32(QByteArray *data, quint32 value)
{
    data->append(char(value & 0xff));
    data->append(char((value >> 8) & 0xff));
    data->append(char((value >> 16) & 0xff));
    data->append(char((value >> 24) & 0xff));
}

QByteArray asciiValue(const QByteArray &value)
{
    QByteArray out = value;
    if (out.isEmpty() || out.at(out.size() - 1) != '\0') {
        out.append('\0');
    }
    return out;
}

QByteArray shortValue(quint16 value)
{
    QByteArray out;
    appendU16(&out, value);
    return out;
}

QByteArray longValue(quint32 value)
{
    QByteArray out;
    appendU32(&out, value);
    return out;
}

QByteArray rationalValue(quint32 numerator, quint32 denominator)
{
    QByteArray out;
    appendU32(&out, numerator);
    appendU32(&out, denominator ? denominator : 1);
    return out;
}

int metadataInt(const QJsonObject &metadata, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = metadata.value(key);
        if (value.isDouble()) {
            const int integer = value.toInt();
            if (integer > 0) {
                return integer;
            }
        } else if (value.isString()) {
            bool ok = false;
            const int integer = value.toString().toInt(&ok);
            if (ok && integer > 0) {
                return integer;
            }
        }
    }
    return 0;
}

qint64 metadataInt64(const QJsonObject &metadata, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = metadata.value(key);
        if (value.isDouble()) {
            const qint64 integer = qint64(value.toDouble());
            if (integer > 0) {
                return integer;
            }
        } else if (value.isString()) {
            bool ok = false;
            const qint64 integer = value.toString().toLongLong(&ok);
            if (ok && integer > 0) {
                return integer;
            }
        }
    }
    return 0;
}

double metadataDouble(const QJsonObject &metadata, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = metadata.value(key);
        if (value.isDouble()) {
            const double number = value.toDouble();
            if (number > 0.0) {
                return number;
            }
        } else if (value.isString()) {
            bool ok = false;
            const double number = value.toString().toDouble(&ok);
            if (ok && number > 0.0) {
                return number;
            }
        }
    }
    return 0.0;
}

quint16 orientationFromDegrees(int degrees)
{
    degrees %= 360;
    if (degrees < 0) {
        degrees += 360;
    }
    switch (degrees) {
    case 90: return 6;
    case 180: return 3;
    case 270: return 8;
    default: return 1;
    }
}

bool metadataPixelsRotated(const QJsonObject &metadata)
{
    const QJsonValue pixelsRotated = metadata.value(QStringLiteral("pixels_rotated"));
    if (pixelsRotated.isBool()) {
        return pixelsRotated.toBool();
    }
    if (pixelsRotated.isString()) {
        return pixelsRotated.toString() == QLatin1String("true") ||
                pixelsRotated.toString() == QLatin1String("1");
    }
    const QString captureSource =
            metadata.value(QStringLiteral("capture_source")).toString();
    return captureSource == QLatin1String("camera2_preview_frame") ||
            metadata.value(QStringLiteral("format")).toString() ==
                    QLatin1String("RAW16");
}

QByteArray buildIfd(QByteArray *tiff, const QList<ExifEntry> &entries)
{
    QByteArray ifd;
    QByteArray extra;
    appendU16(&ifd, quint16(entries.size()));
    const quint32 dataBaseOffset = quint32(tiff->size() + 2 + entries.size() * 12 + 4);
    for (const ExifEntry &entry : entries) {
        appendU16(&ifd, entry.tag);
        appendU16(&ifd, entry.type);
        appendU32(&ifd, entry.count);
        if (entry.value.size() <= 4) {
            QByteArray value = entry.value;
            while (value.size() < 4) {
                value.append('\0');
            }
            ifd.append(value);
        } else {
            appendU32(&ifd, dataBaseOffset + quint32(extra.size()));
            extra.append(entry.value);
            if (extra.size() % 2) {
                extra.append('\0');
            }
        }
    }
    appendU32(&ifd, 0);
    ifd.append(extra);
    tiff->append(ifd);
    return ifd;
}

void patchU32(QByteArray *data, int offset, quint32 value)
{
    if (offset < 0 || offset + 4 > data->size()) {
        return;
    }
    (*data)[offset] = char(value & 0xff);
    (*data)[offset + 1] = char((value >> 8) & 0xff);
    (*data)[offset + 2] = char((value >> 16) & 0xff);
    (*data)[offset + 3] = char((value >> 24) & 0xff);
}

QByteArray buildExifPayload(const QJsonObject &metadata)
{
    const int width = metadataInt(metadata, { QStringLiteral("width") });
    const int height = metadataInt(metadata, { QStringLiteral("height") });
    const int iso = metadataInt(metadata, {
        QStringLiteral("iso"),
        QStringLiteral("live_sensor_sensitivity"),
        QStringLiteral("sensor_sensitivity_requested"),
        QStringLiteral("iso_requested")
    });
    const qint64 exposureNs = metadataInt64(metadata, {
        QStringLiteral("exposure_time_ns"),
        QStringLiteral("live_exposure_time_ns"),
        QStringLiteral("exposure_time_requested_ns")
    });
    double aperture = metadataDouble(metadata, {
        QStringLiteral("lens_aperture")
    });
    if (aperture <= 0.0) {
        const int apertureTenths = metadataInt(metadata, {
            QStringLiteral("aperture_requested")
        });
        aperture = apertureTenths > 0 ? apertureTenths / 10.0 : 0.0;
    }
    const double focalLength = metadataDouble(metadata, {
        QStringLiteral("focal_length_mm")
    });

    const QByteArray now = QDateTime::currentDateTime()
            .toString(QStringLiteral("yyyy:MM:dd hh:mm:ss")).toLatin1();
    const QByteArray cameraId = metadata.value(QStringLiteral("camera_id"))
            .toString().toLatin1();
    const QByteArray model = cameraId.isEmpty()
            ? QByteArrayLiteral("RAWfish Camera2")
            : QByteArrayLiteral("RAWfish Camera2 camera ") + cameraId;

    QByteArray tiff;
    tiff.append("II", 2);
    appendU16(&tiff, 42);
    appendU32(&tiff, 8);

    QList<ExifEntry> ifd0;
    if (width > 0) {
        ifd0.append(ExifEntry{ 0x0100, 4, 1, longValue(quint32(width)) });
    }
    if (height > 0) {
        ifd0.append(ExifEntry{ 0x0101, 4, 1, longValue(quint32(height)) });
    }
    ifd0.append(ExifEntry{ 0x010f, 2, 8, asciiValue(QByteArrayLiteral("RAWfish")) });
    const QByteArray modelValue = asciiValue(model);
    ifd0.append(ExifEntry{ 0x0110, 2, quint32(modelValue.size()), modelValue });
    const int orientation = metadataPixelsRotated(metadata)
            ? 0
            : metadataInt(metadata, {
                  QStringLiteral("jpeg_orientation"),
                  QStringLiteral("orientation")
              });
    ifd0.append(ExifEntry{ 0x0112, 3, 1, shortValue(orientationFromDegrees(orientation)) });
    ifd0.append(ExifEntry{ 0x0131, 2, 15, asciiValue(QByteArrayLiteral("RAWfish 1.3.1")) });
    ifd0.append(ExifEntry{ 0x0132, 2, 20, asciiValue(now) });
    const int exifPointerValueOffset = 8 + 2 + ifd0.size() * 12 + 8;
    ifd0.append(ExifEntry{ 0x8769, 4, 1, longValue(0) });
    buildIfd(&tiff, ifd0);

    const quint32 exifOffset = quint32(tiff.size());
    QList<ExifEntry> exif;
    if (exposureNs > 0) {
        exif.append(ExifEntry{ 0x829a, 5, 1,
                               rationalValue(quint32(qMin<qint64>(exposureNs, UINT_MAX)),
                                             1000000000U) });
    }
    if (aperture > 0.0) {
        exif.append(ExifEntry{ 0x829d, 5, 1,
                               rationalValue(quint32(std::lround(aperture * 100.0)), 100) });
    }
    if (iso > 0) {
        exif.append(ExifEntry{ 0x8827, 3, 1, shortValue(quint16(qMin(iso, 65535))) });
    }
    exif.append(ExifEntry{ 0x9003, 2, 20, asciiValue(now) });
    if (focalLength > 0.0) {
        exif.append(ExifEntry{ 0x920a, 5, 1,
                               rationalValue(quint32(std::lround(focalLength * 100.0)), 100) });
    }
    if (width > 0) {
        exif.append(ExifEntry{ 0xa002, 4, 1, longValue(quint32(width)) });
    }
    if (height > 0) {
        exif.append(ExifEntry{ 0xa003, 4, 1, longValue(quint32(height)) });
    }
    buildIfd(&tiff, exif);
    patchU32(&tiff, exifPointerValueOffset, exifOffset);

    QByteArray payload("Exif\0\0", 6);
    payload.append(tiff);
    return payload;
}

bool isExifApp1(const QByteArray &jpeg, int offset, int segmentLength)
{
    return segmentLength >= 8 &&
            offset + 4 + 6 <= jpeg.size() &&
            jpeg.mid(offset + 4, 6) == QByteArray("Exif\0\0", 6);
}

bool rewriteJpegWithExif(const QString &jpegPath, const QByteArray &exifPayload,
                         QString *error)
{
    QFile file(jpegPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read JPEG for EXIF: %1").arg(jpegPath);
        return false;
    }
    QByteArray jpeg = file.readAll();
    file.close();

    if (jpeg.size() < 4 || uchar(jpeg.at(0)) != 0xff || uchar(jpeg.at(1)) != 0xd8) {
        *error = QStringLiteral("Cannot add EXIF to non-JPEG file: %1").arg(jpegPath);
        return false;
    }
    if (exifPayload.size() + 2 > 65535) {
        *error = QStringLiteral("EXIF block is too large");
        return false;
    }

    QByteArray output;
    output.append(jpeg.left(2));
    int offset = 2;
    while (offset + 4 <= jpeg.size() && uchar(jpeg.at(offset)) == 0xff) {
        const uchar marker = uchar(jpeg.at(offset + 1));
        if (marker == 0xda || marker == 0xd9) {
            break;
        }
        const int segmentLength =
                (uchar(jpeg.at(offset + 2)) << 8) | uchar(jpeg.at(offset + 3));
        if (segmentLength < 2 || offset + 2 + segmentLength > jpeg.size()) {
            break;
        }
        if (!isExifApp1(jpeg, offset, segmentLength)) {
            output.append(jpeg.mid(offset, 2 + segmentLength));
        }
        offset += 2 + segmentLength;
    }

    output.append(char(0xff));
    output.append(char(0xe1));
    const quint16 app1Length = quint16(exifPayload.size() + 2);
    output.append(char((app1Length >> 8) & 0xff));
    output.append(char(app1Length & 0xff));
    output.append(exifPayload);
    output.append(jpeg.mid(offset));

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Cannot write JPEG EXIF: %1").arg(jpegPath);
        return false;
    }
    if (file.write(output) != output.size()) {
        *error = QStringLiteral("Could not write complete JPEG EXIF: %1").arg(jpegPath);
        return false;
    }
    return true;
}

}

namespace ExifUtils {

bool writeJpegExifFromJsonFile(const QString &jpegPath,
                               const QString &metadataPath,
                               QString *error)
{
    QFile metadataFile(metadataPath);
    if (!metadataFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read EXIF metadata: %1").arg(metadataPath);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
            QJsonDocument::fromJson(metadataFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("EXIF metadata is not valid JSON: %1").arg(metadataPath);
        return false;
    }
    return rewriteJpegWithExif(jpegPath, buildExifPayload(document.object()), error);
}

}
