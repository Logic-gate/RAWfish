#ifndef CAMERA2PREVIEW_H
#define CAMERA2PREVIEW_H

#include <QImage>
#include <QMap>
#include <QProcess>
#include <QQuickItem>
#include <QSize>
#include <QTimer>
#include <QVariant>

class Camera2Preview : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool jpegCaptureReady READ jpegCaptureReady NOTIFY jpegCaptureReadyChanged)
    Q_PROPERTY(bool rawCaptureReady READ rawCaptureReady NOTIFY rawCaptureReadyChanged)
    Q_PROPERTY(QString cameraId READ cameraId WRITE setCameraId NOTIFY cameraIdChanged)
    Q_PROPERTY(QSize previewSize READ previewSize WRITE setPreviewSize NOTIFY previewSizeChanged)
    Q_PROPERTY(QString captureSize READ captureSize WRITE setCaptureSize NOTIFY captureSizeChanged)
    Q_PROPERTY(int captureTimeout READ captureTimeout WRITE setCaptureTimeout NOTIFY captureTimeoutChanged)
    Q_PROPERTY(bool jpegCaptureEnabled READ jpegCaptureEnabled WRITE setJpegCaptureEnabled NOTIFY jpegCaptureEnabledChanged)
    Q_PROPERTY(bool rawCaptureEnabled READ rawCaptureEnabled WRITE setRawCaptureEnabled NOTIFY rawCaptureEnabledChanged)
    Q_PROPERTY(int jpegQuality READ jpegQuality WRITE setJpegQuality NOTIFY jpegQualityChanged)
    Q_PROPERTY(int jpegOrientation READ jpegOrientation WRITE setJpegOrientation NOTIFY jpegOrientationChanged)
    Q_PROPERTY(QVariant source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
    Q_PROPERTY(bool mirror READ mirror WRITE setMirror NOTIFY mirrorChanged)
    Q_PROPERTY(bool fill READ fill WRITE setFill NOTIFY fillChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(QString focusMode READ focusMode WRITE setFocusMode NOTIFY focusModeChanged)
    Q_PROPERTY(qreal focusDistance READ focusDistance WRITE setFocusDistance NOTIFY focusDistanceChanged)
    Q_PROPERTY(int exposureCompensation READ exposureCompensation WRITE setExposureCompensation NOTIFY exposureCompensationChanged)
    Q_PROPERTY(int sensorSensitivity READ sensorSensitivity WRITE setSensorSensitivity NOTIFY sensorSensitivityChanged)
    Q_PROPERTY(QString exposureTime READ exposureTime WRITE setExposureTime NOTIFY exposureTimeChanged)
    Q_PROPERTY(int aperture READ aperture WRITE setAperture NOTIFY apertureChanged)
    Q_PROPERTY(int noiseReduction READ noiseReduction WRITE setNoiseReduction NOTIFY noiseReductionChanged)
    Q_PROPERTY(qreal renderExposure READ renderExposure WRITE setRenderExposure NOTIFY renderExposureChanged)
    Q_PROPERTY(QString sceneMode READ sceneMode WRITE setSceneMode NOTIFY sceneModeChanged)
    Q_PROPERTY(int colorTemperature READ colorTemperature WRITE setColorTemperature NOTIFY colorTemperatureChanged)
    Q_PROPERTY(int colorTint READ colorTint WRITE setColorTint NOTIFY colorTintChanged)
    Q_PROPERTY(qreal focalLength READ focalLength NOTIFY focalLengthChanged)
    Q_PROPERTY(int liveSensorSensitivity READ liveSensorSensitivity NOTIFY liveSensorSensitivityChanged)
    Q_PROPERTY(QString liveExposureTime READ liveExposureTime NOTIFY liveExposureTimeChanged)
    Q_PROPERTY(QVariantList histogram READ histogram NOTIFY histogramChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    explicit Camera2Preview(QQuickItem *parent = nullptr);
    ~Camera2Preview() override;

    bool active() const;
    void setActive(bool active);

    bool running() const;
    bool jpegCaptureReady() const;
    bool rawCaptureReady() const;

    QString cameraId() const;
    void setCameraId(const QString &cameraId);

    QSize previewSize() const;
    void setPreviewSize(const QSize &previewSize);

    QString captureSize() const;
    void setCaptureSize(const QString &captureSize);

    int captureTimeout() const;
    void setCaptureTimeout(int captureTimeout);

    bool jpegCaptureEnabled() const;
    void setJpegCaptureEnabled(bool jpegCaptureEnabled);
    bool rawCaptureEnabled() const;
    void setRawCaptureEnabled(bool rawCaptureEnabled);

    int jpegQuality() const;
    void setJpegQuality(int jpegQuality);

    int jpegOrientation() const;
    void setJpegOrientation(int jpegOrientation);

    QVariant source() const;
    void setSource(const QVariant &source);

    int orientation() const;
    void setOrientation(int orientation);

    bool mirror() const;
    void setMirror(bool mirror);

    bool fill() const;
    void setFill(bool fill);

    qreal zoom() const;
    void setZoom(qreal zoom);

    QString focusMode() const;
    void setFocusMode(const QString &focusMode);

    qreal focusDistance() const;
    void setFocusDistance(qreal focusDistance);

    int exposureCompensation() const;
    void setExposureCompensation(int exposureCompensation);

    int sensorSensitivity() const;
    void setSensorSensitivity(int sensorSensitivity);

    QString exposureTime() const;
    void setExposureTime(const QString &exposureTime);

    int aperture() const;
    void setAperture(int aperture);

    int noiseReduction() const;
    void setNoiseReduction(int noiseReduction);

    qreal renderExposure() const;
    void setRenderExposure(qreal renderExposure);

    QString sceneMode() const;
    void setSceneMode(const QString &sceneMode);

    int colorTemperature() const;
    void setColorTemperature(int colorTemperature);

    int colorTint() const;
    void setColorTint(int colorTint);

    qreal focalLength() const;
    int liveSensorSensitivity() const;
    QString liveExposureTime() const;
    QVariantList histogram() const;

    QString errorString() const;

    /**
     * Restarts the Camera2 preview helper process.
     */
    Q_INVOKABLE void restartPreview();

    /**
     * Requests Camera2 focus and metering at normalized viewfinder coordinates.
     */
    Q_INVOKABLE void setFocusPoint(qreal x, qreal y);

    /**
     * Clears the Camera2 focus and metering point.
     */
    Q_INVOKABLE void clearFocusPoint();

    /**
     * Captures a JPEG from the running Camera2 preview session.
     */
    Q_INVOKABLE bool captureJpeg(const QString &path);
    Q_INVOKABLE bool captureRaw(const QString &rawPath,
                                const QString &metadataPath);

signals:
    void activeChanged();
    void runningChanged();
    void jpegCaptureReadyChanged();
    void rawCaptureReadyChanged();
    void cameraIdChanged();
    void previewSizeChanged();
    void captureSizeChanged();
    void captureTimeoutChanged();
    void jpegCaptureEnabledChanged();
    void rawCaptureEnabledChanged();
    void jpegQualityChanged();
    void jpegOrientationChanged();
    void sourceChanged();
    void orientationChanged();
    void mirrorChanged();
    void fillChanged();
    void zoomChanged();
    void focusModeChanged();
    void focusDistanceChanged();
    void exposureCompensationChanged();
    void sensorSensitivityChanged();
    void exposureTimeChanged();
    void apertureChanged();
    void noiseReductionChanged();
    void renderExposureChanged();
    void sceneModeChanged();
    void colorTemperatureChanged();
    void colorTintChanged();
    void focalLengthChanged();
    void liveSensorSensitivityChanged();
    void liveExposureTimeChanged();
    void histogramChanged();
    void errorStringChanged();
    void imageCaptured(const QString &path, const QString &mimeType);
    void rawImageReady(const QString &rawPath, const QString &metadataPath);
    void imageCaptureFailed(const QString &error);

private slots:
    void readFrames();
    void readErrors();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void checkCaptureResult();
    void finishPreviewCapture(const QString &path, bool success,
                              const QString &error);

private:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override;

    void restart();
    void stop();
    void savePreviewJpegAsync(const QString &path);
    void savePreviewMetadata(const QString &path, const QSize &imageSize);
    void parseFrames();
    void parseMetadata(const QByteArray &payload);
    void parseCaptureResult(const QMap<QString, QString> &fields);
    void updateHistogram(const QImage &frame);
    void sendSettings();
    void sendSettings(bool captureExposure);
    void setErrorString(const QString &errorString);
    QString helperPath() const;

    bool m_active = false;
    bool m_running = false;
    QString m_cameraId = QStringLiteral("0");
    QSize m_previewSize = QSize(640, 480);
    QString m_captureSize = QStringLiteral("4096x3072");
    int m_captureTimeout = 30;
    bool m_jpegCaptureEnabled = false;
    bool m_rawCaptureEnabled = false;
    int m_jpegQuality = 92;
    int m_jpegOrientation = 90;
    QVariant m_source;
    int m_orientation = 90;
    bool m_mirror = false;
    bool m_fill = false;
    qreal m_zoom = 1.0;
    qreal m_focusX = -1.0;
    qreal m_focusY = -1.0;
    QString m_focusMode = QStringLiteral("continuous");
    qreal m_focusDistance = 0.0;
    int m_exposureCompensation = 0;
    int m_sensorSensitivity = 0;
    QString m_exposureTime = QStringLiteral("0");
    int m_aperture = 0;
    int m_noiseReduction = 0;
    qreal m_renderExposure = 1.0;
    QString m_sceneMode = QStringLiteral("manual");
    int m_colorTemperature = 0;
    int m_colorTint = 0;
    qreal m_focalLength = 0.0;
    int m_liveSensorSensitivity = 0;
    QString m_liveExposureTime = QStringLiteral("0");
    QVariantList m_histogram;
    QString m_errorString;
    QImage m_frame;
    QByteArray m_buffer;
    bool m_previewCaptureRunning = false;
    QString m_pendingCapturePath;
    QString m_pendingCaptureFinalPath;
    qint64 m_pendingCaptureSize = -1;
    int m_pendingCaptureStablePolls = 0;
    int m_capturePollsRemaining = 0;
    bool m_pendingRawCapture = false;
    bool m_restorePreviewSettingsAfterCapture = false;
    QTimer m_captureTimer;
    QProcess *m_process = nullptr;
};

#endif
