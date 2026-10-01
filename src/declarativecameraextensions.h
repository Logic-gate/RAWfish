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
#include <QFutureWatcher>
#include <QScopedPointer>
#include <QStringList>
#include <QVariantList>
#include <QJsonObject>
#include <QSize>

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
    Q_INVOKABLE QString camera2DeviceConfigDir() const;
    Q_INVOKABLE QString ensureCamera2GeneratedHalConfig(const QString &cameraId);
    Q_INVOKABLE QStringList camera2RawSizeModel(const QString &cameraId,
                                                const QString &rawFormat);
    Q_INVOKABLE QStringList camera2JpegSizeModel(const QString &cameraId);
    Q_INVOKABLE QSize camera2PreferredPreviewSize(const QString &cameraId);
    Q_INVOKABLE int camera2PreviewOrientation(const QString &cameraId, int fallback);
    Q_INVOKABLE QVariantMap camera2SimpleModeOverrides(const QString &cameraId);
    Q_INVOKABLE bool camera2PreviewMirror(const QString &cameraId, bool fallback);
    Q_INVOKABLE QStringList camera2CaptureFormatModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2LensModel(const QString &cameraId);
    Q_INVOKABLE QString camera2LensLabel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2FocusModeModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2SceneModel(const QString &cameraId);
    Q_INVOKABLE QVariantList camera2NoiseReductionModel(const QString &cameraId);
    Q_INVOKABLE QVariantList camera2IsoModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2BracketModel(const QString &cameraId);
    Q_INVOKABLE QString camera2MaxShutterNs(const QString &cameraId);
    Q_INVOKABLE QVariantMap camera2ExposureCapabilities(const QString &cameraId);
    Q_INVOKABLE void setCaptureCompensation(int steps) { m_captureCompensation = steps; }
    Q_INVOKABLE QString camera2BracketShutterNs(const QString &cameraId, const QString &base, int ev);
    Q_INVOKABLE QStringList camera2ShutterModel(const QString &cameraId);
    Q_INVOKABLE QStringList camera2FocusDistanceModel(const QString &cameraId);
    Q_INVOKABLE qreal camera2MaximumZoom(const QString &cameraId);
    Q_INVOKABLE QStringList rawCaptureExposureModel(const QString &cameraId);
    Q_INVOKABLE QVariantList rawCaptureRotationModel(const QString &cameraId,
                                                     const QString &mode);
    Q_INVOKABLE QVariantList rawCaptureFocusTimeoutModel(const QString &cameraId);
    Q_INVOKABLE QStringList rawCaptureRawFormatModel(const QString &cameraId);
    Q_INVOKABLE bool raw10CaptureAvailable(const QString &cameraId);
    Q_INVOKABLE QString camera2PreferredCaptureSize(const QString &cameraId,
                                                    const QString &format);
    Q_INVOKABLE QString camera2WarmCaptureSize(const QString &cameraId);
    Q_INVOKABLE QString preferredCamera2CameraId(const QString &cameraId);
    Q_INVOKABLE QString camera2CompatibilityLevel(const QString &cameraId);
    Q_INVOKABLE QString camera2CompatibilitySummary(const QString &cameraId);
    Q_INVOKABLE QString exportCamera2CompatibilityReport(const QString &cameraId);
    Q_INVOKABLE bool finalizeImageMetadata(const QString &path);
    Q_INVOKABLE void setCaptureMetadata(const QVariantMap &metadata);
    Q_INVOKABLE void setNextCaptureBracketMetadata(int index, int count,
                                                   qreal ev,
                                                   const QString &baseShutterNs);
    Q_INVOKABLE bool captureRawImage(const QString &targetPath, const QString &cameraId,
                                     const QString &rawSize, int timeoutSeconds,
                                     const QString &focusMode, const QString &focusDistance,
                                     int focusTimeoutSeconds, const QString &focusFailure,
                                     const QString &exposure, int jpegQuality,
                                     int rotationDegrees, const QString &rawSaveFormat,
                                     const QString &rawRenderEngine,
                                     const QString &rawFormat,
                                     const QString &sceneMode, int colorTemperature,
                                     int colorTint, bool progressiveJpeg,
                                     int sensorSensitivity,
                                     const QString &exposureTime,
                                     int aperture,
                                     int noiseReduction,
                                     qreal zoom,
                                     qreal focusX,
                                     qreal focusY);
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
                                     const QString &rawRenderEngine,
                                     int colorTemperature,
                                     int colorTint,
                                     bool progressiveJpeg);
    Q_INVOKABLE void discardRawBracket();
    Q_INVOKABLE bool combineRawBracket(const QString &targetPath,
                                          const QVariantList &sourcePaths,
                                          int jpegQuality);
signals:
    void rawBracketFrameReady(const QString &path);
    void rawImageCaptured(const QString &path, const QString &mimeType);
    void rawImageCaptureFailed(const QString &error);

private:
    int m_captureCompensation = 0;
    void finishRawImageCapture(int exitCode, QProcess::ExitStatus exitStatus);
    void finishRawImageRender();
    void finishBracketCombine();
    bool startRawImageProcess(const QString &program, const QStringList &arguments,
                              const QString &errorContext,
                              const QString &standardOutputPath = QString());
    bool renderRawImage();
    bool renderRawImageWithFastJpegConverter();
    bool saveJsonSidecar(const QString &targetPath, const QByteArray &json);
    bool copyJsonSidecar(const QString &sourcePath, const QString &targetPath);
    bool writeDngSidecar(const QString &metadataPath, const QString &targetPath);
    bool preserveRawCaptureFiles();
    bool stageRawBracketFrame();
    void clearRawImageCapture();
    bool loadCamera2Capabilities(const QString &cameraId);

    enum RawCaptureStage {
        RawCaptureIdle,
        RawCaptureCapturing,
        RawCaptureJpegCapturing
    };

    QJsonObject m_captureMetadata;
    QScopedPointer<QProcess> m_rawCaptureProcess;
    QScopedPointer<QFutureWatcher<bool> > m_rawRenderWatcher;
    QScopedPointer<QFutureWatcher<bool> > m_bracketCombineWatcher;
    QScopedPointer<QTemporaryDir> m_rawCaptureDirectory;
    QScopedPointer<QTemporaryDir> m_rawBracketDirectory;
    bool m_rawBracketCancelled = false;
    RawCaptureStage m_rawCaptureStage = RawCaptureIdle;
    QString m_rawCaptureTargetPath;
    QString m_rawCapturePrefix;
    QString m_rawCaptureArchivePrefix;
    QString m_rawCaptureErrors;
    QString m_rawCaptureExposure;
    QByteArray m_rawCaptureStandardOutput;
    QString m_rawCaptureSaveFormat = QStringLiteral("raw16");
    QString m_rawRenderEngine = QStringLiteral("internal");
    bool m_rawCaptureProgressiveJpeg = false;
    int m_rawCaptureJpegQuality = 92;
    int m_rawCaptureRotationDegrees = 0;
    int m_rawCaptureColorTemperature = 0;
    int m_rawCaptureColorTint = 0;
    int m_bracketIndex = -1;
    int m_bracketCount = 0;
    qreal m_bracketEv = 0.0;
    QString m_bracketBaseShutterNs;
    QString m_bracketCombineTargetPath;
    QStringList m_bracketCombineSourcePaths;
    int m_bracketCombineJpegQuality = 92;
    qint64 m_rawRenderStart = 0;
    QElapsedTimer m_rawCaptureTimer;
    bool m_camera2CapabilitiesLoaded = false;
    bool m_camera2CapabilitiesValid = false;
    QString m_camera2CapabilitiesCameraId;
    QStringList m_camera2Raw16Sizes;
    QStringList m_camera2Raw10Sizes;
    QStringList m_camera2JpegSizes;
    QStringList m_camera2PreviewSizes;
    QStringList m_camera2FocusModes;
    QStringList m_camera2Scenes;
    QVariantList m_camera2NoiseReductionModes;
    bool m_camera2ManualExposureSupported = false;
    bool m_camera2ManualBracketingSupported = false;
    qint64 m_camera2HalMaxShutterNs = 0;
    qint64 m_camera2MaxShutterNs = 0;
    QJsonObject m_camera2DeviceProfile;
    QString m_camera2DeviceProfileId;
    QString m_camera2DeviceProfileSource;
    QByteArray m_camera2ProbeJson;
    QByteArray m_camera2ProbeErrors;
    QJsonObject m_camera2SelectedCamera;
    qint64 m_camera2OverrideProfileMtime = 0;
};

#endif
