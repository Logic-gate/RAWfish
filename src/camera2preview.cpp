#include "camera2preview.h"
#include "imageadjustments.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QStringList>
#include <QTransform>
#include <QVariantMap>
#include <QtGlobal>

#include <climits>
#include <cmath>
#include <thread>

namespace {

const int HistogramBins = 32;

quint32 readLe32(const char *data)
{
    const uchar *bytes = reinterpret_cast<const uchar *>(data);
    return quint32(bytes[0])
            | (quint32(bytes[1]) << 8)
            | (quint32(bytes[2]) << 16)
            | (quint32(bytes[3]) << 24);
}

QString jsonSidecarPath(const QString &targetPath)
{
    const QFileInfo targetInfo(targetPath);
    return targetInfo.absolutePath() + QLatin1Char('/')
            + targetInfo.completeBaseName() + QLatin1String(".json");
}

QSize sizeFromString(const QString &value)
{
    const QStringList parts = value.split(QLatin1Char('x'));
    if (parts.count() != 2) {
        return QSize();
    }
    bool widthOk = false;
    bool heightOk = false;
    const int width = parts.at(0).toInt(&widthOk);
    const int height = parts.at(1).toInt(&heightOk);
    return widthOk && heightOk ? QSize(width, height) : QSize();
}

qreal exposureNsFromText(const QString &value)
{
    bool ok = false;
    const qlonglong exposure = value.toLongLong(&ok);
    return ok && exposure > 0 ? qreal(exposure) : 0.0;
}

}

Camera2Preview::Camera2Preview(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_captureTimer.setInterval(50);
    connect(&m_captureTimer, &QTimer::timeout,
            this, &Camera2Preview::checkCaptureResult);
}

Camera2Preview::~Camera2Preview()
{
    stop();
}

bool Camera2Preview::active() const
{
    return m_active;
}

void Camera2Preview::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    emit activeChanged();
    restart();
}

bool Camera2Preview::running() const
{
    return m_running;
}

bool Camera2Preview::jpegCaptureReady() const
{
    return m_running && m_jpegCaptureEnabled && m_process &&
            m_process->state() == QProcess::Running;
}

bool Camera2Preview::rawCaptureReady() const
{
    return m_running && m_rawCaptureEnabled && m_process &&
            m_process->state() == QProcess::Running;
}

QString Camera2Preview::cameraId() const
{
    return m_cameraId;
}

void Camera2Preview::setCameraId(const QString &cameraId)
{
    const QString effectiveId = cameraId.isEmpty() ? QStringLiteral("0") : cameraId;
    if (m_cameraId == effectiveId) {
        return;
    }
    m_cameraId = effectiveId;
    emit cameraIdChanged();
    restart();
}

QSize Camera2Preview::previewSize() const
{
    return m_previewSize;
}

void Camera2Preview::setPreviewSize(const QSize &previewSize)
{
    if (!previewSize.isValid() || m_previewSize == previewSize) {
        return;
    }
    m_previewSize = previewSize;
    emit previewSizeChanged();
    restart();
}

QString Camera2Preview::captureSize() const
{
    return m_captureSize;
}

void Camera2Preview::setCaptureSize(const QString &captureSize)
{
    const QString effectiveSize = captureSize.isEmpty()
            ? QStringLiteral("4096x3072") : captureSize;
    if (m_captureSize == effectiveSize) {
        return;
    }
    m_captureSize = effectiveSize;
    emit captureSizeChanged();
    restart();
}

int Camera2Preview::captureTimeout() const
{
    return m_captureTimeout;
}

void Camera2Preview::setCaptureTimeout(int captureTimeout)
{
    const int timeout = qBound(1, captureTimeout, 3600);
    if (m_captureTimeout == timeout) {
        return;
    }
    m_captureTimeout = timeout;
    emit captureTimeoutChanged();
}

bool Camera2Preview::jpegCaptureEnabled() const
{
    return m_jpegCaptureEnabled;
}

void Camera2Preview::setJpegCaptureEnabled(bool jpegCaptureEnabled)
{
    if (m_jpegCaptureEnabled == jpegCaptureEnabled) {
        return;
    }
    m_jpegCaptureEnabled = jpegCaptureEnabled;
    emit jpegCaptureEnabledChanged();
    restart();
}

bool Camera2Preview::rawCaptureEnabled() const
{
    return m_rawCaptureEnabled;
}

void Camera2Preview::setRawCaptureEnabled(bool rawCaptureEnabled)
{
    if (m_rawCaptureEnabled == rawCaptureEnabled) {
        return;
    }
    m_rawCaptureEnabled = rawCaptureEnabled;
    emit rawCaptureEnabledChanged();
    emit rawCaptureReadyChanged();
    restart();
}

int Camera2Preview::jpegQuality() const
{
    return m_jpegQuality;
}

void Camera2Preview::setJpegQuality(int jpegQuality)
{
    const int quality = qBound(1, jpegQuality, 100);
    if (m_jpegQuality == quality) {
        return;
    }
    m_jpegQuality = quality;
    emit jpegQualityChanged();
    restart();
}

int Camera2Preview::jpegOrientation() const
{
    return m_jpegOrientation;
}

void Camera2Preview::setJpegOrientation(int jpegOrientation)
{
    const int orientation = ((jpegOrientation % 360) + 360) % 360;
    if (m_jpegOrientation == orientation) {
        return;
    }
    m_jpegOrientation = orientation;
    emit jpegOrientationChanged();
    restart();
}

QVariant Camera2Preview::source() const
{
    return m_source;
}

void Camera2Preview::setSource(const QVariant &source)
{
    if (m_source == source) {
        return;
    }
    m_source = source;
    emit sourceChanged();
}

int Camera2Preview::orientation() const
{
    return m_orientation;
}

void Camera2Preview::setOrientation(int orientation)
{
    const int normalized = ((orientation % 360) + 360) % 360;
    if (m_orientation == normalized) {
        return;
    }
    m_orientation = normalized;
    emit orientationChanged();
    update();
}

bool Camera2Preview::mirror() const
{
    return m_mirror;
}

void Camera2Preview::setMirror(bool mirror)
{
    if (m_mirror == mirror) {
        return;
    }
    m_mirror = mirror;
    emit mirrorChanged();
    update();
}

bool Camera2Preview::fill() const
{
    return m_fill;
}

void Camera2Preview::setFill(bool fill)
{
    if (m_fill == fill) {
        return;
    }
    m_fill = fill;
    emit fillChanged();
    update();
}

qreal Camera2Preview::zoom() const
{
    return m_zoom;
}

void Camera2Preview::setZoom(qreal zoom)
{
    const qreal clamped = qMax<qreal>(1.0, zoom);
    if (qFuzzyCompare(m_zoom, clamped)) {
        return;
    }
    m_zoom = clamped;
    emit zoomChanged();
    update();
    sendSettings();
}

QString Camera2Preview::focusMode() const
{
    return m_focusMode;
}

void Camera2Preview::setFocusMode(const QString &focusMode)
{
    const QString mode = focusMode.isEmpty() ? QStringLiteral("continuous")
                                             : focusMode;
    if (m_focusMode == mode) {
        return;
    }
    m_focusMode = mode;
    emit focusModeChanged();
    sendSettings();
}

qreal Camera2Preview::focusDistance() const
{
    return m_focusDistance;
}

void Camera2Preview::setFocusDistance(qreal focusDistance)
{
    const qreal distance = qMax<qreal>(0.0, focusDistance);
    if (qFuzzyCompare(m_focusDistance, distance)) {
        return;
    }
    m_focusDistance = distance;
    emit focusDistanceChanged();
    sendSettings();
}

int Camera2Preview::exposureCompensation() const
{
    return m_exposureCompensation;
}

void Camera2Preview::setExposureCompensation(int exposureCompensation)
{
    const int compensation = qBound(-24, exposureCompensation, 24);
    if (m_exposureCompensation == compensation) {
        return;
    }
    m_exposureCompensation = compensation;
    emit exposureCompensationChanged();
    sendSettings();
}

int Camera2Preview::sensorSensitivity() const
{
    return m_sensorSensitivity;
}

void Camera2Preview::setSensorSensitivity(int sensorSensitivity)
{
    const int sensitivity = qBound(0, sensorSensitivity, 102400);
    if (m_sensorSensitivity == sensitivity) {
        return;
    }
    m_sensorSensitivity = sensitivity;
    emit sensorSensitivityChanged();
    sendSettings();
}

QString Camera2Preview::exposureTime() const
{
    return m_exposureTime;
}

void Camera2Preview::setExposureTime(const QString &exposureTime)
{
    const QString time = exposureTime.isEmpty() ? QStringLiteral("0")
                                                : exposureTime;
    if (m_exposureTime == time) {
        return;
    }
    m_exposureTime = time;
    emit exposureTimeChanged();
    sendSettings();
}

int Camera2Preview::aperture() const
{
    return m_aperture;
}

void Camera2Preview::setAperture(int aperture)
{
    const int value = qBound(0, aperture, 255);
    if (m_aperture == value) {
        return;
    }
    m_aperture = value;
    emit apertureChanged();
    sendSettings();
}

int Camera2Preview::noiseReduction() const
{
    return m_noiseReduction;
}

void Camera2Preview::setNoiseReduction(int noiseReduction)
{
    const int reduction = qMax(0, noiseReduction);
    if (m_noiseReduction == reduction) {
        return;
    }
    m_noiseReduction = reduction;
    emit noiseReductionChanged();
    sendSettings();
}

qreal Camera2Preview::renderExposure() const
{
    return m_renderExposure;
}

void Camera2Preview::setRenderExposure(qreal renderExposure)
{
    const qreal exposure = qBound<qreal>(0.25, renderExposure, 4.0);
    if (qFuzzyCompare(m_renderExposure, exposure)) {
        return;
    }
    m_renderExposure = exposure;
    emit renderExposureChanged();
    update();
}

QString Camera2Preview::sceneMode() const
{
    return m_sceneMode;
}

void Camera2Preview::setSceneMode(const QString &sceneMode)
{
    const QString mode = sceneMode.isEmpty() ? QStringLiteral("manual")
                                             : sceneMode;
    if (m_sceneMode == mode) {
        return;
    }
    m_sceneMode = mode;
    emit sceneModeChanged();
    sendSettings();
}

int Camera2Preview::colorTemperature() const
{
    return m_colorTemperature;
}

void Camera2Preview::setColorTemperature(int colorTemperature)
{
    const int temperature = qBound(0, colorTemperature, 50000);
    if (m_colorTemperature == temperature) {
        return;
    }
    m_colorTemperature = temperature;
    emit colorTemperatureChanged();
    update();
    sendSettings();
}

int Camera2Preview::colorTint() const
{
    return m_colorTint;
}

void Camera2Preview::setColorTint(int colorTint)
{
    const int tint = qBound(-1000, colorTint, 1000);
    if (m_colorTint == tint) {
        return;
    }
    m_colorTint = tint;
    emit colorTintChanged();
    update();
    sendSettings();
}

qreal Camera2Preview::focalLength() const
{
    return m_focalLength;
}

int Camera2Preview::liveSensorSensitivity() const
{
    return m_liveSensorSensitivity;
}

QString Camera2Preview::liveExposureTime() const
{
    return m_liveExposureTime;
}

QVariantList Camera2Preview::histogram() const
{
    return m_histogram;
}

QString Camera2Preview::errorString() const
{
    return m_errorString;
}

/**
 * @brief Uploads the latest bridge RGB frame into the Qt scene graph.
 *
 * The bridge already converts YUV420 to RGB888. This stage keeps texture
 * filtering linear so preview scaling does not add extra nearest-neighbor
 * aliasing on top of the bridge conversion.
 */
QSGNode *Camera2Preview::updatePaintNode(QSGNode *oldNode,
                                         UpdatePaintNodeData *)
{
    delete oldNode;
    if (m_frame.isNull() || width() <= 0 || height() <= 0 || !window()) {
        return nullptr;
    }

    QImage textureImage = m_frame;
    if (ImageAdjustments::requested(m_renderExposure, m_colorTemperature,
                                    m_colorTint)) {
        textureImage = m_frame.copy();
        ImageAdjustments::apply(&textureImage, m_renderExposure,
                                m_colorTemperature, m_colorTint);
    }

    QSGTexture *texture = window()->createTextureFromImage(textureImage);
    if (!texture) {
        return nullptr;
    }
    texture->setFiltering(QSGTexture::Linear);

    QSGSimpleTextureNode *node = new QSGSimpleTextureNode;
    node->setOwnsTexture(true);
    node->setTexture(texture);

    const QRectF target(0, 0, width(), height());
    const QSizeF imageSize = m_frame.size();
    const qreal zoom = qMax<qreal>(1.0, m_zoom);
    const QSizeF sourceSize(imageSize.width() / zoom,
                            imageSize.height() / zoom);
    const QRectF sourceRect((imageSize.width() - sourceSize.width()) / 2,
                            (imageSize.height() - sourceSize.height()) / 2,
                            sourceSize.width(), sourceSize.height());
    const qreal scale = m_fill
            ? qMax(target.width() / sourceSize.width(),
                   target.height() / sourceSize.height())
            : qMin(target.width() / sourceSize.width(),
                   target.height() / sourceSize.height());
    const QSizeF drawSize(sourceSize.width() * scale,
                          sourceSize.height() * scale);
    const QRectF drawRect(target.center().x() - drawSize.width() / 2,
                          target.center().y() - drawSize.height() / 2,
                          drawSize.width(), drawSize.height());

    node->setRect(drawRect);
    node->setSourceRect(sourceRect);
    return node;
}

void Camera2Preview::restartPreview()
{
    restart();
}

void Camera2Preview::setFocusPoint(qreal x, qreal y)
{
    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
        return;
    }
    m_focusX = x;
    m_focusY = y;
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write(QStringLiteral("focus %1 %2\n")
                         .arg(m_focusX, 0, 'f', 4)
                         .arg(m_focusY, 0, 'f', 4)
                         .toLocal8Bit());
    }
}

void Camera2Preview::clearFocusPoint()
{
    m_focusX = -1.0;
    m_focusY = -1.0;
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write("focus-reset\n");
    }
}

bool Camera2Preview::captureJpeg(const QString &path)
{
    if (path.isEmpty()) {
        setErrorString(QStringLiteral("Camera2 JPEG capture path is empty"));
        return false;
    }
    if (!m_process || m_process->state() != QProcess::Running) {
        setErrorString(QStringLiteral("Camera2 preview process is not running"));
        return false;
    }
    if (m_previewCaptureRunning) {
        setErrorString(QStringLiteral("Camera2 JPEG capture is already running"));
        return false;
    }

    if (!m_jpegCaptureEnabled && m_frame.isNull()) {
        setErrorString(QStringLiteral("Camera2 preview frame is not ready"));
        return false;
    }

    const QFileInfo fileInfo(path);
    QDir().mkpath(fileInfo.absolutePath());

    m_previewCaptureRunning = true;
    if (m_jpegCaptureEnabled) {
        QFile::remove(path);
        m_pendingCapturePath = path;
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_capturePollsRemaining = qMax(1, (m_captureTimeout * 1000)
                                          / m_captureTimer.interval());
        const QByteArray command = QByteArrayLiteral("capture-jpeg ")
                + QFile::encodeName(path) + QByteArrayLiteral("\n");
        sendSettings(true);
        m_restorePreviewSettingsAfterCapture = true;
        if (m_process->write(command) >= 0) {
            m_captureTimer.start();
            return true;
        }
        m_restorePreviewSettingsAfterCapture = false;
        sendSettings(false);
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_previewCaptureRunning = false;
        setErrorString(QStringLiteral("Could not write Camera2 JPEG capture command"));
        return false;
    }

    savePreviewJpegAsync(path);
    return true;
}

bool Camera2Preview::captureRaw(const QString &rawPath,
                                const QString &metadataPath)
{
    if (rawPath.isEmpty() || metadataPath.isEmpty()) {
        setErrorString(QStringLiteral("Camera2 RAW capture path is empty"));
        return false;
    }
    if (!rawCaptureReady()) {
        setErrorString(QStringLiteral("Camera2 RAW preview is not running"));
        return false;
    }
    if (m_previewCaptureRunning) {
        setErrorString(QStringLiteral("Camera2 RAW capture is already running"));
        return false;
    }

    QDir().mkpath(QFileInfo(rawPath).absolutePath());
    QDir().mkpath(QFileInfo(metadataPath).absolutePath());
    QFile::remove(rawPath);
    QFile::remove(metadataPath);

    m_previewCaptureRunning = true;
    m_pendingCapturePath = metadataPath;
    m_pendingCaptureFinalPath = rawPath;
    m_pendingRawCapture = true;
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    m_capturePollsRemaining = qMax(1, (m_captureTimeout * 1000)
                                      / m_captureTimer.interval());
    const QByteArray command = QByteArrayLiteral("capture-raw ")
            + QFile::encodeName(rawPath) + QByteArrayLiteral(" ")
            + QFile::encodeName(metadataPath) + QByteArrayLiteral("\n");
    sendSettings(true);
    m_restorePreviewSettingsAfterCapture = true;
    if (m_process->write(command) >= 0) {
        m_captureTimer.start();
        return true;
    }
    m_restorePreviewSettingsAfterCapture = false;
    sendSettings(false);

    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_previewCaptureRunning = false;
    setErrorString(QStringLiteral("Could not write Camera2 RAW capture command"));
    return false;
}

void Camera2Preview::savePreviewJpegAsync(const QString &path)
{
    QImage image = m_frame.copy();
    image.detach();
    const qreal exposure = m_renderExposure;
    const int colorTemperature = m_colorTemperature;
    const int colorTint = m_colorTint;
    const int quality = m_jpegQuality;
    QPointer<Camera2Preview> self(this);

    std::thread([self, path, image, exposure, colorTemperature, colorTint,
                 quality]() mutable {
        QString error;
        if (ImageAdjustments::requested(exposure, colorTemperature, colorTint)) {
            ImageAdjustments::apply(&image, exposure, colorTemperature,
                                    colorTint);
        }
        QImageWriter writer(path, "JPG");
        writer.setQuality(quality);
        const bool success = writer.write(image);
        if (!success) {
            error = QStringLiteral("Could not save Camera2 preview JPEG");
        }
        if (self) {
            QMetaObject::invokeMethod(
                self, "finishPreviewCapture", Qt::QueuedConnection,
                Q_ARG(QString, path), Q_ARG(bool, success),
                Q_ARG(QString, error));
        }
    }).detach();
}

void Camera2Preview::finishPreviewCapture(const QString &path, bool success,
                                          const QString &error)
{
    m_previewCaptureRunning = false;
    if (m_restorePreviewSettingsAfterCapture) {
        m_restorePreviewSettingsAfterCapture = false;
        sendSettings(false);
    }
    if (success) {
        savePreviewMetadata(path, m_frame.size());
        emit imageCaptured(path, QStringLiteral("image/jpeg"));
    } else {
        emit imageCaptureFailed(error);
    }
}

void Camera2Preview::savePreviewMetadata(const QString &path,
                                         const QSize &imageSize)
{
    QJsonObject metadata;
    metadata.insert(QStringLiteral("status"), QStringLiteral("ok"));
    metadata.insert(QStringLiteral("capture_source"),
                    QStringLiteral("camera2_preview"));
    metadata.insert(QStringLiteral("camera_id"), m_cameraId);
    metadata.insert(QStringLiteral("jpeg_path"), path);
    metadata.insert(QStringLiteral("width"), imageSize.width());
    metadata.insert(QStringLiteral("height"), imageSize.height());
    metadata.insert(QStringLiteral("jpeg_quality"), m_jpegQuality);
    metadata.insert(QStringLiteral("jpeg_orientation"), m_jpegOrientation);
    metadata.insert(QStringLiteral("zoom_ratio"), m_zoom);
    metadata.insert(QStringLiteral("focus_mode_requested"), m_focusMode);
    metadata.insert(QStringLiteral("focus_distance_requested_diopters"),
                    m_focusDistance);
    metadata.insert(QStringLiteral("exposure_compensation_requested"),
                    m_exposureCompensation);
    metadata.insert(QStringLiteral("sensor_sensitivity_requested"),
                    m_sensorSensitivity);
    metadata.insert(QStringLiteral("exposure_time_requested_ns"),
                    m_exposureTime);
    metadata.insert(QStringLiteral("aperture_requested"), m_aperture);
    metadata.insert(QStringLiteral("noise_reduction_requested"),
                    m_noiseReduction);
    metadata.insert(QStringLiteral("scene_mode_requested"), m_sceneMode);
    metadata.insert(QStringLiteral("render_exposure"), m_renderExposure);
    metadata.insert(QStringLiteral("color_temperature_requested_kelvin"),
                    m_colorTemperature);
    metadata.insert(QStringLiteral("color_tint_requested"), m_colorTint);
    metadata.insert(QStringLiteral("live_sensor_sensitivity"),
                    m_liveSensorSensitivity);
    metadata.insert(QStringLiteral("live_exposure_time_ns"),
                    m_liveExposureTime);
    metadata.insert(QStringLiteral("focal_length_mm"), m_focalLength);

    const QString sidecarPath = jsonSidecarPath(path);
    QDir().mkpath(QFileInfo(sidecarPath).absolutePath());
    QFile file(sidecarPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    }
}

void Camera2Preview::readFrames()
{
    if (!m_process) {
        return;
    }
    m_buffer.append(m_process->readAllStandardOutput());
    parseFrames();
}

void Camera2Preview::readErrors()
{
    if (!m_process) {
        return;
    }
    const QString errors = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
    if (!errors.isEmpty()) {
        const QStringList lines = errors.split(QLatin1Char('\n'),
                                               QString::SkipEmptyParts);
        QStringList realErrors;
        for (const QString &line : lines) {
            if (line.startsWith(QStringLiteral("capture-timing "))) {
                qDebug().noquote() << line;
            } else {
                realErrors.append(line);
            }
        }
        if (!realErrors.isEmpty()) {
            setErrorString(realErrors.join(QLatin1Char('\n')));
        }
    }
}

void Camera2Preview::processFinished(int, QProcess::ExitStatus)
{
    if (!m_process) {
        return;
    }
    m_process->deleteLater();
    m_process = nullptr;
    if (m_running) {
        m_running = false;
        emit runningChanged();
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
    }
    if (m_captureTimer.isActive()) {
        m_captureTimer.stop();
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        m_restorePreviewSettingsAfterCapture = false;
        emit imageCaptureFailed(QStringLiteral("Camera2 preview stopped during capture"));
    }
    if (m_active) {
        restart();
    }
}

void Camera2Preview::checkCaptureResult()
{
    if (m_pendingCapturePath.isEmpty()) {
        m_captureTimer.stop();
        return;
    }

    if (m_jpegCaptureEnabled || m_pendingRawCapture) {
        --m_capturePollsRemaining;
        if (m_capturePollsRemaining <= 0) {
            m_captureTimer.stop();
            m_pendingCapturePath.clear();
            m_pendingCaptureFinalPath.clear();
            m_pendingRawCapture = false;
            m_pendingCaptureSize = -1;
            m_pendingCaptureStablePolls = 0;
            m_previewCaptureRunning = false;
            if (m_restorePreviewSettingsAfterCapture) {
                m_restorePreviewSettingsAfterCapture = false;
                sendSettings(false);
            }
            emit imageCaptureFailed(QStringLiteral("Camera2 capture timed out"));
        }
        return;
    }

    const QFileInfo fileInfo(m_pendingCapturePath);
    if (fileInfo.exists() && fileInfo.size() > 0) {
        if (fileInfo.size() != m_pendingCaptureSize) {
            m_pendingCaptureSize = fileInfo.size();
            m_pendingCaptureStablePolls = 0;
            return;
        }
        if (++m_pendingCaptureStablePolls < 1) {
            return;
        }
        QString path = m_pendingCapturePath;
        if (!m_pendingCaptureFinalPath.isEmpty()) {
            QFile::remove(m_pendingCaptureFinalPath);
            if (!QFile::rename(m_pendingCapturePath,
                               m_pendingCaptureFinalPath)) {
                m_pendingCapturePath.clear();
                m_pendingCaptureFinalPath.clear();
                m_pendingRawCapture = false;
                m_pendingCaptureSize = -1;
                m_pendingCaptureStablePolls = 0;
                m_previewCaptureRunning = false;
                m_captureTimer.stop();
                emit imageCaptureFailed(QStringLiteral(
                    "Could not move Camera2 JPEG into place"));
                return;
            }
            path = m_pendingCaptureFinalPath;
        }
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingRawCapture = false;
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        m_captureTimer.stop();
        if (ImageAdjustments::requested(m_renderExposure, m_colorTemperature,
                                        m_colorTint)) {
            QImage image(path);
            if (image.isNull()) {
                emit imageCaptureFailed(QStringLiteral(
                    "Could not load Camera2 JPEG for adjustment"));
                return;
            }
            ImageAdjustments::apply(&image, m_renderExposure,
                                    m_colorTemperature, m_colorTint);
            QImageWriter writer(path, "JPG");
            writer.setQuality(m_jpegQuality);
            if (!writer.write(image)) {
                emit imageCaptureFailed(QStringLiteral(
                    "Could not save adjusted Camera2 JPEG"));
                return;
            }
        }
        savePreviewMetadata(path, sizeFromString(m_captureSize));
        emit imageCaptured(path, QStringLiteral("image/jpeg"));
        return;
    }

    --m_capturePollsRemaining;
    if (m_capturePollsRemaining <= 0) {
        m_captureTimer.stop();
        m_pendingCapturePath.clear();
        m_pendingCaptureFinalPath.clear();
        m_pendingCaptureSize = -1;
        m_pendingCaptureStablePolls = 0;
        m_previewCaptureRunning = false;
        if (m_restorePreviewSettingsAfterCapture) {
            m_restorePreviewSettingsAfterCapture = false;
            sendSettings(false);
        }
        emit imageCaptureFailed(QStringLiteral("Camera2 JPEG capture timed out"));
    }
}

void Camera2Preview::restart()
{
    stop();
    if (!m_active) {
        return;
    }

    const QString program = helperPath();
    if (!QFileInfo(program).isExecutable()) {
        setErrorString(QStringLiteral("Camera2 preview helper is not installed"));
        return;
    }

    m_buffer.clear();
    m_process = new QProcess(this);
    m_process->setProgram(program);
    QStringList arguments;
    arguments << QStringLiteral("--preview")
              << QStringLiteral("--camera") << m_cameraId
              << QStringLiteral("--size")
              << QStringLiteral("%1x%2").arg(m_previewSize.width()).arg(m_previewSize.height())
              << QStringLiteral("--frames") << QStringLiteral("10000")
              << QStringLiteral("--timeout") << QStringLiteral("3600")
              << QStringLiteral("--zoom") << QString::number(m_zoom, 'f', 4);
    if (m_jpegCaptureEnabled) {
        arguments << QStringLiteral("--jpeg-size") << m_captureSize
                  << QStringLiteral("--quality") << QString::number(m_jpegQuality)
                  << QStringLiteral("--orientation") << QString::number(m_jpegOrientation);
    }
    if (m_rawCaptureEnabled) {
        arguments << QStringLiteral("--raw-size") << m_captureSize;
    }
    if (m_focusX >= 0.0 && m_focusY >= 0.0) {
        arguments << QStringLiteral("--focus-x") << QString::number(m_focusX, 'f', 4)
                  << QStringLiteral("--focus-y") << QString::number(m_focusY, 'f', 4);
    }
    m_process->setArguments(arguments);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &Camera2Preview::readFrames);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &Camera2Preview::readErrors);
    connect(m_process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &Camera2Preview::processFinished);
    m_process->start();
    if (m_process->waitForStarted(1000)) {
        m_running = true;
        emit runningChanged();
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
        setErrorString(QString());
        sendSettings();
    } else {
        setErrorString(m_process->errorString());
        m_process->deleteLater();
        m_process = nullptr;
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
    }
}

void Camera2Preview::stop()
{
    if (!m_process) {
        m_buffer.clear();
        return;
    }
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(500)) {
            process->kill();
            process->waitForFinished(500);
        }
    }
    process->deleteLater();
    m_buffer.clear();
    m_captureTimer.stop();
    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    m_previewCaptureRunning = false;
    if (m_running) {
        m_running = false;
        emit runningChanged();
        emit jpegCaptureReadyChanged();
        emit rawCaptureReadyChanged();
    }
}

void Camera2Preview::sendSettings()
{
    sendSettings(false);
}

void Camera2Preview::sendSettings(bool captureExposure)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        return;
    }

    const int sensorSensitivity = captureExposure ? m_sensorSensitivity : 0;
    const QString exposureTime = captureExposure ? m_exposureTime
                                                 : QStringLiteral("0");

    m_process->write(QStringLiteral("settings %1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11\n")
                     .arg(m_focusMode)
                     .arg(m_focusDistance, 0, 'f', 4)
                     .arg(m_exposureCompensation)
                     .arg(m_sceneMode)
                     .arg(m_colorTemperature)
                     .arg(m_colorTint)
                     .arg(sensorSensitivity)
                     .arg(exposureTime)
                     .arg(m_aperture)
                     .arg(m_noiseReduction)
                     .arg(m_zoom, 0, 'f', 4)
                     .toLocal8Bit());
}

void Camera2Preview::parseFrames()
{
    bool updated = false;
    while (true) {
        static const QByteArray frameMagic("SF2P", 4);
        static const QByteArray metadataMagic("SF2M", 4);
        if (!m_buffer.startsWith(frameMagic) &&
                !m_buffer.startsWith(metadataMagic)) {
            const int frameMarker = m_buffer.indexOf(frameMagic);
            const int metadataMarker = m_buffer.indexOf(metadataMagic);
            int marker = -1;
            if (frameMarker >= 0 && metadataMarker >= 0) {
                marker = qMin(frameMarker, metadataMarker);
            } else {
                marker = qMax(frameMarker, metadataMarker);
            }
            if (marker < 0) {
                m_buffer.clear();
                break;
            }
            m_buffer.remove(0, marker);
        }

        if (m_buffer.startsWith(metadataMagic)) {
            const int headerBytes = 8;
            if (m_buffer.size() < headerBytes) {
                break;
            }
            const quint32 payloadBytesValue = readLe32(m_buffer.constData() + 4);
            if (payloadBytesValue == 0 || payloadBytesValue > 8192) {
                m_buffer.remove(0, metadataMagic.size());
                continue;
            }
            const int payloadBytes = int(payloadBytesValue);
            if (m_buffer.size() < headerBytes + payloadBytes) {
                break;
            }
            parseMetadata(m_buffer.mid(headerBytes, payloadBytes));
            m_buffer.remove(0, headerBytes + payloadBytes);
            continue;
        }

        const int headerBytes = 16;
        if (m_buffer.size() < headerBytes) {
            break;
        }

        const quint32 widthValue = readLe32(m_buffer.constData() + 4);
        const quint32 heightValue = readLe32(m_buffer.constData() + 8);
        const quint32 frameBytesValue = readLe32(m_buffer.constData() + 12);
        const quint64 expectedBytes = quint64(widthValue) * quint64(heightValue) * 3;
        if (widthValue == 0 || heightValue == 0 || frameBytesValue == 0 ||
                expectedBytes > quint64(INT_MAX) ||
                frameBytesValue != expectedBytes) {
            m_buffer.remove(0, frameMagic.size());
            continue;
        }
        const int width = int(widthValue);
        const int height = int(heightValue);
        const int frameBytes = int(frameBytesValue);
        if (m_buffer.size() < headerBytes + frameBytes) {
            break;
        }

        const uchar *bits = reinterpret_cast<const uchar *>(m_buffer.constData() + headerBytes);
        QImage frame = QImage(bits, width, height, width * 3, QImage::Format_RGB888).copy();
        if (m_orientation != 0) {
            frame = frame.transformed(QTransform().rotate(m_orientation));
        }
        if (m_mirror) {
            frame = frame.mirrored(true, false);
        }
        updateHistogram(frame);
        m_frame = frame;
        m_buffer.remove(0, headerBytes + frameBytes);
        updated = true;
    }
    if (updated) {
        update();
    }
}

void Camera2Preview::updateHistogram(const QImage &frame)
{
    if (frame.isNull()) {
        return;
    }

    int redBins[HistogramBins] = {};
    int greenBins[HistogramBins] = {};
    int blueBins[HistogramBins] = {};
    const qreal isoGain = m_sensorSensitivity > 0 && m_liveSensorSensitivity > 0
            ? qreal(m_sensorSensitivity) / qreal(m_liveSensorSensitivity) : 1.0;
    const qreal captureExposure = exposureNsFromText(m_exposureTime);
    const qreal previewExposure = exposureNsFromText(m_liveExposureTime);
    const qreal shutterGain = captureExposure > 0.0 && previewExposure > 0.0
            ? captureExposure / previewExposure : 1.0;
    const qreal gain = isoGain * shutterGain;
    const int step = qMax(1, qMin(frame.width(), frame.height()) / 160);
    for (int y = 0; y < frame.height(); y += step) {
        const uchar *line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); x += step) {
            const uchar *pixel = line + x * 3;
            const int red = qBound(0, int(std::round(pixel[0] * gain)), 255);
            const int green = qBound(0, int(std::round(pixel[1] * gain)), 255);
            const int blue = qBound(0, int(std::round(pixel[2] * gain)), 255);
            ++redBins[qBound(0, red * HistogramBins / 256, HistogramBins - 1)];
            ++greenBins[qBound(0, green * HistogramBins / 256, HistogramBins - 1)];
            ++blueBins[qBound(0, blue * HistogramBins / 256, HistogramBins - 1)];
        }
    }

    QVariantList histogram;
    histogram.reserve(HistogramBins);
    for (int index = 0; index < HistogramBins; ++index) {
        QVariantMap bin;
        bin.insert(QStringLiteral("r"), redBins[index]);
        bin.insert(QStringLiteral("g"), greenBins[index]);
        bin.insert(QStringLiteral("b"), blueBins[index]);
        histogram.append(bin);
    }

    if (m_histogram != histogram) {
        m_histogram = histogram;
        emit histogramChanged();
    }
}

void Camera2Preview::parseMetadata(const QByteArray &payload)
{
    const QStringList fields = QString::fromLatin1(payload).simplified().split(QLatin1Char(' '));
    QMap<QString, QString> parsedFields;
    for (const QString &field : fields) {
        const int separator = field.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            continue;
        }

        const QString key = field.left(separator);
        const QString value = field.mid(separator + 1);
        parsedFields.insert(key, value);
        if (key == QLatin1String("focal")) {
            bool ok = false;
            const qreal focal = value.toDouble(&ok);
            if (ok && !qFuzzyCompare(m_focalLength, focal)) {
                m_focalLength = focal;
                emit focalLengthChanged();
            }
        } else if (key == QLatin1String("iso")) {
            bool ok = false;
            const int iso = value.toInt(&ok);
            if (ok && m_liveSensorSensitivity != iso) {
                m_liveSensorSensitivity = iso;
                emit liveSensorSensitivityChanged();
            }
        } else if (key == QLatin1String("shutter")) {
            bool ok = false;
            const qlonglong shutter = value.toLongLong(&ok);
            const QString shutterText = ok && shutter > 0
                    ? QString::number(shutter) : QStringLiteral("0");
            if (m_liveExposureTime != shutterText) {
                m_liveExposureTime = shutterText;
                emit liveExposureTimeChanged();
            }
        }
    }
    if (parsedFields.contains(QStringLiteral("capture-status"))) {
        parseCaptureResult(parsedFields);
    }
}

void Camera2Preview::parseCaptureResult(const QMap<QString, QString> &fields)
{
    if (!m_previewCaptureRunning) {
        return;
    }

    const QString status = fields.value(QStringLiteral("capture-status"));
    const QString path = fields.value(QStringLiteral("path"), m_pendingCapturePath);
    const bool rawCapture = m_pendingRawCapture;
    const QString rawPath = m_pendingCaptureFinalPath;
    m_captureTimer.stop();
    m_pendingCapturePath.clear();
    m_pendingCaptureFinalPath.clear();
    m_pendingRawCapture = false;
    m_pendingCaptureSize = -1;
    m_pendingCaptureStablePolls = 0;
    m_previewCaptureRunning = false;
    if (m_restorePreviewSettingsAfterCapture) {
        m_restorePreviewSettingsAfterCapture = false;
        sendSettings(false);
    }

    if (status == QLatin1String("ok")) {
        if (rawCapture) {
            emit rawImageReady(rawPath, path);
            return;
        }
        savePreviewMetadata(path, sizeFromString(m_captureSize));
        emit imageCaptured(path, QStringLiteral("image/jpeg"));
    } else {
        emit imageCaptureFailed(QStringLiteral("Camera2 warm capture failed"));
    }
}

void Camera2Preview::setErrorString(const QString &errorString)
{
    if (m_errorString == errorString) {
        return;
    }
    m_errorString = errorString;
    emit errorStringChanged();
}

QString Camera2Preview::helperPath() const
{
    const QString override = QString::fromLocal8Bit(qgetenv("SFOS_CAMERA2_PROBE"));
    return override.isEmpty()
            ? QStringLiteral("/usr/libexec/rawfish/sfos-camera2-probe")
            : override;
}
