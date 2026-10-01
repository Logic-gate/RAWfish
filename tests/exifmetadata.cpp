// SPDX-License-Identifier: BSD-3-Clause
#include "../src/exifutils.h"
#include <QtTest>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <memory>

using Exif = std::unique_ptr<ExifData, decltype(&exif_data_unref)>;
static Exif readExif(const QString &path)
{
    return Exif(exif_data_new_from_file(QFile::encodeName(path).constData()), exif_data_unref);
}
static QJsonObject details(ExifData *data)
{
    ExifEntry *entry = exif_content_get_entry(data->ifd[EXIF_IFD_EXIF], EXIF_TAG_USER_COMMENT);
    if (!entry || entry->size < 8) return {};
    return QJsonDocument::fromJson(QByteArray(reinterpret_cast<char *>(entry->data + 8), entry->size - 8)).object();
}
static bool writePhoto(const QString &path, const QJsonObject &metadata, QString *error)
{
    if (!QFile::exists(path)) {
        QImage image(4, 6, QImage::Format_RGB32);
        image.fill(Qt::green);
        if (!image.save(path, "JPEG")) return false;
    }
    QFile json(path + ".json");
    if (!json.open(QIODevice::WriteOnly)) return false;
    json.write(QJsonDocument(metadata).toJson());
    json.close();
    return ExifUtils::writeJpegExifFromJsonFile(path, json.fileName(), error);
}
class MetadataTest : public QObject {
    Q_OBJECT
private slots:
    void orientation_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<int>("expected");
        QTest::newRow("landscape") << 0 << 1;
        QTest::newRow("portrait") << 90 << 6;
        QTest::newRow("inverted-landscape") << 180 << 3;
        QTest::newRow("inverted-portrait") << 270 << 8;
    }
    void orientation()
    {
        QFETCH(int, rotation); QFETCH(int, expected);
        for (const QString &mode : {QString("simple"), QString("advanced"), QString("raw-render")}) {
            for (bool rotated : {false, true}) {
                QTemporaryDir dir;
                const QString path = dir.filePath("image.jpg");
                QJsonObject metadata{{"capture_mode", mode}, {"capture_orientation", rotation}, {"pixels_rotated", rotated}};
                QString error;
                QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
                auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
                ExifEntry *tag = exif_content_get_entry(exif->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
                QVERIFY(tag);
                QCOMPARE(int(exif_get_short(tag->data, exif_data_get_byte_order(exif.get()))), rotated ? 1 : expected);
                QCOMPARE(ExifUtils::orientation(metadata, true), expected);
                QCOMPARE(details(exif.get()).value("capture_mode").toString(), mode);
            }
        }
    }
    void longExposureAndTimestamp()
    {
        QTemporaryDir dir; QString error;
        QJsonObject metadata{{"exposure_time_ns", "31500000000"}, {"iso", 200},
                             {"captured_at", "2026-09-29T10:20:30.123+03:00"}};
        const QString path = dir.filePath("long.jpg");
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        auto tag = exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME); QVERIFY(tag);
        const ExifRational exposure = exif_get_rational(tag->data, exif_data_get_byte_order(exif.get()));
        QVERIFY(qAbs(double(exposure.numerator) / exposure.denominator - 31.5) < 0.000001);
        tag = exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL); QVERIFY(tag);
        QCOMPARE(QByteArray(reinterpret_cast<char *>(tag->data)), QByteArray("2026:09:29 10:20:30"));
    }
    void privacyAndMissingValues()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("gps.jpg");
        QJsonObject metadata{{"save_location", true}, {"gps", QJsonObject{{"latitude", 24.7}, {"longitude", 46.6}, {"altitude", -12}}},
                             {"sensor_sensitivity_requested", 800}, {"exposure_time_requested_ns", "30000000000"}};
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        QVERIFY(exif->ifd[EXIF_IFD_GPS]->count > 0);
        QVERIFY(!exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME));
        metadata["save_location"] = false;
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        exif = readExif(path); QVERIFY(exif.get() != nullptr);
        QCOMPARE(exif->ifd[EXIF_IFD_GPS]->count, 0u);
        QVERIFY(!details(exif.get()).contains("gps"));
        const QJsonObject merged = ExifUtils::mergeCaptureMetadata(metadata, QJsonObject{{"save_location", false}});
        QVERIFY(!merged.contains("gps"));
    }
    void preserveMetadataAfterPixelRewrite()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("preserve.jpg");
        QVERIFY(writePhoto(path, {{"lens_model", "Test lens"}, {"iso", 100}}, &error));
        const QByteArray original = ExifUtils::originalJpegExif(path);
        QImage pixels(path); QVERIFY(!pixels.isNull());
        QVERIFY(pixels.save(path, "JPEG"));
        QVERIFY(writePhoto(path, {{"_original_exif", QString::fromLatin1(original.toBase64())}, {"pixels_rotated", true}}, &error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        QVERIFY(exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], ExifTag(0xa434)));
        QVERIFY(exif_content_get_entry(exif->ifd[EXIF_IFD_EXIF], EXIF_TAG_ISO_SPEED_RATINGS));
    }
    void commentOverflowAndBrackets()
    {
        QTemporaryDir dir; QString error;
        const QString path = dir.filePath("bracket.jpg");
        QJsonArray calibration;
        for (int i = 0; i < 30000; ++i) calibration.append(i);
        QJsonObject metadata{{"capture_mode", "advanced"}, {"bracket_combined", true},
                             {"bracket_frames", QJsonArray{QJsonObject{{"iso", 100}}, QJsonObject{{"iso", 200}}}},
                             {"calibration", calibration}, {"label", QString::fromUtf8("صورة")}};
        QVERIFY2(writePhoto(path, metadata, &error), qPrintable(error));
        auto exif = readExif(path); QVERIFY(exif.get() != nullptr);
        const auto embedded = details(exif.get());
        QCOMPARE(embedded.value("label"), metadata.value("label"));
        QVERIFY(embedded.value("bracket_combined").toBool());
        QVERIFY(embedded.value("omitted_fields").toArray().contains("calibration"));
        QFile sidecar(path + ".json"); QVERIFY(sidecar.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(sidecar.readAll()).object().contains("calibration"));
    }
    void dngMetadata()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("raw.dng");
        TIFF *tiff = TIFFOpen(QFile::encodeName(path).constData(), "w"); QVERIFY(tiff);
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 1);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 1);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 1);
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        uint16_t pixel = 1234;
        QVERIFY(TIFFWriteScanline(tiff, &pixel, 0) >= 0);
        QJsonObject metadata{{"capture_orientation", 90}, {"pixels_rotated", true},
                             {"iso", 400}, {"exposure_time_ns", "30000000000"}, {"width", 1}, {"height", 1},
                             {"save_location", true}, {"gps", QJsonObject{{"latitude", -24.7}, {"longitude", 46.6}}}};
        QString error;
        const bool ok = ExifUtils::writeDngMetadata(tiff, metadata, &error);
        TIFFClose(tiff); QVERIFY2(ok, qPrintable(error));
        tiff = TIFFOpen(QFile::encodeName(path).constData(), "r"); QVERIFY(tiff);
        uint16_t orientation = 0;
        QVERIFY(TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation)); QCOMPARE(orientation, uint16_t(6));
        uint16_t readPixel = 0;
        QVERIFY(TIFFReadScanline(tiff, &readPixel, 0) >= 0); QCOMPARE(readPixel, pixel);
        uint64_t exifOffset = 0, gpsOffset = 0;
        QVERIFY(TIFFGetField(tiff, TIFFTAG_EXIFIFD, &exifOffset));
        QVERIFY(TIFFGetField(tiff, TIFFTAG_GPSIFD, &gpsOffset));
        QVERIFY(TIFFReadEXIFDirectory(tiff, exifOffset));
        float exposure = 0;
        QVERIFY(TIFFGetField(tiff, EXIFTAG_EXPOSURETIME, &exposure)); QCOMPARE(exposure, 30.0f);
        QVERIFY(TIFFReadGPSDirectory(tiff, gpsOffset));
        char *reference = nullptr;
        QVERIFY(TIFFGetField(tiff, GPSTAG_LATITUDEREF, &reference)); QCOMPARE(QByteArray(reference), QByteArray("S"));
        TIFFClose(tiff);
    }
};
QTEST_GUILESS_MAIN(MetadataTest)
#include "exifmetadata.moc"
