/*
 * SPDX-FileCopyrightText: 2013 - 2014 Jolla Ltd.
 * SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef DECLARATIVECAMERAEXTENSIONS_H
#define DECLARATIVECAMERAEXTENSIONS_H

#include <QProcess>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QScopedPointer>

class QTemporaryDir;

class DeclarativeCameraExtensions : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool rawImageCaptureAvailable READ rawImageCaptureAvailable CONSTANT)

public:
    DeclarativeCameraExtensions(QObject *parent = nullptr);
    ~DeclarativeCameraExtensions();

    bool rawImageCaptureAvailable() const;

    Q_INVOKABLE void disableNotifications(QQuickItem *item, bool disable);
    Q_INVOKABLE bool captureRawImage(const QString &targetPath, const QString &cameraId,
                                     const QString &rawSize, int timeoutSeconds,
                                     const QString &focusMode, const QString &focusDistance,
                                     int focusTimeoutSeconds, const QString &focusFailure,
                                     const QString &exposure, int jpegQuality,
                                     int rotationDegrees, const QString &rawSaveFormat,
                                     const QString &sceneMode, int colorTemperature,
                                     int colorTint, bool progressiveJpeg,
                                     int sensorSensitivity,
                                     const QString &exposureTime,
                                     int aperture,
                                     int noiseReduction,
                                     qreal zoom);
    Q_INVOKABLE bool captureJpegImage(const QString &targetPath, const QString &cameraId,
                                      const QString &jpegSize, int timeoutSeconds,
                                      int jpegQuality, int rotationDegrees,
                                      const QString &exposure,
                                      const QString &sceneMode,
                                      int sensorSensitivity,
                                      const QString &exposureTime,
                                      int aperture,
                                      int noiseReduction,
                                      qreal zoom);
    Q_INVOKABLE bool processRawImage(const QString &targetPath,
                                     const QString &rawPath,
                                     const QString &metadataPath,
                                     const QString &exposure,
                                     int jpegQuality,
                                     int rotationDegrees,
                                     const QString &rawSaveFormat,
                                     int colorTemperature,
                                     int colorTint,
                                     bool progressiveJpeg);

signals:
    void rawImageCaptured(const QString &path, const QString &mimeType);
    void rawImageCaptureFailed(const QString &error);

private:
    void finishRawImageCapture(int exitCode, QProcess::ExitStatus exitStatus);
    bool startRawImageProcess(const QString &program, const QStringList &arguments,
                              const QString &errorContext,
                              const QString &standardOutputPath = QString());
    bool renderRawImage();
    bool saveJsonSidecar(const QString &targetPath, const QByteArray &json);
    bool copyJsonSidecar(const QString &sourcePath, const QString &targetPath);
    bool writeDngSidecar(const QString &metadataPath, const QString &targetPath);
    void preserveRawCaptureFiles();
    void clearRawImageCapture();

    enum RawCaptureStage {
        RawCaptureIdle,
        RawCaptureCapturing,
        RawCaptureJpegCapturing
    };

    QScopedPointer<QProcess> m_rawCaptureProcess;
    QScopedPointer<QTemporaryDir> m_rawCaptureDirectory;
    RawCaptureStage m_rawCaptureStage = RawCaptureIdle;
    QString m_rawCaptureTargetPath;
    QString m_rawCapturePrefix;
    QString m_rawCaptureArchivePrefix;
    QString m_rawCaptureErrors;
    QString m_rawCaptureExposure;
    QByteArray m_rawCaptureStandardOutput;
    QString m_rawCaptureSaveFormat = QStringLiteral("raw16");
    bool m_rawCaptureProgressiveJpeg = false;
    int m_rawCaptureJpegQuality = 92;
    int m_rawCaptureRotationDegrees = 0;
    int m_rawCaptureColorTemperature = 0;
    int m_rawCaptureColorTint = 0;
    QElapsedTimer m_rawCaptureTimer;
};

#endif
