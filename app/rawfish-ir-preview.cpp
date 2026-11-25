/*
 * rawfish-ir-preview.cpp -- see rawfish-ir-preview.h.
 */
#include "rawfish-ir-preview.h"
#include "hwprobe.h"
#include "hwframes.h"
#include "hwegl.h"
#include "procstats.h"
#include "conftable.h"   /* every key of rawfish-ir.conf: section, default, values, text */
#include <QTimer>
#include <QFileSystemWatcher>
#include <QElapsedTimer>
#include <QProcessEnvironment>

#include <QSGSimpleTextureNode>
#include <QSGGeometryNode>
#include <QSGTextureMaterial>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QSharedPointer>
#include <cmath>
#include <QQuickWindow>
#include <QtEndian>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTransform>
#include <signal.h>
#include <sys/stat.h>
#include <QSettings>
#include <QRegularExpression>
#include <QStorageInfo>
static QString settingsPath();   /* defined with the settings code below */
static QString confKey(const QString &key);   /* a key's place in rawfish-ir.conf, "section/key" (below) */
static QString s_confFromBackup;              /* the earlier version's rawfish-ir.conf, in its backup folder (archiveOnNewVersion) */
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>
#include <QHash>
#include <QtMath>

/* The SF2P frame header: 4 magic bytes "SF2P" then three little-endian uint32s
 * (width, height, byte count), 16 bytes total. Matches the engine's write_sf2p. */
static const char SF2P_MAGIC[4] = { 'S', 'F', '2', 'P' };   // image frame
static const char SF2M_MAGIC[4] = { 'S', 'F', '2', 'M' };   // metadata / event
static const char SF2Y_MAGIC[4] = { 'S', 'F', '2', 'Y' };   // image frame, I420 (0.3.11)
static const char SF2H_MAGIC[4] = { 'S', 'F', '2', 'H' };   // image frame, a buffer handle (zero copy)
static const char SF2A_MAGIC[4] = { 'S', 'F', '2', 'A' };   // the aids' values of the next SF2H frame
static const int SF2P_HEADER = 16;
static const int SF2M_HEADER = 8;

IndrawPreview::IndrawPreview(QQuickItem *parent)
    : QQuickItem(parent)
{
    setOutput(QString(), QString(), QString());   /* the defaults until the QML sets them */
    /* We draw with the scene graph, so mark the item as having visual content. */
    setFlag(ItemHasContents, true);
    /* The overlay worker on its own thread (queued connections: the frame
     * and the result cross threads as implicitly shared QImages). */
    qRegisterMetaType<OverlayParams>("OverlayParams");
    m_overlayThread = new QThread(this);
    m_overlayWorker = new OverlayWorker;
    m_overlayWorker->moveToThread(m_overlayThread);
    connect(m_overlayThread, &QThread::finished, m_overlayWorker, &QObject::deleteLater);
    connect(m_overlayWorker, &OverlayWorker::done, this, &IndrawPreview::onOverlayDone, Qt::QueuedConnection);
    m_overlayThread->start();
    /* "processing" ends 0.6 s after a file is written (a bracket writes
     * several in a row), or at the 45 s cap if no file comes (a failure) --
     * not while an old engine is still finishing its photo. */
    m_procTimer = new QTimer(this);
    m_procTimer->setSingleShot(true);
    connect(m_procTimer, &QTimer::timeout, this, [this]() { if (!m_retiringBusy) { m_shotPending = false; setProcessing(false); } });
}

IndrawPreview::~IndrawPreview()
{
    stop(true);
    delete m_stats;
    m_overlayThread->quit();
    m_overlayThread->wait(2000);
}

bool IndrawPreview::active() const { return m_active; }

void IndrawPreview::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged();
    if (m_active) { m_stopWanted = false; start(); return; }
    /* Inactive (screen off, app behind): the engine stops -- unless a stack
     * is running, which must finish (or be aborted) first; the stop then
     * follows stack_done. Nothing is left running without a reason. */
    if (m_stacking) m_stopWanted = true; else stop();
}

bool IndrawPreview::running() const { return m_running; }

QString IndrawPreview::cameraId() const { return m_cameraId; }
void IndrawPreview::setCameraId(const QString &id)
{
    if (m_cameraId == id) return;
    m_cameraId = id;
    emit cameraIdChanged();
    /* Another camera may hold the YUV stream: try it again. */
    m_yuvFails = 0;
    if (m_yuvFellBack) { m_yuvFellBack = false; emit yuvFellBackChanged(); }
    /* The camera is an argument of the engine: switch by restarting it. */
    if (m_active) { stop(); start(); }
}

QSize IndrawPreview::previewSize() const { return m_previewSize; }
void IndrawPreview::setPreviewSize(const QSize &size)
{
    if (m_previewSize == size) return;
    m_previewSize = size;
    emit previewSizeChanged();
}

QSize IndrawPreview::captureSize() const { return m_captureSize; }
/* The session is made of its streams and its JPEG size: when what is asked
 * differs from what the running engine was started with, it is restarted.
 * A change of type moves several of these at once (snap: the streams AND
 * the JPEG size): the restart is asked once, at the end of the event-loop
 * turn, and only if the engine is past its start (its caps received);
 * otherwise the caps' arrival does the same check. Two restarts in a row, or
 * an engine left with the first value only, were the bugs this avoids. */
bool IndrawPreview::sessionOutdated() const
{
    return m_startedStreams != m_sessionStreams || m_startedOverride != m_jpegOverride;
}
void IndrawPreview::restartIfOutdated()
{
    if (m_restartQueued) return;
    m_restartQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_restartQueued = false;
        if (m_active && m_gotCaps && sessionOutdated()) restart();
    });
}

void IndrawPreview::setJpegSizeOverride(const QSize &s)
{
    if (m_jpegOverride == s) return;
    m_jpegOverride = s;
    emit jpegSizeOverrideChanged();
    restartIfOutdated();
}

void IndrawPreview::setSessionStreams(const QString &s)
{
    if (m_sessionStreams == s) return;
    m_sessionStreams = s;
    emit sessionStreamsChanged();
    restartIfOutdated();
}

void IndrawPreview::setRawSession(bool raw)
{
    if (m_rawSession == raw) return;
    m_rawSession = raw;
    m_rawFellBack = false;
    emit rawSessionChanged();
    /* "streams": "exclusive" (from the caps): the session holds one type's
     * stream, so the type's change reopens it; elsewhere nothing to do. */
    if (m_active && m_gotCaps && m_caps.value(QStringLiteral("limits")).toString().contains(QLatin1String("exclusive"))
            && m_sessionRaw != raw)
        restart();
}

void IndrawPreview::setCaptureSize(const QSize &size)
{
    if (m_captureSize == size) return;
    m_captureSize = size;
    emit captureSizeChanged();
}

bool IndrawPreview::fill() const { return m_fill; }
void IndrawPreview::setFill(bool fill)
{
    if (m_fill == fill) return;
    m_fill = fill;
    emit fillChanged();
    update();                      /* redraw with the new fit                   */
}

QRectF IndrawPreview::viewWindow() const { return m_viewWindow; }
QRectF IndrawPreview::frameCrop() const { return m_frameCrop; }
void IndrawPreview::setViewWindow(const QRectF &w)
{
    if (!(w.width() > 0) || !(w.height() > 0) || w == m_viewWindow) return;
    m_viewWindow = w;
    emit viewWindowChanged();
    update();
}

int IndrawPreview::sensorOrientation() const { return m_sensorOrientation; }
void IndrawPreview::setSensorOrientation(int degrees)
{
    /* Normalise to 0/90/180/270. */
    degrees = ((degrees % 360) + 360) % 360;
    if (m_sensorOrientation == degrees) return;
    m_sensorOrientation = degrees;
    emit sensorOrientationChanged();
}

int IndrawPreview::displayRotation() const { return m_displayRotation; }
void IndrawPreview::setDisplayRotation(int d)
{
    d = ((d % 360) + 360) % 360;
    if (m_displayRotation == d) return;
    m_displayRotation = d;
    emit displayRotationChanged();
    update();
}

QString IndrawPreview::errorString() const { return m_errorString; }

/* --- Process lifecycle --- */

void IndrawPreview::start()
{
    if (m_process)
        return;                    /* already running                          */
    /* The engine before is still finishing (a bracket, a burst, a DNG being
     * written: "quit" waits behind them): the new one opens the camera only
     * once it has ended. Started together, the new engine took the camera
     * while the old one's bracket ran and its last frame was refused (log of
     * 15 Nov 2026, 08:59:53: a change of type during a RAW bracket of 3).
     * A camera closing between two engines takes ~150 ms; that is the wait
     * on a plain camera or type change. */
    if (m_retiring && m_retiring->state() != QProcess::NotRunning) {
        m_startPending = true;
        return;
    }
    m_startPending = false;

    setError(QString());               /* a fresh start clears an old failure */
    m_process = new QProcess(this);
    /* stdout carries the binary SF2P frames; we read it ourselves. stderr (the
     * linker noise) is merged and ignored. The engine's JSON reports go to its
     * own --log file, not to us. */
    m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &IndrawPreview::onReadyRead);
    connect(m_process, &QProcess::errorOccurred,
            this, &IndrawPreview::onProcessError);
    connect(m_process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &IndrawPreview::onProcessFinished);

    /* Build the arguments: indraw --log <file> stream <id> <JPEGsize> <PREVIEWsize> raw=<size>
     * The engine runs its own command loop on stdin, which we keep open to send
     * commands (focus, capture...) later. */
    /* The engine's log goes to the app's cache directory: inside the Sailjail
     * sandbox /tmp is private to the app (invisible from a terminal), while
     * ~/.cache/com.rawfish/rawfish-ir is whitelisted and shared. */
    const QString cache = QDir::homePath() + QStringLiteral("/.cache/com.rawfish/rawfish-ir");
    QDir().mkpath(cache);
    const QString log = cache + QStringLiteral("/rawfish-ir.log");
    /* The engine appends to the log at every start; left alone it grew
     * without bound (ui 171). Past 2 MB it is rotated: the previous
     * rawfish-ir.log.1 goes, the current log becomes it, a fresh one starts.
     * Two files at most, 4 MB at most, and the last two sessions kept. */
    {
        QFileInfo li(log);
        if (li.exists() && li.size() > 2 * 1024 * 1024) {
            QFile::remove(log + QStringLiteral(".1"));
            QFile::rename(log, log + QStringLiteral(".1"));
        }
    }
    /* A memory card may have been put in or taken out since the folders
     * were set: checked again at every engine start (the gallery follows). */
    {
        const QString oldNote = m_storageNote;
        if (resolveFolders() || m_storageNote != oldNote) emit outputChanged();
    }
    /* The output folders exist before the first photo, so the gallery's
     * folder models watch existing directories from the start. */
    QDir().mkpath(m_jpgDir);
    QDir().mkpath(m_dngDir);
    QDir().mkpath(m_dngDir + QStringLiteral("/.previews"));   /* see setOutput */
    QStringList args;
    /* An empty size means "max": the engine picks the largest listed JPEG /
     * RAW16 size and the widest preview <= 1280, so every camera (front
     * included, with its own size list) opens without the UI knowing it. */
    /* "max" also after an engine that died before its caps: a stored size
     * the camera does not list made every restart fail the same way ('jpeg
     * size not listed', 'engine stopped (3 restarts failed)': JP2 front
     * camera, October 2026); the caps of the "max" engine let the page
     * settle a size this camera has. */
    m_gotCaps = false;
    m_startedOverride = m_jpegOverride;
    const QSize capSize = !m_jpegOverride.isEmpty() ? m_jpegOverride : m_captureSize;
    /* No size chosen: the engine's "default" (the largest of at most 13 MP,
     * the menu's own rule), so that it starts where the menu says; after a
     * failure at a chosen size, "max" as before. */
    const QString cap = m_sizeFallback ? QStringLiteral("max") : capSize.isEmpty() ? QStringLiteral("default")
            : QStringLiteral("%1x%2").arg(capSize.width()).arg(capSize.height());
    const QString prev = m_previewSize.isEmpty() ? QStringLiteral("max")
            : QStringLiteral("%1x%2").arg(m_previewSize.width()).arg(m_previewSize.height());
    /* The RAW stream is always "max": the engine picks the camera's RAW16
     * size, or opens without RAW when it lists none (front camera). Tying it
     * to the JPEG size failed on every camera but the back one ("raw16 size
     * not listed") and on 16:9 JPEG sizes. */
    /* The type's session ("streams": "exclusive" phones only; the engine
     * ignores it elsewhere). A RAW session that died before its caps is not
     * tried again: JPEG, and the page is told once the caps confirm. */
    m_sessionRaw = m_rawSession && !m_rawFellBack;
    m_sessionYuv = !m_yuvFellBack && m_sessionStreams.contains(QLatin1String("yuv"));
    m_startedStreams = m_sessionStreams;
    args << QStringLiteral("--log") << log
         << QStringLiteral("--jpgdir") << m_jpgDir << QStringLiteral("--dngdir") << m_dngDir
         << QStringLiteral("stream")
         << m_cameraId
         << cap << prev << QStringLiteral("raw=max")
         << (m_sessionRaw ? QStringLiteral("type=raw") : QStringLiteral("type=jpeg"));
    if (m_sessionYuv) {
        /* The YUV stream's size: the photo's (jpeg) unless rawfish-ir.conf says
         * otherwise (jpgYuvStream = jpeg | max | WxH): a 12-megapixel YUV
         * stream slows the JP2's viewfinder, a test key while this is sorted out. */
        QString ys = QSettings(settingsPath(), QSettings::IniFormat).value(confKey(QStringLiteral("jpgYuvStream")), QStringLiteral("jpeg")).toString().trimmed();
        static const QRegularExpression ok(QStringLiteral("^(jpeg|max|[0-9]{3,5}x[0-9]{3,5})$"));
        if (!ok.match(ys).hasMatch()) ys = QStringLiteral("jpeg");
        /* jpgYuvFromLargestStream (on by default since it was tried on the
         * phone, 14 Nov 2026): with "jpeg", and where no listed stream is a
         * multiple of the photo's size, the camera's largest stream averaged
         * down (the selfie camera's viewfinder lagged without). Only "0" turns
         * it off (a key absent counts as on, as the example's default); with
         * max or WxH it means nothing. */
        if (ys == QLatin1String("jpeg") &&
            QSettings(settingsPath(), QSettings::IniFormat).value(confKey(QStringLiteral("jpgYuvFromLargestStream")), QStringLiteral("1")).toString().trimmed() != QLatin1String("0"))
            ys = QStringLiteral("jpeg+largest");
        args << QStringLiteral("yuv=") + ys;
    }
    {   /* only the streams the type and the settings need (yuv dropped after it failed) */
        QStringList want;
        for (const QString &s : m_sessionStreams.split(QLatin1Char(','), QString::SkipEmptyParts))
            if (s != QLatin1String("yuv") || m_sessionYuv) want << s;
        if (m_yuvFellBack && m_sessionStreams.contains(QLatin1String("yuv")) && !want.contains(QStringLiteral("jpeg")))
            want << QStringLiteral("jpeg");       /* JPG then takes the camera's JPEG */
        args << QStringLiteral("streams=") + want.join(QLatin1Char(','));
    }

    m_buffer.clear();
    m_vfModeSent.clear();          /* a new engine starts with SF2P: asked again */
    m_aidsSent = -1; m_aidsGot = false; m_aidsUnsupported = false;
    m_aidMaps.clear(); m_aidMask = 0;
    m_hwbId = 0;
    /* The frames' format (syncVfMode): zero copy unless rawfish-ir.conf
     * viewfinderZeroCopy=0, the YUV pipe unless viewfinderYuv=0; the
     * viewfinder's buffers from vfBuffers (4-10, default 6). */
    {
        QSettings conf(settingsPath(), QSettings::IniFormat);
        m_liveConf.clear();
        readLiveConf(false);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const int nb = conf.value(confKey(QStringLiteral("vfBuffers")), 6).toInt();
        if (nb >= 4 && nb <= 10) env.insert(QStringLiteral("RAWFISH_VF_BUFFERS"), QString::number(nb));
        m_process->setProcessEnvironment(env);
        if (m_hwframes) m_hwframes->clearFailed();   /* each engine tries again */
    }
    /* hwprobe (branch zero_copy, docs/ROADMAP.md): rawfish-ir.conf hwProbe=N
     * sends the engine's next N viewfinder buffers to be imported here
     * without a copy (hwprobe.h); hwProbeGpuUsage=1 has them allocated for
     * GPU sampling too. Off unless asked; read at each engine start. */
    {
        QSettings conf(settingsPath(), QSettings::IniFormat);
        m_hwpFrames = qBound(0, conf.value(confKey(QStringLiteral("hwProbe")), 0).toInt(), 20);
        m_hwpAsked = false;
        if (m_hwpFrames > 0) {
            if (!m_hwprobe) {
                m_hwprobe = new HwProbe(this);
                connect(m_hwprobe, &HwProbe::bufferArrived, this, &QQuickItem::update);
                m_hwprobe->listen(cache + QStringLiteral("/hwprobe.sock"), cache);
            }
            QProcessEnvironment env = m_process->processEnvironment();
            if (conf.value(confKey(QStringLiteral("hwProbeGpuUsage")), 0).toInt() == 1)
                env.insert(QStringLiteral("RAWFISH_PREVIEW_GPU_USAGE"), QStringLiteral("1"));
            m_process->setProcessEnvironment(env);
        }
    }
    /* Inside the Sailjail sandbox /usr/bin only holds the app binary
     * (firejail --private-bin), so "rawfish-ir-cli" in the PATH does not exist
     * there: use the engine's own directory, /usr/libexec/rawfish-ir, visible in the
     * sandbox, and fall back to the PATH for an unsandboxed/dev run. */
    const QString libexec = QStringLiteral("/usr/libexec/rawfish-ir/rawfish-ir-cli");
    const QString engine = QFileInfo(libexec).isExecutable() ? libexec : QStringLiteral("rawfish-ir-cli");
    m_process->start(engine, args);
    m_lastTouch.restart();                 /* opening the camera counts as a touch */
    updateKeepScreenOn();
}

void IndrawPreview::stop(bool wait)
{
    if (!m_process)
        return;
    if (m_keepScreenOn) { m_keepScreenOn = false; emit keepScreenOnChanged(); }
    /* This process is on its way out: none of its signals must reach us any
     * more. Its "finished" (emitted inside waitForFinished, or later after a
     * kill) used to be taken for the NEW engine dying: "engine stopped" on a
     * camera switch while the new engine was streaming. */
    QProcess *old = m_process;
    const bool busy = m_shotPending;  /* a photo still in this engine: see it out */
    m_process = nullptr;
    old->disconnect(this);
    m_retiring = old;              /* start() waits for its end              */
    if (m_stacking) { m_stacking = false; emit stackingChanged(); }
    /* Ask the engine to quit cleanly (closes the camera), then tear down.
     * Not waited for on the UI thread (ui 168): a stack sub-frame of 0.5 s
     * plus a DNG write could hold the engine for seconds, and the camera
     * switch or the sleep froze that long. The old process is deleted when
     * it ends, and killed if it has not ended in 30 s (3 s up to 0.3.9: a
     * bracket of RAWs or a burst finishing behind "quit" was cut, and 8
     * frames of 50 MP take about 13 s to merge -- a switch to RAW during it
     * lost the photo); a photo still in it keeps "processing" on until then
     * (the dots round the TYPE button), and a start asked meanwhile waits for
     * its end (start()). The destructor
     * (the app closing) waits instead: the process would be killed with it. */
    if (old->state() != QProcess::NotRunning) {
        old->write("quit\n");
        old->closeWriteChannel();
        if (wait) {
            if (!old->waitForFinished(3000)) old->kill();
            old->deleteLater();
        } else {
            connect(old, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                    old, &QObject::deleteLater);
            /* its end lets a start that waited for it go (a crash or a kill too) */
            connect(old, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                    this, [this]() { if (m_startPending && m_active && !m_process) start(); });
            /* a photo being computed ends it: "processing" until then */
            if (busy) {
                m_retiringBusy = true;
                connect(old, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                        this, [this]() { m_retiringBusy = false; m_shotPending = false; setProcessing(false); });
                setProcessing(true);       /* the photo goes on in the old engine */
            }
            QTimer::singleShot(30000, old, [old]() { if (old->state() != QProcess::NotRunning) old->kill(); });
        }
    } else
        old->deleteLater();
    m_buffer.clear();
    setRunning(false);
}

void IndrawPreview::sendCommand(const QString &command)
{
    if (!m_process || m_process->state() != QProcess::Running)
        return;
    m_process->write(command.toUtf8());
    m_process->write("\n");        /* the engine reads one command per line     */
    if (command.startsWith(QLatin1String("sh "))) { m_shotPending = true; m_procTimer->start(45000); }   /* the dots: at the flash */
}

/* Settings: a plain INI file in the app's config directory (whitelisted by
 * Sailjail), so the UI preferences survive restarts without dconf. */
static QString settingsPath()
{
    const QString dir = QDir::homePath() + QStringLiteral("/.config/com.rawfish/rawfish-ir");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/rawfish-ir.conf");
}
/* The memory card a folder lies on: "/run/media/<user>/<card>" for a folder
 * at or below it (where the system mounts memory cards and USB drives),
 * "" for any other folder. */
static QString cardRoot(const QString &folder)
{
    static const QString media = QStringLiteral("/run/media/");
    if (!folder.startsWith(media)) return QString();
    const QStringList parts = folder.mid(media.size()).split(QLatin1Char('/'), QString::SkipEmptyParts);
    if (parts.size() < 2) return QString();      /* /run/media/<user> itself: no card named */
    return media + parts.at(0) + QLatin1Char('/') + parts.at(1);
}

/* True when a file system is mounted on `root`: it exists, is a folder, and
 * lies on another device than its parent folder. (When a card is taken out
 * the system removes its folder; an empty folder left behind is on the
 * parent's device, and writing there would fill the phone's /run, a small
 * memory-backed file system, with photos lost at the next boot.) */
static bool isMountPoint(const QString &root)
{
    struct stat s, p;
    const QByteArray r = QFile::encodeName(root), up = QFile::encodeName(QFileInfo(root).absolutePath());
    if (stat(r.constData(), &s) != 0 || !S_ISDIR(s.st_mode)) return false;
    if (stat(up.constData(), &p) != 0) return false;
    return s.st_dev != p.st_dev;
}

bool IndrawPreview::resolveFolders()
{
    const QString def = QDir::homePath() + QStringLiteral("/Pictures/RAWfish-ir");
    QStringList missing;
    auto usable = [&](const QString &want) {
        const QString root = cardRoot(want);
        if (root.isEmpty() || isMountPoint(root)) return want;
        if (!missing.contains(root)) missing << root;
        return def;
    };
    const QString jp = usable(m_jpgWant), dn = usable(m_dngWant);
    m_storageNote = missing.isEmpty() ? QString()
        : QStringLiteral("storage: memory card %1 not mounted: saving to %2 instead").arg(missing.join(QStringLiteral(", ")), def);
    const bool changed = jp != m_jpgDir || dn != m_dngDir;
    m_jpgDir = jp; m_dngDir = dn;
    return changed;
}

/* Output settings, from dconf through the QML (ConfigurationGroup). The
 * folders the sandbox lets the app write to are ~/Pictures and, with the
 * RemovableMedia permission, the memory cards under /run/media/<user>; any
 * other folder is accepted (a developer may run unsandboxed), but an empty
 * or relative value and a prefix outside the engine's safe charset fall back
 * to the defaults. A folder on a memory card that is not mounted is replaced
 * by the default one (resolveFolders). An empty prefix is a choice (files
 * named by the date alone): kept as is, sent to the engine as its "no
 * prefix" NAME (capturePrefix). */
void IndrawPreview::setOutput(const QString &prefix, const QString &jpgDir, const QString &dngDir)
{
    const QString def = QDir::homePath() + QStringLiteral("/Pictures/RAWfish-ir");
    auto folder = [&](QString v) {
        v = v.trimmed();
        if (v.startsWith(QLatin1String("~/"))) v = QDir::homePath() + v.mid(1);
        if (v.isEmpty() || !v.startsWith(QLatin1Char('/'))) v = def;
        while (v.size() > 1 && v.endsWith(QLatin1Char('/'))) v.chop(1);
        return v;
    };
    /* The settings page's rule: not starting with '-' ("-" alone is the
     * engine's "measure only", no file). */
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_-]{0,31}$"));
    const QString pf = prefix.isEmpty() || safe.match(prefix).hasMatch() ? prefix : QStringLiteral("photo");
    m_jpgWant = folder(jpgDir); m_dngWant = folder(dngDir);
    const QString oldNote = m_storageNote;
    const bool dirsChanged = resolveFolders();
    if (!dirsChanged && pf == m_prefix && m_storageNote == oldNote) return;
    m_prefix = pf;
    QDir().mkpath(m_jpgDir);
    QDir().mkpath(m_dngDir);
    /* The DNG previews' hidden folder exists from the start: a folder model
     * set on a folder that does not exist yet never notices its creation
     * (the gallery showed no previews in a new DNG folder). */
    QDir().mkpath(m_dngDir + QStringLiteral("/.previews"));
    emit outputChanged();
    /* The folders are arguments of the engine: a running one would go on
     * writing to the old ones. The prefix goes with each shot: no restart. */
    if (dirsChanged && m_active && m_gotCaps) restart();
}
QString IndrawPreview::jpgDir() const { return m_jpgDir; }
QString IndrawPreview::dngDir() const { return m_dngDir; }
/* The NAME of a shot's command: the prefix, or "+" (the engine's "no
 * prefix") when it is empty. */
QString IndrawPreview::capturePrefix() const { return m_prefix.isEmpty() ? QStringLiteral("+") : m_prefix; }

/* The first start of a newly installed version sets the user's earlier
 * files aside, so that the version starts from its own defaults and its own
 * device profile, and nothing left by an earlier version (a stale key, a
 * calibration that no longer fits) can make it behave differently from a
 * fresh install. Nothing is deleted: the files are MOVED into
 *   ~/.config/com.rawfish/rawfish-ir/backup-<earlier version>-<date>/
 * keeping their folder (config/ or data/), and the log says what moved. To
 * take one back, copy it from there to where it was.
 * Moved: rawfish-ir.conf, simple.json (configuration), override-hal.json,
 * focus-calibration.json and the measured lens-shading maps (device
 * profiles). rawfish-ir.conf is moved too, but its values are carried into
 * the new one (writeDefaultSettings, 0.3.11-0551): the settings stay the
 * user's, the file is the new version's. Kept: the photos, the Settings page's choices (output folders,
 * prefix: in dconf), the examples (rewritten anyway), the log.
 * "installed-version" in the configuration folder records the version
 * whose files are in use (the version only, not the package release: a new
 * patch build of the same version keeps them); a build outside the package
 * ("dev") moves nothing. */
QString IndrawPreview::s_archiveNote;
QString IndrawPreview::archiveOnNewVersion()
{
    /* The version alone, without the package release: since 0.3.9 every
     * build carries its patch number as the release (0.3.9-433.beta,
     * scripts/with-build-release.sh), and comparing the whole string set the
     * settings, simple.json and the measured maps aside at every patch
     * installed -- the app came back in simple mode each time (15 Nov 2026).
     * A new version (0.3.10 -> 0.3.11) still starts from its own defaults. */
    const QString full = QStringLiteral(APP_VERSION);
    const QString cur = full.section(QLatin1Char('-'), 0, 0);
    const QString cfg = QDir::homePath() + QStringLiteral("/.config/com.rawfish/rawfish-ir");
    const QString data = QDir::homePath() + QStringLiteral("/.local/share/com.rawfish/rawfish-ir/device-profiles");
    const QString stampPath = cfg + QStringLiteral("/installed-version");
    QString prev;
    QStringList moved;
    if (cur == QLatin1String("dev")) return QString();
    QDir().mkpath(cfg);
    {
        QFile f(stampPath);
        if (f.open(QIODevice::ReadOnly)) prev = QString::fromUtf8(f.readAll()).trimmed().section(QLatin1Char('-'), 0, 0);
    }   /* a stamp written by 0.3.9-425..433 holds the release too: compared without it */
    if (prev == cur) return QString();
    {
        const struct { QString dir, sub, name; } files[] = {
            { cfg,  QStringLiteral("config"), QStringLiteral("rawfish-ir.conf") },
            { cfg,  QStringLiteral("config"), QStringLiteral("rawfish-ir.conf.bak") },
            { cfg,  QStringLiteral("config"), QStringLiteral("simple.json") },
            { data, QStringLiteral("data"),   QStringLiteral("override-hal.json") },
            { data, QStringLiteral("data"),   QStringLiteral("focus-calibration.json") },
        };
        const QString backup = cfg + QStringLiteral("/backup-") + (prev.isEmpty() ? QStringLiteral("before-") + cur : prev)
                             + QLatin1Char('-') + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        for (const auto &e : files) {
            const QString from = e.dir + QLatin1Char('/') + e.name;
            if (!QFile::exists(from)) continue;
            QDir().mkpath(backup + QLatin1Char('/') + e.sub);
            if (QFile::rename(from, backup + QLatin1Char('/') + e.sub + QLatin1Char('/') + e.name)) {
                moved << e.sub + QLatin1Char('/') + e.name;
                /* rawfish-ir.conf: its values go into the new file (writeDefaultSettings) */
                if (e.name == QLatin1String("rawfish-ir.conf")) s_confFromBackup = backup + QStringLiteral("/config/rawfish-ir.conf");
            }
        }
        /* the measured lens-shading maps: the folder as a whole */
        const QString shade = data + QStringLiteral("/lens-shading");
        if (QDir(shade).exists() && !QDir(shade).entryList(QDir::Files).isEmpty()) {
            QDir().mkpath(backup + QStringLiteral("/data"));
            if (QDir().rename(shade, backup + QStringLiteral("/data/lens-shading"))) moved << QStringLiteral("data/lens-shading/");
        }
        if (moved.isEmpty()) QDir().rmdir(backup);
        QFile f(stampPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(cur.toUtf8() + '\n');
        if (moved.isEmpty()) return QString();
        return QStringLiteral("new version %1 (was %2): earlier files set aside in %3: %4")
                .arg(cur, prev.isEmpty() ? QStringLiteral("unknown") : prev, backup, moved.join(QStringLiteral(", ")));
    }
}

void IndrawPreview::installExamples()
{
    const QString src = QStringLiteral("/usr/share/rawfish-ir/examples/");
    const QString profiles = QDir::homePath() + QStringLiteral("/.local/share/com.rawfish/rawfish-ir/device-profiles");
    QDir().mkpath(profiles);
    {   /* the device profile's: copied from the package */
        const QString from = src + QStringLiteral("override-hal.example.jsonc"), to = profiles + QStringLiteral("/override-hal.example.jsonc");
        if (QFile::exists(from)) { QFile::remove(to); QFile::copy(from, to); }   /* refreshed: the version's own */
    }
    {   /* rawfish-ir.conf's: written from the app's own table (conftable.h) */
        const QString to = configDir() + QStringLiteral("/rawfish-ir.conf.example");
        const QByteArray tmp = QFile::encodeName(to + QStringLiteral(".new"));
        FILE *f = fopen(tmp.constData(), "w");
        if (!f) return;
        const int bad = conf_write_example(f);
        if (fclose(f) != 0 || bad) { ::remove(tmp.constData()); return; }
        QFile::remove(to);
        QFile::rename(QString::fromLocal8Bit(tmp), to);
    }
}

/* ---- rawfish-ir.conf (0.3.11, patch 0551) --------------------------------
 *
 * Every key is written down once, in conftable.h: its section, its default,
 * the values it takes and its text. From that table:
 *  - confKey() places a key in its section: the interface asks for a key by
 *    its name ("timerSec"), QSettings keeps it as "menu/timerSec", which it
 *    writes as [menu] timerSec=...; a camera's own keys are named by the
 *    menu itself ("camera_2/jpgSharpen");
 *  - writeDefaultSettings() makes the file whole at each start (below);
 *  - installExamples() writes rawfish-ir.conf.example, the user's reference
 *    (nothing reads it).
 * The file is written by QSettings: in alphabetical order (the sections, and
 * the keys in each), without comments. Called from main() before the
 * interface reads any setting: QSettings keeps one cached copy of a file per
 * process, and a file changed behind it after a first read would be
 * overwritten from that copy at the next save. */
static QString confKey(const QString &key)
{
    if (key.contains(QLatin1Char('/'))) return key;
    for (int i = 0; k_conf_keys[i].key; ++i) {
        const conf_key_t &k = k_conf_keys[i];
        if (!*k.section || !strcmp(k.section, "camera")) continue;   /* the top's old keys; [camera_N] named by the menu */
        if (key == QLatin1String(k.key)) return QLatin1String(k.section) + QLatin1Char('/') + key;
    }
    return key;                                   /* unknown: at the top (dropped at the next start) */
}

/* The text of a value as written (QSettings reads an unquoted "a,b,c" as a
 * list: joined back). */
static QString confText(const QVariant &v)
{
    return v.type() == QVariant::StringList ? v.toStringList().join(QLatin1Char(',')) : v.toString().trimmed();
}

/* Is "group/key", or a top-level "key", one of the table's? */
static const conf_key_t *confKnown(const QString &full)
{
    static const QRegularExpression cam(QStringLiteral("^camera_[0-9]+$"));
    const int slash = full.indexOf(QLatin1Char('/'));
    const QString group = slash > 0 ? full.left(slash) : QString(), key = slash > 0 ? full.mid(slash + 1) : full;
    const QByteArray k = key.toUtf8();
    if (group.isEmpty()) return conf_find("", k.constData());
    if (cam.match(group).hasMatch()) return conf_find("camera", k.constData());
    return conf_find(group.toUtf8().constData(), k.constData());
}

/* A new rawfish-ir.conf at `path`: every key at its default, and the values
 * of `oldPath` (the user's file of before: a new version's, kept in its
 * backup folder, or this one before the sections, copied there first) for
 * the keys the table still has and that pass their check -- in their
 * section, or at the top of a file of before the sections. A key no longer
 * in the table is left out, a value no longer allowed takes the default.
 * The [camera_N] sections are carried the same way ("auto" becomes
 * "default"). Written to a new file, then put in place. Returns a line for
 * the log, "" on success with nothing to say. */
static QString confRebuild(const QString &path, const QString &oldPath)
{
    static const QRegularExpression cam(QStringLiteral("^camera_[0-9]+$"));
    const QString tmp = path + QStringLiteral(".new");
    int carried = 0, refused = 0;
    QStringList refusedKeys;
    QFile::remove(tmp);
    {
        QSettings s(tmp, QSettings::IniFormat);
        QScopedPointer<QSettings> old(oldPath.isEmpty() ? nullptr : new QSettings(oldPath, QSettings::IniFormat));
        for (int i = 0; k_conf_keys[i].key; ++i) {
            const conf_key_t &k = k_conf_keys[i];
            if (!strcmp(k.section, "camera")) continue;          /* below, per camera */
            const QString key = QLatin1String(k.key);
            const QString full = *k.section ? QLatin1String(k.section) + QLatin1Char('/') + key : key;
            QVariant v;
            if (old && old->contains(full)) v = old->value(full);
            else if (old && *k.section && old->contains(key)) v = old->value(key);   /* a file of before the sections */
            if (v.isValid() && conf_value_ok(k.check, confText(v).toUtf8().constData())) {
                s.setValue(full, v);
                ++carried;
            } else {
                if (v.isValid()) { ++refused; refusedKeys << full; }
                if (k.def) s.setValue(full, QString::fromUtf8(k.def));
            }
        }
        if (old)
            for (const QString &g : old->childGroups()) {
                if (!cam.match(g).hasMatch()) continue;
                for (int i = 0; k_conf_keys[i].key; ++i) {
                    const conf_key_t &k = k_conf_keys[i];
                    if (strcmp(k.section, "camera")) continue;
                    const QString full = g + QLatin1Char('/') + QLatin1String(k.key);
                    QString v = old->contains(full) ? confText(old->value(full)) : QString::fromUtf8(k.def);
                    if (v == QLatin1String("auto")) v = QStringLiteral("default");   /* the luma's word until 0.3.11-550 */
                    if (!conf_value_ok(k.check, v.toUtf8().constData())) { ++refused; refusedKeys << full; v = QString::fromUtf8(k.def); }
                    else ++carried;
                    s.setValue(full, v);
                }
            }
        s.sync();
        if (s.status() != QSettings::NoError) { QFile::remove(tmp); return QStringLiteral("rawfish-ir.conf: could not write the new file; the old one is kept"); }
    }
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) return QStringLiteral("rawfish-ir.conf: could not put the new file in place (%1)").arg(tmp);
    if (oldPath.isEmpty()) return QString();
    return QStringLiteral("rawfish-ir.conf: a new file, %1 value(s) carried from %2%3")
            .arg(carried).arg(oldPath)
            .arg(refused ? QStringLiteral("; %1 not allowed any more, at the default: %2").arg(refused).arg(refusedKeys.join(QStringLiteral(", "))) : QString());
}

/* Is the file at `path` of before the sections (0.3.11-550 and earlier): a
 * key of the table that has a section, at the top or under [General]? Read
 * as text, not through QSettings: the file is about to be replaced, and
 * QSettings must not hold a copy of it (see above). */
static bool confBeforeSections(const QString &path)
{
    QFile f(path);
    QString section;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) { section = QString::fromUtf8(line.mid(1, line.size() - 2)); continue; }
        const int eq = line.indexOf('=');
        if (eq <= 0 || line.startsWith(';') || (!section.isEmpty() && section != QLatin1String("General"))) continue;
        const QString key = QString::fromUtf8(line.left(eq)).trimmed();
        if (confKey(key) != key) return true;
    }
    return false;
}

/* rawfish-ir.conf whole at each start:
 *  - a new version (archiveOnNewVersion moved the old file to its backup
 *    folder): a new file, the old one's values carried (confRebuild);
 *  - no file (a new install, or deleted): a new file at the defaults;
 *  - a file of before the sections: copied to a backup folder, then
 *    rebuilt from that copy, every value kept;
 *  - otherwise the keys the file lacks are added at their defaults, and the
 *    keys the table does not have (an older version's, or a key put in the
 *    wrong section) are dropped -- except the old one-for-all JPG keys at
 *    the top, which the menu carries into [camera_N] once (it needs the
 *    cameras' list, which only the engine gives). */
QString IndrawPreview::s_confNote;
void IndrawPreview::writeDefaultSettings()
{
    const QString path = settingsPath();
    if (!s_confFromBackup.isEmpty() && QFile::exists(s_confFromBackup)) {
        s_confNote = confRebuild(path, s_confFromBackup);
        return;
    }
    if (!QFile::exists(path)) { s_confNote = confRebuild(path, QString()); return; }
    if (confBeforeSections(path)) {
        const QString backup = QDir::homePath() + QStringLiteral("/.config/com.rawfish/rawfish-ir/backup-before-sections-")
                             + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral("/config");
        const QString copy = backup + QStringLiteral("/rawfish-ir.conf");
        if (!QDir().mkpath(backup) || !QFile::copy(path, copy)) { s_confNote = QStringLiteral("rawfish-ir.conf: could not keep a copy before sorting it into sections: left as it is"); return; }
        s_confNote = confRebuild(path, copy);
        return;
    }
    QSettings s(path, QSettings::IniFormat);
    int changed = 0;
    for (const QString &k : s.allKeys())
        if (!confKnown(k)) { s.remove(k); ++changed; }
    for (int i = 0; k_conf_keys[i].key; ++i) {
        const conf_key_t &k = k_conf_keys[i];
        if (!k.def || !*k.section || !strcmp(k.section, "camera")) continue;   /* the menu writes the cameras' */
        const QString full = QLatin1String(k.section) + QLatin1Char('/') + QLatin1String(k.key);
        if (s.contains(full)) continue;
        s.setValue(full, QString::fromUtf8(k.def));
        ++changed;
    }
    if (changed) s.sync();
}

/* The reset, while the interface runs: through QSettings itself (its cached
 * copy stays right), every key at its default; the cameras' sections go
 * (the menu writes them again, at "default"). */
static void resetToDefaults(const QString &path)
{
    QSettings s(path, QSettings::IniFormat);
    s.clear();
    for (int i = 0; k_conf_keys[i].key; ++i) {
        const conf_key_t &k = k_conf_keys[i];
        if (k.def && *k.section && strcmp(k.section, "camera"))
            s.setValue(QLatin1String(k.section) + QLatin1Char('/') + QLatin1String(k.key), QString::fromUtf8(k.def));
    }
    s.sync();
}

QString IndrawPreview::configDir() const
{
    const QString dir = QDir::homePath() + QStringLiteral("/.config/com.rawfish/rawfish-ir");
    QDir().mkpath(dir);
    return dir;
}
QString IndrawPreview::resetSettingsFile()
{
    const QString path = settingsPath(), bak = path + QStringLiteral(".bak");
    if (!QFile::exists(path)) return QString();
    QFile::remove(bak);
    if (!QFile::copy(path, bak)) return QString();       /* the previous file kept */
    resetToDefaults(path);                               /* every key at its default */
    return bak;
}

/* A key of rawfish-ir.conf by its name, in its section (confKey). */
QVariant IndrawPreview::setting(const QString &key, const QVariant &def) const
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(confKey(key), def);
}
void IndrawPreview::setSetting(const QString &key, const QVariant &value)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(confKey(key), value);
}
/* A key taken out of rawfish-ir.conf (an old key carried into new ones). */
void IndrawPreview::removeSetting(const QString &key)
{
    QSettings(settingsPath(), QSettings::IniFormat).remove(confKey(key));
}

void IndrawPreview::abortCapture()
{
    if (m_process && m_process->processId() > 0)
        ::kill(static_cast<pid_t>(m_process->processId()), SIGUSR1);
}
bool IndrawPreview::runProbe()
{
    const QString libexec = QStringLiteral("/usr/libexec/rawfish-ir/rawfish-ir-cli");
    const QString engine = QFileInfo(libexec).isExecutable() ? libexec : QStringLiteral("rawfish-ir-cli");
    QProcess p;
    p.start(engine, QStringList() << QStringLiteral("probe"));
    if (!p.waitForFinished(15000)) { p.kill(); return false; }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}
bool IndrawPreview::stacking() const { return m_stacking; }

void IndrawPreview::restart()
{
    if (!m_active) return;
    stop();
    start();
}

/* True when `path`, made canonical (no "..", no link leading out), lies in
 * `dir` or below it. The two calls below are reachable from QML: each is
 * kept to the folders it is meant for, whatever path it is given. */
static bool pathWithin(const QString &path, const QString &dir)
{
    if (dir.isEmpty()) return false;
    const QString d = QDir(dir).canonicalPath();
    const QFileInfo fi(path);
    const QString p = fi.exists() ? fi.canonicalFilePath() : QDir(fi.absolutePath()).canonicalPath() + QLatin1Char('/') + fi.fileName();
    return !d.isEmpty() && (p == d || p.startsWith(d + QLatin1Char('/')));
}

/* A JSON object from the app's own configuration folder (simple.json). */
QVariantMap IndrawPreview::readJsonFile(const QString &path)
{
    if (!pathWithin(path, configDir())) return QVariantMap();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1000000) return QVariantMap();
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

/* Deletes a capture or its preview: only inside the two output folders. */
bool IndrawPreview::deleteFile(const QString &path)
{
    if (!pathWithin(path, m_jpgDir) && !pathWithin(path, m_dngDir)) return false;
    return QFile::remove(path);
}

bool IndrawPreview::saveScreenshot(const QString &path)
{
    if (!window()) return false;
    const QImage shot = window()->grabWindow();
    if (shot.isNull()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    return shot.save(path, "PNG");
}

bool IndrawPreview::convertDngPreview(const QString &ppmPath, int orientation)
{
    QImage img(ppmPath);
    if (img.isNull()) { QFile::remove(ppmPath); return false; }
    /* TIFF orientation -> rotation: 3 = 180, 6 = 90 CW, 8 = 90 CCW. */
    int deg = orientation == 6 ? 90 : orientation == 3 ? 180 : orientation == 8 ? 270 : 0;
    if (deg) { QTransform t; t.rotate(deg); img = img.transformed(t); }
    QString jpg = ppmPath;
    jpg.replace(QStringLiteral(".ppm"), QStringLiteral(".jpg"));
    const bool ok = img.save(jpg, "JPEG", 85);
    QFile::remove(ppmPath);
    return ok;
}

bool IndrawPreview::saveFramePreview(const QString &path, int rotation)
{
    const QImage rgb = frameRgb();
    if (rgb.isNull()) return false;
    QTransform t;
    t.rotate(((rotation % 360) + 360) % 360);
    const QImage upright = rgb.transformed(t);
    return upright.save(path, "JPEG", 85);
}

/* --- Reading the frame stream --- */

void IndrawPreview::onReadyRead()
{
    m_buffer.append(m_process->readAllStandardOutput());
    parseFrames();
}

/* Pulls every complete SF2P frame out of the buffer, keeping the latest as the
 * frame to draw. If several frames arrived at once we skip to the newest (the
 * viewfinder only ever shows the most recent image). */
void IndrawPreview::parseFrames()
{
    /* A read index over m_buffer: nothing is removed from its front until the
     * end (one memmove of what remains, instead of one per packet on a
     * multi-MB buffer, ui 167). Several frames in one batch: only the newest
     * complete one is copied into m_frame. */
    bool gotFrame = false;
    int pos = 0, newest = -1;
    quint32 newestW = 0, newestH = 0;
    bool newestYuv = false, newestHwb = false;
    quint64 newestHwbId = 0;
    int aidPos = -1; quint32 aidW = 0, aidH = 0, aidN = 0;   /* the latest SF2A */
    const int size = m_buffer.size();
    const char *base = m_buffer.constData();
    for (;;) {
        if (size - pos < SF2M_HEADER)
            break;                 /* not even the shorter header yet (an SF2M
                                    * packet needs 8 bytes; it waited for 16
                                    * until ui 183: a short event sat until the
                                    * next frame arrived)                      */

        /* Which packet is at the front? SF2M (metadata/event), SF2P (an RGB
         * frame), SF2Y (an I420 frame) or SF2H (the camera buffer a frame is
         * in: zero copy). */
        const bool isM = memcmp(base + pos, SF2M_MAGIC, 4) == 0;
        const bool isY = memcmp(base + pos, SF2Y_MAGIC, 4) == 0;
        const bool isP = memcmp(base + pos, SF2P_MAGIC, 4) == 0;
        const bool isH = memcmp(base + pos, SF2H_MAGIC, 4) == 0;
        const bool isA = memcmp(base + pos, SF2A_MAGIC, 4) == 0;
        if (!isM && !isP && !isY && !isH && !isA) {
            /* Resync: jump to the next occurrence of any magic. */
            int next = -1;
            for (const char *magic : { SF2P_MAGIC, SF2M_MAGIC, SF2Y_MAGIC, SF2H_MAGIC, SF2A_MAGIC }) {
                const int n = m_buffer.indexOf(QByteArray(magic, 4), pos + 1);
                if (n >= 0 && (next < 0 || n < next)) next = n;
            }
            if (next < 0) { pos = size; break; }
            pos = next;
            continue;
        }

        if (isM) {
            /* SF2M: 8-byte header + a text line. Parse the whole packet. */
            const uchar *mp = reinterpret_cast<const uchar *>(base + pos);
            const quint32 mn = qFromLittleEndian<quint32>(mp + 4);
            if (mn > 4096) { ++pos; continue; }                    /* sanity */
            if (size - pos < SF2M_HEADER + int(mn)) break;         /* not all here */
            const QByteArray line = QByteArray(base + pos + SF2M_HEADER, int(mn));
            pos += SF2M_HEADER + int(mn);
            parseEvent(line);
            continue;
        }

        if (size - pos < SF2P_HEADER)
            break;                 /* a frame header is 16 bytes                */
        const uchar *p = reinterpret_cast<const uchar *>(base + pos);
        const quint32 w = qFromLittleEndian<quint32>(p + 4);
        const quint32 h = qFromLittleEndian<quint32>(p + 8);
        const quint32 n = qFromLittleEndian<quint32>(p + 12);

        if (isA) {
            /* SF2A: w x h cells, 16 bytes (the maps' mask, the frame's size),
             * then one cell map per bit of the mask */
            if (w == 0 || h == 0 || w > 8192 || h > 8192 || n < 16) { ++pos; continue; }
            if (size - pos < SF2P_HEADER + 16) break;
            const quint32 mask = qFromLittleEndian<quint32>(p + 16);
            int k = 0;
            for (int b = 1; b <= 8; b <<= 1) if (mask & quint32(b)) ++k;
            if (mask == 0 || mask > 15 || quint64(n) != 16u + quint64(k) * w * h) { ++pos; continue; }
            if (size - pos < SF2P_HEADER + int(n)) break;
            aidPos = pos; aidW = w; aidH = h; aidN = n;
            pos += SF2P_HEADER + int(n);
            continue;
        }

        /* Sanity-check the header before trusting the size: the product in
         * 64 bits (w = h = 65536 made w*h*3 wrap to 0 in 32 bits and a frame
         * announcing n = 0 pass, then img.copy() read 12 GB past the buffer:
         * October 2026 audit), and a frame no larger than any sensor's. */
        /* SF2Y: Y (w*h), then U and V ((w+1)/2 * (h+1)/2 each) */
        const quint64 want = isH ? 16u : isY ? quint64(w) * h + 2u * quint64((w + 1) / 2) * ((h + 1) / 2) : quint64(w) * h * 3u;
        if (w == 0 || h == 0 || w > 16384 || h > 16384 || quint64(n) != want) {
            ++pos;                 /* bad header: slip a byte and resync         */
            continue;
        }
        if (size - pos < SF2P_HEADER + int(n))
            break;                 /* the pixels have not all arrived yet       */

        /* a buffer frame passed over for a newer one goes back to the camera */
        if (newest >= 0 && newestHwb && m_hwframes) m_hwframes->release(newestHwbId);
        newest = pos + SF2P_HEADER; newestW = w; newestH = h;   /* the latest so far */
        newestYuv = isY;
        newestHwb = isH;
        if (isH) newestHwbId = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(base + pos + SF2P_HEADER));
        pos += SF2P_HEADER + int(n);
        gotFrame = true;
    }
    if (aidPos >= 0 && m_vfModeSent == QLatin1String("hwb")) {
        /* the aids' values of the zero-copy frame that follows them (kept
         * even when that frame comes with the next read) */
        m_aidMask = int(qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(base + aidPos + SF2P_HEADER)));
        m_aidMaps = m_buffer.mid(aidPos + SF2P_HEADER + 16, int(aidN) - 16);
        m_aidCells = QSize(int(aidW), int(aidH));
        m_aidsGot = true;
        m_aidsNew = true;
        ++m_aidSerial;
    }
    if (newest >= 0) {
        /* Copy the newest frame's pixels (the buffer's bytes are about to
         * go): an I420 frame as it is, for the shader (half the bytes of
         * RGB, and no conversion on this thread); an RGB888 one into a QImage. */
        if (newestHwb) {
            /* zero copy: the frame is the camera's buffer; its handle came
             * (or is waiting) on the socket. The one received before and
             * never drawn goes back. */
            if (m_hwframes) {
                if (!m_hwframes->known(newestHwbId)) m_hwframes->drain();
                if (m_hwbId && m_hwbId != m_hwbShown && m_hwbId != newestHwbId) m_hwframes->release(m_hwbId);
            }
            m_hwbId = newestHwbId;
            m_yuv.clear();
            m_frame = QImage();
        } else if (newestYuv) {
            m_yuv = m_buffer.mid(newest, int(quint64(newestW) * newestH + 2u * quint64((newestW + 1) / 2) * ((newestH + 1) / 2)));
            m_frame = QImage();
            m_hwbId = 0;
        } else {
            QImage img(reinterpret_cast<const uchar *>(base + newest), int(newestW), int(newestH), int(newestW * 3), QImage::Format_RGB888);
            m_frame = img.copy();
            m_yuv.clear();
            m_hwbId = 0;
        }
        syncVfMode();
        ++m_frameSerial;                                             /* the render thread uploads it once */
        if (m_hwprobe) {               /* hwprobe: asked once per engine, its lines logged */
            if (m_hwpFrames > 0 && !m_hwpAsked && m_process) {
                m_hwpAsked = true;
                sendCommand(QStringLiteral("hwprobe %1 %2").arg(m_hwprobe->path()).arg(m_hwpFrames));
            }
            for (const QString &r : m_hwprobe->takeResults()) sendCommand(QStringLiteral("# hwprobe app: ") + r);
        }
        if (m_frameSize != QSize(int(newestW), int(newestH))) {     /* the page sizes the viewfinder from it */
            m_frameSize = QSize(int(newestW), int(newestH));
            emit frameSizeChanged();
        }
    }
    if (pos >= size) m_buffer.clear();
    else if (pos > 0) m_buffer.remove(0, pos);
    /* the overlay again: with a new frame -- zero copy: with new maps
     * (one frame in aidsEvery; they come after their frame) */
    if (m_hwbId ? m_aidsNew : gotFrame) buildOverlay();
    m_aidsNew = false;

    if (gotFrame) {
        setRunning(true);          /* first frame proves the stream is live     */
        update();                  /* schedule a repaint with the new frame     */
    }
}

/* --- SF2M events --- */

/* Splits "key=value key=value ..." into a map (first word = the event name). */
static QHash<QByteArray, QByteArray> kv(const QByteArray &line, QByteArray *name)
{
    QHash<QByteArray, QByteArray> out;
    const QList<QByteArray> words = line.trimmed().split(' ');
    if (!words.isEmpty()) *name = words.first();
    for (int i = 1; i < words.size(); ++i) {
        const int eq = words.at(i).indexOf('=');
        if (eq > 0) out.insert(words.at(i).left(eq), words.at(i).mid(eq + 1));
    }
    return out;
}

/* Parses "a,b,c,..." into a list of ints. */
static QVariantList intList(const QByteArray &csv)
{
    QVariantList l;
    for (const QByteArray &v : csv.split(',')) l.append(v.toInt());
    return l;
}

/* The space left for the photos: the smaller of the two folders' volumes
 * (they may be on the phone and on a memory card). */
void IndrawPreview::updateFreeSpace()
{
    qint64 least = -1;
    for (const QString &dir : { m_jpgDir, m_dngDir }) {
        if (dir.isEmpty()) continue;
        QStorageInfo si(dir);
        if (!si.isValid() || !si.isReady()) continue;
        const qint64 b = si.bytesAvailable();
        if (b >= 0 && (least < 0 || b < least)) least = b;
    }
    const int mb = least < 0 ? -1 : int(least / (1024 * 1024));
    if (mb != m_freeMb) { m_freeMb = mb; emit freeSpaceChanged(); }
}

void IndrawPreview::parseEvent(const QByteArray &line)
{
    QByteArray name;
    const QHash<QByteArray, QByteArray> f = kv(line, &name);
    if (name == "captured") {
        emit captureFinished();
        if (m_shotPending) setProcessing(true);   /* the dots start with the flash */
    } else if (name == "fcrop") {
        /* The crop the next frames were taken with (sent right before them). */
        const QRectF c(f.value("x").toDouble(), f.value("y").toDouble(),
                       f.value("w").toDouble(), f.value("h").toDouble());
        if (c.width() > 0 && c.height() > 0 && c != m_frameCrop) { m_frameCrop = c; emit viewWindowChanged(); }
    } else if (name == "wbpick") {
        emit wbPicked(f.value("ok").toInt() != 0, QString::fromLatin1(f.value("gains")), QString::fromLatin1(f.value("why")),
                      QString::fromLatin1(f.value("awb")));
    } else if (name == "cal_progress") {
        emit calProgress(f.value("done").toInt(), f.value("total").toInt());
    } else if (name == "cal_exposure") {
        emit calExposure(f.value("exp_ns").toLongLong(), f.value("iso").toInt());
    } else if (name == "cal_done") {
        QString why = QString::fromLatin1(f.value("why"));
        why.replace(QLatin1Char('_'), QLatin1Char(' '));   /* the engine wrote '_' for spaces */
        emit calDone(f.value("ok").toInt() != 0, QString::fromLatin1(f.value("path")), why);
    } else if (name == "stack_start") {
        /* The engine says a stack began (the controller issued it): the
         * overlay, the lock-out and the screen kept on start here. */
        if (!m_stacking) { m_stacking = true; emit stackingChanged(); }
        emit stackStarted(f.value("n").toInt());
    } else if (name == "progress") {
        emit stackProgress(f.value("frame").toInt(), f.value("n").toInt(), f.value("elapsed_ms").toInt());
    } else if (name == "stack_done") {
        emit stackDone(f.value("frames").toInt(), f.value("n").toInt(), f.value("aborted").toInt() != 0, f.value("ms").toInt());
        if (m_stacking) { m_stacking = false; emit stackingChanged(); }
        /* The app went inactive while the stack ran: stop now, as promised. */
        if (m_stopWanted) { m_stopWanted = false; if (!m_active) stop(); }
    } else if (name == "written") {
        const QString kind = QString::fromUtf8(f.value("kind"));
        const QString path = QString::fromUtf8(f.value("path"));
        if (kind == QLatin1String("preview"))   /* the engine's DNG render: make the jpg */
            convertDngPreview(path, f.value("orientation").toInt());
        emit fileWritten(kind, path);
        if (m_shotPending && kind != QLatin1String("preview")) m_procTimer->start(600);
        if (kind != QLatin1String("preview")) updateFreeSpace();
    } else if (name == "shotfailed") {
        /* the photo is lost: said on the screen, and the busy dots stop */
        QString why = QString::fromUtf8(f.value("why"));
        why.replace(QLatin1Char('_'), QLatin1Char(' '));
        updateFreeSpace();
        emit shotFailed(why, f.value("full").toInt() != 0);
        if (m_shotPending) m_procTimer->start(600);
    } else if (name == "caps") {
        updateFreeSpace();
        /* The engine gives n/d for the EV step; the UI wants EV per step. */
        const QList<QByteArray> step = f.value("ev_step", "0/1").split('/');
        const double num = step.value(0).toDouble();
        const double den = step.value(1, "1").toDouble();
        m_caps.clear();
        m_caps.insert(QStringLiteral("isoMin"), f.value("iso_min").toInt());
        m_caps.insert(QStringLiteral("isoMax"), f.value("iso_max").toInt());
        m_caps.insert(QStringLiteral("isoAnalogMax"), f.value("iso_analog_max").toInt());
        m_caps.insert(QStringLiteral("expMinNs"), f.value("exp_min_ns").toDouble());
        m_caps.insert(QStringLiteral("expMaxNs"), f.value("exp_max_ns").toDouble());
        m_caps.insert(QStringLiteral("shutterMaxNs"), f.value("shutter_max_ns").toDouble());
        m_caps.insert(QStringLiteral("evMin"), f.value("ev_min").toInt());
        m_caps.insert(QStringLiteral("evMax"), f.value("ev_max").toInt());
        m_caps.insert(QStringLiteral("evStep"), den > 0 ? num / den : 0.0);
        m_caps.insert(QStringLiteral("orientation"), f.value("orientation").toInt());
        m_caps.insert(QStringLiteral("black"), f.value("black").toInt());
        m_caps.insert(QStringLiteral("white"), f.value("white").toInt());
        m_caps.insert(QStringLiteral("zoomMax"), f.value("zoom_max", "1").toDouble());
        m_caps.insert(QStringLiteral("focalMm"), f.value("focal_mm", "0").toDouble());
        m_caps.insert(QStringLiteral("focalEq"), f.value("focal_eq", "0").toDouble());
        QVariantList cams;
        for (const QByteArray &id : f.value("cameras").split(','))
            if (!id.isEmpty()) cams.append(QString::fromLatin1(id));
        m_caps.insert(QStringLiteral("cameras"), cams);
        QVariantList facing;                       /* each camera's side, in the order of "cameras" */
        for (const QByteArray &fc : f.value("facing").split(','))
            if (!fc.isEmpty()) facing.append(QString::fromLatin1(fc));
        m_caps.insert(QStringLiteral("facing"), facing);
        /* the YUV stream our JPG is taken from, "WxH/k" (k: the averaging), for the log */
        m_caps.insert(QStringLiteral("yuvStream"), QString::fromLatin1(f.value("yuv_stream")));
        QVariantList rawCams;                      /* each camera gives RAW (1) or not (0), in the order of "cameras" (0.3.8) */
        for (const QByteArray &rc : f.value("raw_cams").split(','))
            if (!rc.isEmpty()) rawCams.append(rc.toInt());
        m_caps.insert(QStringLiteral("rawCams"), rawCams);
        m_caps.insert(QStringLiteral("focusCal"), f.value("focus_cal", "0").toInt());   /* calibration points in force */
        /* our JPG's processing by default on this camera (device profile, else
         * the app's): "method,frames,tile_frames,tile_split,colour,moire,luma,
         * sharpen", the menu's defaults (SettingsMenu jpgDefault) */
        m_caps.insert(QStringLiteral("jpgDefault"), QString::fromLatin1(f.value("jpg_default")));
        m_caps.insert(QStringLiteral("touchFocus"), f.value("touch_focus", "1").toInt());   /* 0: no focus by touch (device profile) */
        {   /* the camera's RAW capability, independent of this session's streams */
            const QList<QByteArray> rc = f.value("raw_cap", "0x0").split('x');
            m_caps.insert(QStringLiteral("rawCapW"), rc.size() == 2 ? rc.at(0).toInt() : 0);
        }
        QVariantList sizes;
        for (const QByteArray &sz : f.value("jpeg_sizes").split(','))
            if (!sz.isEmpty()) sizes.append(QString::fromLatin1(sz));
        m_caps.insert(QStringLiteral("jpegSizes"), sizes);
        QVariantList awb;
        for (const QByteArray &m : f.value("awb_modes").split(','))
            if (!m.isEmpty()) awb.append(m.toInt());
        m_caps.insert(QStringLiteral("awbModes"), awb);
        QVariantList scenes;
        for (const QByteArray &m : f.value("scene_modes").split(','))
            if (!m.isEmpty()) scenes.append(m.toInt());
        m_caps.insert(QStringLiteral("sceneModes"), scenes);
        m_caps.insert(QStringLiteral("limits"), f.value("limits", ""));   /* "raw_off", "jpeg_max", both, or "" */
        m_caps.insert(QStringLiteral("rawW"), f.value("raw").split(',').value(0).toInt());
        m_caps.insert(QStringLiteral("rawH"), f.value("raw").split(',').value(1).toInt());
        /* the JPEG size this session runs at (what 'max' resolved to) */
        m_caps.insert(QStringLiteral("jpegW"), f.value("jpeg").split(',').value(0).toInt());
        m_caps.insert(QStringLiteral("jpegH"), f.value("jpeg").split(',').value(1).toInt());
        m_caps.insert(QStringLiteral("peakEdge"), f.value("peak_edge", "40").toInt());
        m_caps.insert(QStringLiteral("zebraJpeg"), f.value("zebra_jpeg", "250").toInt());
        m_caps.insert(QStringLiteral("zebraRaw"), f.value("zebra_raw", "992").toInt());
        m_caps.insert(QStringLiteral("peakWidth"), f.value("peak_width", "2").toInt());
        m_caps.insert(QStringLiteral("flash"), f.value("flash", "0").toInt() != 0);
        m_caps.insert(QStringLiteral("cropFree"), f.value("crop_free", "0").toInt() != 0);
        m_caps.insert(QStringLiteral("aperture"), f.value("aperture", "0").toDouble());
        m_caps.insert(QStringLiteral("min_focus_diopters"), f.value("min_focus", "0").toDouble());  /* the closest lens position, HAL value */
        /* the screen's top cutout from the device profile: 1, 0, -1 unknown */
        m_caps.insert(QStringLiteral("notch"), f.value("notch", "-1").toInt());
        m_gotCaps = true;
        if (!s_archiveNote.isEmpty()) {             /* the first start of a new version: say what was set aside */
            sendCommand(QStringLiteral("# ") + s_archiveNote);
            s_archiveNote.clear();
        }
        if (!s_confNote.isEmpty()) {                /* what was done to rawfish-ir.conf at this start */
            sendCommand(QStringLiteral("# ") + s_confNote);
            s_confNote.clear();
        }
        /* where the photos go when the memory card asked for is not there */
        if (!m_storageNote.isEmpty()) sendCommand(QStringLiteral("# ") + m_storageNote);
        m_sizeFallback = false;
        m_yuvFails = 0;
        /* The streams or the JPEG size asked changed while this engine was
         * starting (the type changed within its first second): it holds the
         * wrong ones, and a capture would fail ("session has no raw16
         * stream") or come at the wrong size. Start again with the right ones. */
        if (sessionOutdated()) restartIfOutdated();
        emit capsChanged();
        if (m_caps.value(QStringLiteral("limits")).toString().contains(QLatin1String("exclusive"))) {
            if (m_rawFellBack) {
                m_rawFellBack = false;         /* the page switches to JPEG: rawSession follows */
                emit rawSessionFailed();
            } else if (m_sessionRaw != m_rawSession) {
                restart();                     /* the type was set before the caps said "exclusive" */
            }
        }
    } else if (name == "ae") {
        m_liveIso = f.value("iso").toInt();
        m_wbCct = f.value("cct").toInt();
        m_wbDuv = f.value("duv").toDouble();
        m_liveExposureNs = f.value("exp_ns").toDouble();
        emit liveChanged();
    } else if (name == "xpstate") {
        QVariantMap st;
        st.insert(QStringLiteral("iso"), f.value("iso").toInt());
        st.insert(QStringLiteral("expNs"), f.value("exp_ns").toDouble());
        st.insert(QStringLiteral("isoManual"), f.value("iso_manual").toInt() != 0);
        st.insert(QStringLiteral("shutterManual"), f.value("shutter_manual").toInt() != 0);
        st.insert(QStringLiteral("fullAuto"), f.value("full_auto").toInt() != 0);
        st.insert(QStringLiteral("fullManual"), f.value("full_manual").toInt() != 0);
        st.insert(QStringLiteral("fixed"), f.value("fixed").toInt() != 0);
        st.insert(QStringLiteral("spot"), f.value("spot").toInt() != 0);
        st.insert(QStringLiteral("ev"), f.value("ev").toInt());
        st.insert(QStringLiteral("offset"), f.value("offset").toDouble());
        st.insert(QStringLiteral("offsetValid"), f.value("offset_valid").toInt() != 0);
        st.insert(QStringLiteral("pvMs"), f.value("pv_ms").toDouble());
        st.insert(QStringLiteral("pvSlow"), f.value("pv_slow").toInt() != 0);
        st.insert(QStringLiteral("deficit"), f.value("deficit").toDouble());
        st.insert(QStringLiteral("tooDark"), f.value("too_dark").toInt() != 0);
        st.insert(QStringLiteral("stops"), f.value("stops").toDouble());           /* from a neutral grey (0.3.8) */
        st.insert(QStringLiteral("stopsValid"), f.value("stops_valid").toInt() != 0);
        st.insert(QStringLiteral("stopsClip"), f.value("stops_clip").toInt());
        const QByteArray ev100 = f.value("ev100");
        st.insert(QStringLiteral("ev100"), ev100 == "nan" || ev100.isEmpty() ? qQNaN() : ev100.toDouble());
        m_exposureState = st;
        emit exposureStateChanged();
    } else if (name == "vwstate") {
        QVariantMap st;
        const QList<QByteArray> vw = f.value("view").split(',');
        const QList<QByteArray> rf = f.value("ref").split(',');
        st.insert(QStringLiteral("zoom"), f.value("zoom").toDouble());
        st.insert(QStringLiteral("loupe"), f.value("loupe").toDouble());
        if (vw.size() == 4) {
            const QRectF w(vw.at(0).toDouble(), vw.at(1).toDouble(), vw.at(2).toDouble(), vw.at(3).toDouble());
            setViewWindow(w);                  /* what to show: the item applies it directly */
        }
        st.insert(QStringLiteral("hasRef"), rf.size() == 3 && rf.at(0).toInt() != 0);
        st.insert(QStringLiteral("refU"), rf.size() == 3 ? rf.at(1).toDouble() : 0.5);
        st.insert(QStringLiteral("refV"), rf.size() == 3 ? rf.at(2).toDouble() : 0.5);
        const QList<QByteArray> lf = f.value("light").split(',');      /* the light point (sun) */
        st.insert(QStringLiteral("hasLight"), lf.size() == 3 && lf.at(0).toInt() != 0);
        st.insert(QStringLiteral("lightU"), lf.size() == 3 ? lf.at(1).toDouble() : 0.5);
        st.insert(QStringLiteral("lightV"), lf.size() == 3 ? lf.at(2).toDouble() : 0.5);
        m_viewState = st;
        emit viewStateChanged();
    } else if (name == "fcstate") {
        QVariantMap st;
        st.insert(QStringLiteral("mode"), f.value("mode").toInt());
        st.insert(QStringLiteral("diopters"), f.value("d").toDouble());
        st.insert(QStringLiteral("circle"), f.value("circle").toInt());
        st.insert(QStringLiteral("searching"), f.value("searching").toInt() != 0);
        st.insert(QStringLiteral("light"), f.value("light").toInt() != 0);
        m_focusState = st;
        emit focusStateChanged();
    } else if (name == "vfmeter") {
        /* The viewfinder's measure, made by the engine on the frame it sent
         * (step 2 of docs/ARCHITECTURE.md): mean, and the histogram in jpg. */
        m_vfMean = f.value("mean").toDouble();
        emit vfMeterChanged();
        if (m_histogramSource == QLatin1String("vf")) takeHistogram(f);
    } else if (name == "meter") {
        /* zclip: the share (%) of what is shown at the RAW zebra level, for
         * the RAW zebra (zebraRawShare); an engine without it leaves -1 */
        /* zlost: the part of it with two channels or more (striped fully).
         * Both eased over the measures (5 a second): eight ninths of the way
         * to each new one, an exposure change followed within about a tenth
         * of a second of its measure -- each measure moved the stripes at
         * once, in steps. */
        auto ease = [](qreal kept, qreal now) { return now < 0 || kept < 0 ? now : kept + (now - kept) * 8 / 9; };
        m_rawClipShare = ease(m_rawClipShare, f.contains("zclip") ? qBound<qreal>(0, f.value("zclip").toDouble() / 100.0, 1) : -1);
        m_rawLostShare = ease(m_rawLostShare, f.contains("zlost") ? qBound<qreal>(0, f.value("zlost").toDouble() / 100.0, 1) : -1);
        m_meterMean = f.value("mean").toDouble();
        m_meterIso = f.value("iso").toInt();
        m_meterExposureNs = f.value("exp_ns").toDouble();
        emit meterChanged();
        if (m_histogramSource == QLatin1String("raw")) takeHistogram(f);
    }
}

/* Zebra + peaking overlay, full resolution.
 * Zebra: diagonal black/white stripes where the luminance (zebraLuma) or
 * any channel is at or above zebraLevel -- or, for the RAW zebra (share),
 * on the brightest pixels of the shown region, as many as the RAW clips:
 * fully where it lost two channels or three, lightly (white stripes only,
 * half opaque) where it lost one.
 * Peaking: focus is high-frequency content, so the criterion is the 3x3
 * Laplacian (a high-pass) of the luma, not the gradient: a soft, blurred
 * edge has a large gradient spread over many pixels but a small second
 * derivative, a sharp one a large second derivative on one or two pixels.
 * The luma is box-smoothed 2x2 first so single-pixel sensor noise does not
 * pass. A pixel is marked (1-pixel red) where |Laplacian| exceeds the device
 * threshold and is the local maximum of its row and column neighbours, which
 * keeps the lines thin. Rebuilt per frame while an aid is on (~1.2 Mpx). */
/* The pure computation: a frame and the aids' settings in, the ARGB overlay
 * out (null when nothing to draw). No static state: runs on the worker. */
/* The frame as the aids read it, pixel by pixel: `peak` the value the
 * peaking smooths (the green), `luma` the Rec.601 luma, `maxCh` the
 * brightest of R, G, B. An I420 frame is read where it lies (0.3.11): the
 * luma is its Y plane as it is (the camera's Y is the Rec.601 luma the
 * zebra computed from RGB); the green and the brightest channel are worked
 * out for the pixel, with the engine's arithmetic, only when the peaking or
 * the zebra on channels asks -- nothing converts the whole frame to RGB.
 * The peaking keeps the green: on Y its marks fell on other pixels (the
 * same density, 43 % of them moved on a test photo), and its threshold was
 * tuned on the green. */
struct OverlayRgbSrc
{
    const uchar *b; int bpl, w, h;
    explicit OverlayRgbSrc(const QImage &f) : b(f.constBits()), bpl(f.bytesPerLine()), w(f.width()), h(f.height()) {}
    int peak(int x, int y) const { return b[y * bpl + 3 * x + 1]; }
    int luma(int x, int y) const { const uchar *p = b + y * bpl + 3 * x; return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8; }
    int maxCh(int x, int y) const { const uchar *p = b + y * bpl + 3 * x; return qMax(p[0], qMax(p[1], p[2])); }
};
struct OverlayI420Src
{
    const uchar *py, *pu, *pv; int cw, w, h;
    OverlayI420Src(const QByteArray &i420, const QSize &size)
        : py(reinterpret_cast<const uchar *>(i420.constData())), cw((size.width() + 1) / 2), w(size.width()), h(size.height())
    {
        pu = py + w * h;
        pv = pu + cw * ((h + 1) / 2);
    }
    int peak(int x, int y) const
    {
        const int ci = (y / 2) * cw + x / 2;
        return qBound(0, py[y * w + x] - ((22554 * (pu[ci] - 128) + 46802 * (pv[ci] - 128)) >> 16), 255);
    }
    int luma(int x, int y) const { return py[y * w + x]; }
    int maxCh(int x, int y) const
    {
        /* android/yuv.h's arithmetic: the pixel's R, G, B, the brightest */
        const int Y = py[y * w + x], ci = (y / 2) * cw + x / 2, U = pu[ci] - 128, V = pv[ci] - 128;
        const int r = Y + ((91881 * V) >> 16), g = Y - ((22554 * U + 46802 * V) >> 16), b = Y + ((116130 * U) >> 16);
        return qBound(0, qMax(r, qMax(g, b)), 255);
    }
};

/* What the overlay is drawn from, whatever the frame's form: the size
 * (frame pixels, or the engine's 2x2 cells), the peaking's |Laplacian|
 * (null: no peaking) and the value it must exceed, the border it skips,
 * the RAW zebra's smoothed luma and the step its histogram samples it at,
 * the zebra's stripe period (log2) and the peaking line's extra width;
 * `pix` the luma and the brightest channel for the fixed zebra. */
struct OverlayBase {
    int w = 0, h = 0;
    const short *lap = nullptr; int lapAbove = 0, border = 2;
    const uchar *zl = nullptr; int histStep = 2;
    int stripeShift = 3, extra = 0;
};
template <class Pix>
static QImage overlayCore(const OverlayBase &ob, const Pix &src, const OverlayParams &prm, OverlayState *st)
{
    const int w = ob.w, h = ob.h;
    QImage overlay(w, h, QImage::Format_ARGB32_Premultiplied);
    const QRgb zebraA = qRgba(255, 255, 255, 220), zebraB = qRgba(0, 0, 0, 220);
    const QRgb zebraLight = qRgba(128, 128, 128, 128);    /* premultiplied: white at half opacity */
    const QRgb peak = qRgba(255, 40, 40, 255);
    const bool peaking = prm.peaking && ob.lap;
    const short *lapAll = ob.lap;
    const int edge = ob.lapAbove, bd = ob.border, sh = ob.stripeShift;
    const bool shareMode = prm.zebra && prm.share >= 0 && ob.zl;
    const uchar *zl = ob.zl;
    const int RAW_HYST = 4;
    int level = prm.level, lostLevel = -1;            /* -1: every stripe full */
    bool zLuma = prm.zebraLuma;
    if (st && (!shareMode || st->size != QSize(w, h))) {   /* a fresh start */
        st->level = st->lostLevel = -1;
        st->mask.clear();
        st->size = QSize(w, h);
    }
    if (shareMode) {
        const QRect r = prm.region.isValid() ? prm.region.intersected(QRect(0, 0, w, h)) : QRect(0, 0, w, h);
        long cnt[256] = { 0 }, total = 0;
        zLuma = true;
        for (int y = r.top(); y <= r.bottom(); y += ob.histStep) {
            const uchar *lr = zl + y * w;
            for (int x = r.left(); x <= r.right(); x += ob.histStep) { cnt[lr[x]]++; total++; }
        }
        /* the luminance above which lie `share` of the pixels (256: none) */
        auto levelFor = [&](double share) {
            if (total <= 0 || share < 0.0001) return 256;
            const double target = share * total;
            long cum = 0;
            for (int v = 255; v >= 0; --v) { cum += cnt[v]; if (cum >= target) return v; }
            return 0;
        };
        level = levelFor(prm.share);
        if (prm.lostShare >= 0) lostLevel = levelFor(qMin(prm.lostShare, prm.share));
        if (st) {
            /* eased: three quarters of the way to the new level each frame
             * (a few frames at 20-30 a second); "none" (256) at once */
            auto ease = [](double &kept, int now) {
                if (now > 255 || now < 0 || kept < 0) kept = now;
                else kept += 0.75 * (now - kept);
                return now > 255 || now < 0 ? now : int(kept + 0.5);
            };
            level = ease(st->level, level);
            if (lostLevel >= 0) lostLevel = ease(st->lostLevel, lostLevel); else st->lostLevel = -1;
            if (st->mask.size() != w * h) st->mask = QVector<uchar>(w * h, 0);
        }
    }
    uchar *zmask = shareMode && st ? st->mask.data() : nullptr;
    for (int y = 0; y < h; ++y) {
        QRgb *out = reinterpret_cast<QRgb *>(overlay.scanLine(y));
        const short *l = peaking ? lapAll + y * w : nullptr;
        const short *lu = peaking ? lapAll + (y > 0 ? y - 1 : 0) * w : nullptr;
        const short *ld = peaking ? lapAll + (y < h - 1 ? y + 1 : y) * w : nullptr;
        for (int x = 0; x < w; ++x) {
            QRgb v = 0;
            if (peaking && x >= bd && x < w - bd && y >= bd && y < h - bd) {
                const int a = l[x];
                if (a > edge && a >= l[x - 1] && a >= l[x + 1] && a >= lu[x] && a >= ld[x]) v = peak;
            }
            if (shareMode) {
                /* the RAW zebra: 0 none, 1 light, 2 full -- a pixel keeps
                 * its stripes down to RAW_HYST below the level */
                const int lv = zl[y * w + x], was = zmask ? zmask[y * w + x] : 0;
                int z = 0;
                if (lv >= level || (was && level <= 255 && lv >= level - RAW_HYST)) {
                    z = 1;
                    if (lostLevel < 0 || lv >= lostLevel || (was == 2 && lv >= lostLevel - RAW_HYST)) z = 2;
                }
                if (zmask) zmask[y * w + x] = uchar(z);
                const bool stripe = ((x + y) >> sh) & 1;
                if (z == 2) v = stripe ? zebraA : zebraB;               /* lost: full stripes */
                else if (z == 1 && stripe && v == 0) v = zebraLight;   /* one channel: light, peaking kept */
            } else if (prm.zebra) {
                const bool hit = zLuma ? src.luma(x, y) >= level     /* Rec.601 luma */
                                       : src.maxCh(x, y) >= level;   /* any channel */
                if (hit) v = (((x + y) >> sh) & 1) ? zebraA : zebraB;
            }
            out[x] = v;
        }
    }
    /* Line width: thicken the 1-pixel peaking marks by (width - 1) pixels to
     * the right and below (a cheap dilation; width from the device override). */
    if (peaking && ob.extra > 0) {
        const int extra = ob.extra;
        for (int y = h - 1; y >= 0; --y) {
            QRgb *row = reinterpret_cast<QRgb *>(overlay.scanLine(y));
            for (int x = w - 1; x >= 0; --x) {
                if (row[x] != peak) continue;
                for (int k = 1; k <= extra; ++k) {
                    if (x + k < w && row[x + k] == 0) row[x + k] = peak;
                    if (y + k < h) { QRgb *r2 = reinterpret_cast<QRgb *>(overlay.scanLine(y + k)); if (r2[x] == 0) r2[x] = peak; }
                }
            }
        }
    }
    return overlay;
}


template <class Src>
static QImage overlayFrom(const Src &src, const OverlayParams &prm, OverlayState *st)
{
    const int w = src.w, h = src.h;
    if (w < 4 || h < 4) return QImage();

    /* Smoothed luma (2x2 box of the green channel), one byte per pixel:
     * only the peaking reads it. */
    QVector<uchar> luma;
    QVector<short> lap;
    if (prm.peaking) {
        QVector<uchar> green(w * h);
        for (int y = 0; y < h; ++y) {
            uchar *o = green.data() + y * w;
            for (int x = 0; x < w; ++x) o[x] = uchar(src.peak(x, y));
        }
        luma.resize(w * h);
        lap = QVector<short>(w * h, 0);
        for (int y = 0; y < h; ++y) {
            const uchar *r0 = green.constData() + y * w, *r1 = green.constData() + (y < h - 1 ? y + 1 : y) * w;
            uchar *out = luma.data() + y * w;
            for (int x = 0; x < w; ++x) {
                const int x1 = x < w - 1 ? x + 1 : x;
                out[x] = uchar((r0[x] + r0[x1] + r1[x] + r1[x1]) >> 2);
            }
        }
    }
    /* |Laplacian| = |8c - sum of the 8 neighbours| on the smoothed luma. */
    if (prm.peaking) {
        for (int y = 1; y < h - 1; ++y) {
            const uchar *u = luma.constData() + (y - 1) * w, *c = u + w, *d = c + w;
            short *o = lap.data() + y * w;
            for (int x = 1; x < w - 1; ++x) {
                const int v = 8 * c[x] - u[x - 1] - u[x] - u[x + 1] - c[x - 1] - c[x + 1] - d[x - 1] - d[x] - d[x + 1];
                o[x] = short(v < 0 ? -v : v);
            }
        }
    }
    /* The RAW zebra: the level is the one above which lie `share` of the
     * shown region's pixels (by luminance), so that as much of the
     * viewfinder is striped as the RAW has clipped -- in its brightest
     * part. Pixels tied at the level are all striped (a viewfinder white
     * at 255 cannot be told apart). A share under 1/10000: nothing. The
     * level of lostShare the same way: from it up, the stripes are full. */
    /* The RAW zebra reads a smoothed luminance: the Rec.601 luma in a 2x2
     * box, so that a pixel's own noise no longer crosses the level frame
     * after frame. (Not the peaking's green alone: the green, the
     * strongest channel, reaches 255 over wide areas, the two levels then
     * met there and the light stripes vanished.) Its levels are eased from
     * frame to frame, and a pixel striped stays so down to RAW_HYST levels
     * below (OverlayState). */
    const bool shareMode = prm.zebra && prm.share >= 0;
    QVector<uchar> zl;
    if (shareMode) {
        QVector<uchar> y601(w * h);
        for (int y = 0; y < h; ++y) {
            uchar *o = y601.data() + y * w;
            for (int x = 0; x < w; ++x) o[x] = uchar(src.luma(x, y));
        }
        zl.resize(w * h);
        for (int y = 0; y < h; ++y) {
            const uchar *r0 = y601.constData() + y * w, *r1 = y601.constData() + (y < h - 1 ? y + 1 : y) * w;
            uchar *o = zl.data() + y * w;
            for (int x = 0; x < w; ++x) {
                const int x1 = x < w - 1 ? x + 1 : x;
                o[x] = uchar((r0[x] + r0[x1] + r1[x] + r1[x1] + 2) >> 2);
            }
        }
    }
    OverlayBase ob;
    ob.w = w; ob.h = h;
    ob.lap = prm.peaking ? lap.constData() : nullptr;
    ob.lapAbove = prm.edge * 3;                          /* Laplacian scale: 8x luma span */
    ob.border = 2;
    ob.zl = shareMode ? zl.constData() : nullptr;
    ob.histStep = 2;
    ob.stripeShift = 3;
    ob.extra = prm.width > 1 ? qMin(prm.width, 6) - 1 : 0;
    return overlayCore(ob, src, prm, st);
}

QImage computeOverlay(const QImage &frame, const OverlayParams &prm, OverlayState *st)
{
    if ((!prm.zebra && !prm.peaking) || frame.isNull() || frame.format() != QImage::Format_RGB888) return QImage();
    return overlayFrom(OverlayRgbSrc(frame), prm, st);
}

QImage computeOverlayI420(const QByteArray &i420, const QSize &size, const OverlayParams &prm, OverlayState *st)
{
    const int w = size.width(), h = size.height();
    if ((!prm.zebra && !prm.peaking) || w <= 0 || h <= 0 ||
        quint64(i420.size()) != quint64(w) * h + 2u * quint64((w + 1) / 2) * ((h + 1) / 2))
        return QImage();
    return overlayFrom(OverlayI420Src(i420, size), prm, st);
}

/* The engine's aids maps (SF2A, zero copy): one value per 2x2 cell of the
 * frame, worked out on the full frame with this file's arithmetic and the
 * strongest of the cell kept (android/rawfish-ir.c, aid_maps). The same
 * decisions on the cells: the peaking's coded |Laplacian| q >= edge exactly
 * where a pixel's a > 3 * edge, a cell striped where one of its pixels is
 * at the level, the RAW zebra's histogram on the same 2x2 means. The
 * overlay is the cells' size (drawn over the frame at twice its scale);
 * stripes and lines keep their size on screen. */
struct OverlayCellPix
{
    const uchar *ymax, *cmax; int w;
    int luma(int x, int y) const { return ymax ? ymax[y * w + x] : 0; }
    int maxCh(int x, int y) const { return cmax ? cmax[y * w + x] : 0; }
};
QImage computeOverlayCells(const QByteArray &maps, const QSize &cells, int mask, const OverlayParams &prmIn, OverlayState *st)
{
    const int w = cells.width(), h = cells.height();
    const quint64 n = quint64(qMax(0, w)) * quint64(qMax(0, h));
    int k = 0;
    for (int b = 1; b <= 8; b <<= 1) if (mask & b) ++k;
    if ((!prmIn.zebra && !prmIn.peaking) || w < 4 || h < 4 || quint64(maps.size()) != quint64(k) * n) return QImage();
    const uchar *m = reinterpret_cast<const uchar *>(maps.constData());
    const uchar *lapq = nullptr, *ymax = nullptr, *cmax = nullptr, *lum = nullptr;
    if (mask & 1) { lapq = m; m += n; }
    if (mask & 2) { ymax = m; m += n; }
    if (mask & 4) { cmax = m; m += n; }
    if (mask & 8) { lum = m; m += n; }
    OverlayParams prm = prmIn;
    if (prm.region.isValid())                            /* frame pixels -> cells */
        prm.region = QRect(prm.region.x() / 2, prm.region.y() / 2, (prm.region.width() + 1) / 2, (prm.region.height() + 1) / 2);
    /* the zebra this map set can draw (a map not sent yet: none) */
    if (prm.zebra && prm.share < 0 && !(prm.zebraLuma ? ymax : cmax)) prm.zebra = false;
    QVector<short> lap;
    if (prm.peaking && lapq) {
        lap.resize(int(n));
        for (quint64 i = 0; i < n; ++i) lap[int(i)] = lapq[i];
    }
    OverlayBase ob;
    ob.w = w; ob.h = h;
    ob.lap = lap.isEmpty() ? nullptr : lap.constData();
    ob.lapAbove = prm.edge - 1;                          /* q > edge - 1: q >= edge */
    ob.border = 1;
    ob.zl = prm.zebra && prm.share >= 0 ? lum : nullptr;
    ob.histStep = 1;
    ob.stripeShift = 2;
    ob.extra = qMax(1, (qMin(prm.width, 6) + 1) / 2) - 1;
    if (prm.zebra && prm.share >= 0 && !lum) prm.zebra = false;
    return overlayCore(ob, OverlayCellPix{ ymax, cmax, w }, prm, st);
}

void OverlayWorker::computeCells(const QByteArray &maps, const QSize &cells, int mask, const OverlayParams &p)
{
    emit done(computeOverlayCells(maps, cells, mask, p, &m_state));
}

void OverlayWorker::compute(const QImage &frame, const QByteArray &i420, const QSize &size, const OverlayParams &p)
{
    /* An I420 frame read where it lies (no RGB conversion), an RGB one as before. */
    emit done(frame.isNull() ? computeOverlayI420(i420, size, p, &m_state) : computeOverlay(frame, p, &m_state));
}

QImage i420ToRgb(const QByteArray &i420, const QSize &size)
{
    const int w = size.width(), h = size.height(), cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (w <= 0 || h <= 0 || quint64(i420.size()) != quint64(w) * h + 2u * quint64(cw) * ch) return QImage();
    QImage rgb(w, h, QImage::Format_RGB888);
    const uchar *py = reinterpret_cast<const uchar *>(i420.constData());
    const uchar *pu = py + w * h, *pv = pu + cw * ch;
    auto clip8 = [](int v) { return uchar(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    for (int y = 0; y < h; ++y) {
        uchar *o = rgb.scanLine(y);
        const uchar *yr = py + y * w, *ur = pu + (y / 2) * cw, *vr = pv + (y / 2) * cw;
        for (int x = 0; x < w; ++x, o += 3) {
            /* android/yuv.h's yuv420_to_rgb, the same fixed point */
            const int Y = yr[x], U = ur[x / 2] - 128, V = vr[x / 2] - 128;
            o[0] = clip8(Y + ((91881 * V) >> 16));
            o[1] = clip8(Y - ((22554 * U + 46802 * V) >> 16));
            o[2] = clip8(Y + ((116130 * U) >> 16));
        }
    }
    return rgb;
}

/* The frames' format wanted now, asked of the engine when it changes:
 * zero copy (SF2H) when it works and no aid needs the pixels (the zebra and
 * the peaking read the YUV pipe's frames), else the YUV pipe (SF2Y), else
 * RGB (SF2P) -- what an engine starts with, and what an older one keeps
 * sending whatever it is asked. A failed import (HwFrames::failed) falls
 * back to the YUV pipe for this engine. */
/* The settings that apply without a new engine, read when an engine starts
 * and again each time rawfish-ir.conf is saved (to compare two of them from
 * one run: the log says when each took effect). */
void IndrawPreview::readLiveConf(bool announce)
{
    const QString path = settingsPath();
    if (!m_confWatch) {
        m_confWatch = new QFileSystemWatcher(this);
        /* Read 300 ms after the last change: an editor saves in steps (kate
         * over sshfs: the file emptied, then written -- an empty file read
         * in between gave the defaults, viewfinderZeroCopy=1 included, for
         * 11 ms; log of 15 Nov, 12:06:38). An editor saving by renaming
         * drops the watch: the folder is watched too. */
        m_confDebounce = new QTimer(this);
        m_confDebounce->setSingleShot(true);
        m_confDebounce->setInterval(300);
        connect(m_confDebounce, &QTimer::timeout, this, [this]() { readLiveConf(true); });
        connect(m_confWatch, &QFileSystemWatcher::fileChanged, m_confDebounce, static_cast<void (QTimer::*)()>(&QTimer::start));
        connect(m_confWatch, &QFileSystemWatcher::directoryChanged, m_confDebounce, static_cast<void (QTimer::*)()>(&QTimer::start));
    }
    if (!m_confWatch->files().contains(path) && QFileInfo::exists(path)) m_confWatch->addPath(path);
    const QString dir = QFileInfo(path).absolutePath();
    if (!m_confWatch->directories().contains(dir) && QFileInfo::exists(dir)) m_confWatch->addPath(dir);

    QSettings conf(path, QSettings::IniFormat);
    const bool zc = conf.value(confKey(QStringLiteral("viewfinderZeroCopy")), QStringLiteral("1")).toString().trimmed() != QLatin1String("0");
    const bool yuv = conf.value(confKey(QStringLiteral("viewfinderYuv")), QStringLiteral("1")).toString().trimmed() != QLatin1String("0");
    const int secs = qBound(0, conf.value(confKey(QStringLiteral("statsLog")), 0).toInt(), 3600);
    const int every = qBound(1, conf.value(confKey(QStringLiteral("aidsEvery")), 2).toInt(), 4);
    const bool gpu = conf.value(confKey(QStringLiteral("aidsGpu")), QStringLiteral("1")).toString().trimmed() != QLatin1String("0");
    const int screenMin = qBound(0, conf.value(confKey(QStringLiteral("screenOnMinutes")), 5).toInt(), 60);
    const QString now = QStringLiteral("statsLog=%1 viewfinderZeroCopy=%2 viewfinderYuv=%3 aidsEvery=%4 aidsGpu=%5 screenOnMinutes=%6")
                            .arg(secs).arg(zc ? 1 : 0).arg(yuv ? 1 : 0).arg(every).arg(gpu ? 1 : 0).arg(screenMin);
    if (now == m_liveConf) return;     /* saved, but none of these changed */
    m_liveConf = now;
    m_zeroCopy = zc;
    m_vfYuvPref = yuv;
    if (m_zeroCopy && !m_hwframes) {
        m_hwframes = new HwFrames(this);
        const QString cache = QDir::homePath() + QStringLiteral("/.cache/com.rawfish/rawfish-ir");
        QDir().mkpath(cache);
        m_hwframes->listen(cache + QStringLiteral("/hwframes.sock"));
    }
    m_statsSecs = secs;
    m_aidsEvery = every;
    m_screenOnMin = screenMin;
    updateKeepScreenOn();
    if (m_aidsGpuPref != gpu) { m_aidsGpuPref = gpu; buildOverlay(); }
    if (!m_stats) m_stats = new ProcStats;
    if (!m_statsTimer) {
        m_statsTimer = new QTimer(this);
        connect(m_statsTimer, &QTimer::timeout, this, &IndrawPreview::logStats);
    }
    if (m_statsSecs > 0) {
        if (!m_statsTimer->isActive()) {   /* the first line covers a whole interval */
            m_stats->reset();
            m_stats->line(m_process ? m_process->processId() : 0, QString());
            m_statsSerial = m_frameSerial;
            m_statsClock.start();
        }
        if (m_statsTimer->interval() != m_statsSecs * 1000 || !m_statsTimer->isActive()) m_statsTimer->start(m_statsSecs * 1000);
    } else {
        m_statsTimer->stop();
    }
    if (announce) {
        sendCommand(QStringLiteral("# conf changed: ") + now);
        syncVfMode();
    }
}

void IndrawPreview::logStats()
{
    if (!m_process || m_process->state() != QProcess::Running) return;
    if (!m_stats) m_stats = new ProcStats;
    const double secs = m_statsClock.isValid() ? m_statsClock.nsecsElapsed() / 1e9 : 0.0;
    m_statsClock.start();
    QString extra = QStringLiteral("vf=") + (m_vfModeSent.isEmpty() ? QStringLiteral("rgb") : m_vfModeSent);
    if (secs > 0 && m_frameSerial >= m_statsSerial)
        extra += QStringLiteral(" fps=%1").arg(double(m_frameSerial - m_statsSerial) / secs, 0, 'f', 1);
    m_statsSerial = m_frameSerial;
    sendCommand(QStringLiteral("# stats ") + m_stats->line(m_process->processId(), extra));
}

void IndrawPreview::syncVfMode()
{
    if (!m_process || m_process->state() != QProcess::Running) return;   /* asked with its first frame */
    if (m_hwframes) for (const QString &n : m_hwframes->takeNotes()) sendCommand(QStringLiteral("# zero copy: ") + n);
    const int aids = aidsMask();
    /* An engine that sends no aids' maps (older than 0439): the aids from
     * the YUV pipe, as before. Two seconds of zero-copy frames without any. */
    if (aids && m_vfModeSent == QLatin1String("hwb") && m_aidsSent == aids && !m_aidsGot && !m_aidsUnsupported &&
        m_aidsClock.isValid() && m_aidsClock.elapsed() > 2000) {
        m_aidsUnsupported = true;
        sendCommand(QStringLiteral("# zero copy: no aids maps from this engine, the aids from the YUV pipe"));
    }
    const bool hwbOk = m_zeroCopy && m_hwframes && !m_hwframes->failed() && !(aids && m_aidsUnsupported);
    const QString want = hwbOk ? QStringLiteral("hwb") : m_vfYuvPref ? QStringLiteral("yuv") : QStringLiteral("rgb");
    if (want != m_vfModeSent && !(want == QLatin1String("rgb") && m_vfModeSent.isEmpty())) {
        if (m_vfModeSent == QLatin1String("hwb") && m_hwframes && m_hwframes->failed())
            sendCommand(QStringLiteral("# zero copy: failed, back to the YUV pipe"));
        m_vfModeSent = want;
        m_aidsSent = -1;
        m_aidMaps.clear();
        sendCommand(want == QLatin1String("hwb") ? QStringLiteral("vf hwb ") + m_hwframes->path() : QStringLiteral("vf ") + want);
    }
    /* zero copy: the maps the aids on now need, with each frame */
    if (m_vfModeSent == QLatin1String("hwb") && (aids != m_aidsSent || (aids && m_aidsEvery != m_aidsEverySent))) {
        m_aidsSent = aids;
        m_aidsEverySent = m_aidsEvery;
        m_aidsGot = false;
        m_aidsClock.start();
        if (!aids) m_aidMaps.clear();
        sendCommand(aids ? QStringLiteral("vf aids %1 %2").arg(aids).arg(m_aidsEvery) : QStringLiteral("vf aids 0"));
    }
}

/* The display kept on while the viewfinder is in use (it went to sleep
 * after the system's delay, the viewfinder running: 15 Nov 2026): the
 * engine running, a touch less than screenOnMinutes ago (0: the system's
 * own delay, as before). Checked every 15 s and at each touch. */
void IndrawPreview::updateKeepScreenOn()
{
    if (!m_screenTimer) {
        m_screenTimer = new QTimer(this);
        m_screenTimer->setInterval(15000);
        connect(m_screenTimer, &QTimer::timeout, this, &IndrawPreview::updateKeepScreenOn);
        m_screenTimer->start();
    }
    const bool on = m_active && m_process && m_process->state() != QProcess::NotRunning && m_screenOnMin > 0 &&
                    m_lastTouch.isValid() && m_lastTouch.elapsed() < qint64(m_screenOnMin) * 60000;
    if (on != m_keepScreenOn) { m_keepScreenOn = on; emit keepScreenOnChanged(); }
}

void IndrawPreview::itemChange(ItemChange change, const ItemChangeData &value)
{
    /* every touch of the window counts, wherever it lands */
    if (change == ItemSceneChange && value.window) value.window->installEventFilter(this);
    QQuickItem::itemChange(change, value);
}

bool IndrawPreview::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type t = event->type();
    if (t == QEvent::TouchBegin || t == QEvent::MouseButtonPress || t == QEvent::KeyPress) {
        m_lastTouch.restart();
        if (!m_keepScreenOn) updateKeepScreenOn();
    }
    return QQuickItem::eventFilter(watched, event);
}

/* The engine's maps the aids on now read (SF2A; android/rawfish-ir.c,
 * aid_maps): 1 the peaking, 2 the zebra on the luma, 4 on the channels,
 * 8 the RAW zebra. */
int IndrawPreview::aidsMask() const
{
    int m = m_peaking ? 1 : 0;
    if (m_zebra) m |= m_zebraRawShare ? 8 : m_zebraLuma ? 2 : 4;
    return m;
}

QImage IndrawPreview::frameRgb() const
{
    return !m_frame.isNull() ? m_frame : i420ToRgb(m_yuv, m_frameSize);
}

/* Hands the latest frame to the worker, or notes that one waits. Nothing
 * to draw: the overlay is cleared at once. */
void IndrawPreview::buildOverlay()
{
    if ((!m_zebra && !m_peaking) || !hasFrame()) { m_overlay = QImage(); m_overlayPending = false; m_aidsGpu = false; return; }
    /* zero copy: the GPU draws the aids from the maps (AidsMaterial) --
     * but the RAW zebra, which keeps state from frame to frame, and when
     * rawfish-ir.conf aidsGpu=0 */
    m_aidsGpu = m_hwbId && m_aidsGpuPref && !(m_zebra && m_zebraRawShare);
    if (m_aidsGpu) { m_overlay = QImage(); m_overlayPending = false; update(); return; }
    if (m_overlayBusy) { m_overlayPending = true; return; }
    OverlayParams p; p.zebra = m_zebra; p.peaking = m_peaking; p.zebraLuma = m_zebraLuma; p.level = m_zebraLevel; p.edge = m_peakEdge; p.width = m_peakWidth;
    if (m_zebraRawShare) {                               /* the RAW zebra: the measured share, over what is shown */
        p.share = m_rawClipShare >= 0 ? qMin<qreal>(1, m_rawClipShare * m_zebraShareScale) : 0;
        if (m_rawLostShare >= 0) p.lostShare = qMin<qreal>(1, m_rawLostShare * m_zebraShareScale);
        p.region = sourceRectIn(m_frameSize);
    }
    if (m_hwbId) {
        /* zero copy: the engine's maps of the cells (none yet: nothing) */
        if (m_aidMaps.isEmpty()) { m_overlay = QImage(); return; }
        m_overlayBusy = true;
        QMetaObject::invokeMethod(m_overlayWorker, "computeCells", Qt::QueuedConnection, Q_ARG(QByteArray, m_aidMaps),
                                  Q_ARG(QSize, m_aidCells), Q_ARG(int, m_aidMask), Q_ARG(OverlayParams, p));
        return;
    }
    m_overlayBusy = true;
    QMetaObject::invokeMethod(m_overlayWorker, "compute", Qt::QueuedConnection, Q_ARG(QImage, m_frame),
                              Q_ARG(QByteArray, m_yuv), Q_ARG(QSize, m_frameSize), Q_ARG(OverlayParams, p));
}
void IndrawPreview::onOverlayDone(const QImage &overlay)
{
    m_overlayBusy = false;
    if ((m_zebra || m_peaking) && !m_aidsGpu) { m_overlay = overlay; update(); }   /* the GPU draws them now: dropped */
    if (m_overlayPending) { m_overlayPending = false; buildOverlay(); }
}
int IndrawPreview::peakWidth() const { return m_peakWidth; }
void IndrawPreview::setPeakWidth(int w) { w = qBound(1, w, 6); if (m_peakWidth == w) return; m_peakWidth = w; emit aidsChanged(); buildOverlay(); update(); }

bool IndrawPreview::zebra() const { return m_zebra; }
void IndrawPreview::setZebra(bool on) { if (m_zebra == on) return; m_zebra = on; emit aidsChanged(); syncVfMode(); buildOverlay(); update(); }
bool IndrawPreview::peaking() const { return m_peaking; }
void IndrawPreview::setPeaking(bool on) { if (m_peaking == on) return; m_peaking = on; emit aidsChanged(); syncVfMode(); buildOverlay(); update(); }
int IndrawPreview::zebraLevel() const { return m_zebraLevel; }
void IndrawPreview::setZebraLuma(bool on) { if (m_zebraLuma == on) return; m_zebraLuma = on; emit aidsChanged(); syncVfMode(); buildOverlay(); update(); }
void IndrawPreview::setZebraShareScale(qreal s)
{
    s = qBound<qreal>(0.1, s, 3);
    if (qFuzzyCompare(m_zebraShareScale, s)) return;
    m_zebraShareScale = s;
    emit aidsChanged(); buildOverlay(); update();
}
void IndrawPreview::setZebraRawShare(bool on)
{
    if (m_zebraRawShare == on) return;
    m_zebraRawShare = on;
    m_rawClipShare = m_rawLostShare = -1;                /* a measure from before is not this one's */
    emit aidsChanged(); syncVfMode(); buildOverlay(); update();
}
void IndrawPreview::setZebraLevel(int level) { level = qBound(1, level, 255); if (m_zebraLevel == level) return; m_zebraLevel = level; emit aidsChanged(); buildOverlay(); update(); }
int IndrawPreview::peakEdge() const { return m_peakEdge; }
void IndrawPreview::setPeakEdge(int edge) { edge = qBound(1, edge, 255); if (m_peakEdge == edge) return; m_peakEdge = edge; emit aidsChanged(); buildOverlay(); update(); }

QVariantMap IndrawPreview::caps() const { return m_caps; }
int IndrawPreview::liveIso() const { return m_liveIso; }
qreal IndrawPreview::liveExposureNs() const { return m_liveExposureNs; }
qreal IndrawPreview::meterMean() const { return m_meterMean; }
int IndrawPreview::meterIso() const { return m_meterIso; }
qreal IndrawPreview::meterExposureNs() const { return m_meterExposureNs; }
/* The histogram of a meter line (hist=r;g;b), and the spot's own (shist=,
 * present while a light reference is set), both on the same 64 bins. */
void IndrawPreview::takeHistogram(const QHash<QByteArray, QByteArray> &f)
{
    const QList<QByteArray> ch = f.value("hist").split(';');
    if (ch.size() != 3) return;
    m_histR = intList(ch.at(0));
    m_histG = intList(ch.at(1));
    m_histB = intList(ch.at(2));
    const QList<QByteArray> sh = f.value("shist").split(';');
    if (sh.size() == 3) { m_spotHistR = intList(sh.at(0)); m_spotHistG = intList(sh.at(1)); m_spotHistB = intList(sh.at(2)); }
    else { m_spotHistR.clear(); m_spotHistG.clear(); m_spotHistB.clear(); }
    m_histClipLow = f.value("clip_lo").toDouble();
    m_histClipHigh = f.value("clip_hi").toDouble();
    emit histogramChanged();
}
QVariantList IndrawPreview::spotHistR() const { return m_spotHistR; }
QVariantList IndrawPreview::spotHistG() const { return m_spotHistG; }
QVariantList IndrawPreview::spotHistB() const { return m_spotHistB; }

qreal IndrawPreview::vfMean() const { return m_vfMean; }
int IndrawPreview::wbCct() const { return m_wbCct; }
qreal IndrawPreview::wbDuv() const { return m_wbDuv; }
QVariantMap IndrawPreview::exposureState() const { return m_exposureState; }
QSize IndrawPreview::frameSize() const { return m_frameSize; }
QVariantMap IndrawPreview::focusState() const { return m_focusState; }
QVariantMap IndrawPreview::viewState() const { return m_viewState; }
QString IndrawPreview::histogramSource() const { return m_histogramSource; }
void IndrawPreview::setHistogramSource(const QString &source)
{
    if (m_histogramSource == source) return;
    m_histogramSource = source;
    emit histogramSourceChanged();
}
QVariantList IndrawPreview::histR() const { return m_histR; }
QVariantList IndrawPreview::histG() const { return m_histG; }
QVariantList IndrawPreview::histB() const { return m_histB; }
qreal IndrawPreview::histClipLow() const { return m_histClipLow; }
qreal IndrawPreview::histClipHigh() const { return m_histClipHigh; }

/* --- Rendering (render thread) --- */

/* The part of a frame the interface shows: the view window (fractions of
 * the band, as the controller sends it) mapped into the frame through the
 * crop it was really taken with ("fcrop"), kept inside the frame. */
static QRectF sourceRectF(const QSize &size, const QRectF &view, const QRectF &frameCrop)
{
    const QSizeF imageSize = size;
    const QRectF fc = frameCrop.width() > 0 ? frameCrop : QRectF(0, 0, 1, 1);
    const qreal sw = qMin<qreal>(1.0, view.width() / fc.width()) * imageSize.width();
    const qreal sh = qMin<qreal>(1.0, view.height() / fc.height()) * imageSize.height();
    const qreal sx = qBound<qreal>(0, (view.x() - fc.x()) / fc.width() * imageSize.width(), imageSize.width() - sw);
    const qreal sy = qBound<qreal>(0, (view.y() - fc.y()) / fc.height() * imageSize.height(), imageSize.height() - sh);
    return QRectF(sx, sy, sw, sh);
}
QRect IndrawPreview::sourceRectIn(const QSize &imageSize) const
{
    return sourceRectF(imageSize, m_viewWindow, m_frameCrop).toAlignedRect();
}

void IndrawPreview::setBubbleCentre(const QPointF &p)
{
    if (m_bubbleCentre == p) return;
    m_bubbleCentre = p;
    emit bubbleChanged();
    if (m_bubbleRadius > 0) update();
}
void IndrawPreview::setBubbleRadius(qreal r)
{
    r = qMax<qreal>(0, r);
    if (qFuzzyCompare(m_bubbleRadius + 1, r + 1)) return;
    m_bubbleRadius = r;
    emit bubbleChanged();
    update();
}

/* --- The I420 frame on the GPU (SF2Y, 0.3.11) ---
 *
 * The three planes go into three one-byte (luminance) textures, reused from
 * frame to frame while the size holds (glTexSubImage2D: no allocation), and
 * a shader turns them into colour as it draws: BT.601 full range, the
 * engine's coefficients (android/yuv.h), chroma filtered bilinearly. The
 * CPU only copies the bytes received; the RGB conversion the engine did on
 * the camera thread (1.2 Mpx, 30 times a second), the 3-byte frame in the
 * pipe and Qt's own conversion of an RGB888 image to 32 bits before its
 * upload are gone. Render thread only (updatePaintNode, the renderer). */
class YuvPlanes
{
public:
    ~YuvPlanes()
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (m_tex[0] && ctx && ctx == m_ctx) ctx->functions()->glDeleteTextures(3, m_tex);   /* only its own context's (see HwFrames::m_ctx) */
    }
    /* Uploads one frame; false without a current GL context or on a short buffer. */
    bool upload(const QByteArray &i420, const QSize &size)
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        const int w = size.width(), h = size.height(), cw = (w + 1) / 2, ch = (h + 1) / 2;
        if (!ctx || w <= 0 || h <= 0 || quint64(i420.size()) != quint64(w) * h + 2u * quint64(cw) * ch) return false;
        QOpenGLFunctions *gl = ctx->functions();
        if (ctx != m_ctx) { m_tex[0] = m_tex[1] = m_tex[2] = 0; m_size = QSize(); m_ctx = ctx; }   /* a new context: new names */
        if (!m_tex[0]) gl->glGenTextures(3, m_tex);
        const bool fresh = m_size != size;
        m_size = size;
        const uchar *p = reinterpret_cast<const uchar *>(i420.constData());
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);        /* rows unpadded: any width */
        for (int i = 0; i < 3; ++i) {
            const int pw = i ? cw : w, ph = i ? ch : h;
            gl->glBindTexture(GL_TEXTURE_2D, m_tex[i]);
            if (fresh) {
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, pw, ph, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, p);
            } else {
                gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_LUMINANCE, GL_UNSIGNED_BYTE, p);
            }
            p += pw * ph;
        }
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }
    GLuint tex(int i) const { return m_tex[i]; }
private:
    GLuint m_tex[3] = { 0, 0, 0 };
    QSize m_size;
    QOpenGLContext *m_ctx = nullptr;   /* the context the names belong to */
};

class YuvMaterial : public QSGMaterial
{
public:
    explicit YuvMaterial(const QSharedPointer<YuvPlanes> &planes) : planes(planes) {}
    QSGMaterialType *type() const override { static QSGMaterialType t; return &t; }
    QSGMaterialShader *createShader() const override;
    int compare(const QSGMaterial *other) const override
    {
        const YuvPlanes *a = planes.data(), *b = static_cast<const YuvMaterial *>(other)->planes.data();
        return a == b ? 0 : (a < b ? -1 : 1);
    }
    QSharedPointer<YuvPlanes> planes;
};

class YuvShader : public QSGMaterialShader
{
public:
    const char *vertexShader() const override
    {
        return "attribute highp vec4 qt_VertexPosition;\n"
               "attribute highp vec2 qt_VertexTexCoord;\n"
               "uniform highp mat4 qt_Matrix;\n"
               "varying highp vec2 vTex;\n"
               "void main() {\n"
               "    vTex = qt_VertexTexCoord;\n"
               "    gl_Position = qt_Matrix * qt_VertexPosition;\n"
               "}\n";
    }
    const char *fragmentShader() const override
    {
        /* R = Y + 1.402 V, G = Y - 0.344 U - 0.714 V, B = Y + 1.772 U, with
         * U and V centred on 128/255: yuv.h's 91881, 22554, 46802, 116130 / 65536 */
        return "uniform sampler2D texY;\n"
               "uniform sampler2D texU;\n"
               "uniform sampler2D texV;\n"
               "uniform lowp float qt_Opacity;\n"
               "varying highp vec2 vTex;\n"
               "void main() {\n"
               "    mediump float y = texture2D(texY, vTex).r;\n"
               "    mediump float u = texture2D(texU, vTex).r - 0.5019608;\n"
               "    mediump float v = texture2D(texV, vTex).r - 0.5019608;\n"
               "    mediump vec3 rgb = vec3(y + 1.4020 * v, y - 0.3441 * u - 0.7141 * v, y + 1.7720 * u);\n"
               "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0) * qt_Opacity;\n"
               "}\n";
    }
    char const *const *attributeNames() const override
    {
        static const char *const names[] = { "qt_VertexPosition", "qt_VertexTexCoord", nullptr };
        return names;
    }
    void initialize() override
    {
        m_matrix = program()->uniformLocation("qt_Matrix");
        m_opacity = program()->uniformLocation("qt_Opacity");
        m_texY = program()->uniformLocation("texY");
        m_texU = program()->uniformLocation("texU");
        m_texV = program()->uniformLocation("texV");
    }
    void updateState(const RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        if (state.isMatrixDirty()) program()->setUniformValue(m_matrix, state.combinedMatrix());
        if (state.isOpacityDirty()) program()->setUniformValue(m_opacity, state.opacity());
        program()->setUniformValue(m_texY, 0);
        program()->setUniformValue(m_texU, 1);
        program()->setUniformValue(m_texV, 2);
        const YuvMaterial *m = static_cast<YuvMaterial *>(newMaterial);
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        for (int i = 2; i >= 0; --i) {                    /* unit 0 (Y) left active, as Qt expects */
            gl->glActiveTexture(GL_TEXTURE0 + i);
            gl->glBindTexture(GL_TEXTURE_2D, m->planes->tex(i));
        }
    }
private:
    int m_matrix = -1, m_opacity = -1, m_texY = -1, m_texU = -1, m_texV = -1;
};

QSGMaterialShader *YuvMaterial::createShader() const { return new YuvShader; }

/* A camera buffer drawn as it is (zero copy): an external texture, the
 * GPU's own YUV sampling (hwframes.h). */
class OesMaterial : public QSGMaterial
{
public:
    explicit OesMaterial(unsigned tex) : tex(tex) {}
    QSGMaterialType *type() const override { static QSGMaterialType t; return &t; }
    QSGMaterialShader *createShader() const override;
    int compare(const QSGMaterial *other) const override
    {
        const unsigned o = static_cast<const OesMaterial *>(other)->tex;
        return tex == o ? 0 : (tex < o ? -1 : 1);
    }
    unsigned tex;
};
class OesShader : public QSGMaterialShader
{
public:
    const char *vertexShader() const override
    {
        return "attribute highp vec4 qt_VertexPosition;\n"
               "attribute highp vec2 qt_VertexTexCoord;\n"
               "uniform highp mat4 qt_Matrix;\n"
               "varying highp vec2 vTex;\n"
               "void main() {\n"
               "    vTex = qt_VertexTexCoord;\n"
               "    gl_Position = qt_Matrix * qt_VertexPosition;\n"
               "}\n";
    }
    const char *fragmentShader() const override
    {
        return "#extension GL_OES_EGL_image_external : require\n"
               "uniform samplerExternalOES tex;\n"
               "uniform lowp float qt_Opacity;\n"
               "varying highp vec2 vTex;\n"
               "void main() {\n"
               "    gl_FragColor = vec4(texture2D(tex, vTex).rgb, 1.0) * qt_Opacity;\n"
               "}\n";
    }
    char const *const *attributeNames() const override
    {
        static const char *const names[] = { "qt_VertexPosition", "qt_VertexTexCoord", nullptr };
        return names;
    }
    void initialize() override
    {
        m_matrix = program()->uniformLocation("qt_Matrix");
        m_opacity = program()->uniformLocation("qt_Opacity");
        m_tex = program()->uniformLocation("tex");
    }
    void updateState(const RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        if (state.isMatrixDirty()) program()->setUniformValue(m_matrix, state.combinedMatrix());
        if (state.isOpacityDirty()) program()->setUniformValue(m_opacity, state.opacity());
        program()->setUniformValue(m_tex, 0);
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(kGlTextureExternal, static_cast<OesMaterial *>(newMaterial)->tex);
    }
private:
    int m_matrix = -1, m_opacity = -1, m_tex = -1;
};
QSGMaterialShader *OesMaterial::createShader() const { return new OesShader; }

/* The aids drawn by the GPU from the engine's maps (zero copy, SF2A): the
 * peaking's coded |Laplacian| and the zebra's value of each 2x2 cell in two
 * one-byte textures, the decisions of overlayCore (cells: border 1, the
 * peaking a local maximum >= edge, thickened right and down by `extra`
 * cells, the zebra's stripes over it every 4 cells) made per fragment. No
 * image built and uploaded per update (~20 points of CPU at 15 a second,
 * 15 Nov 2026): two 0.3 MB textures refilled. The RAW zebra (its levels
 * from the histogram, its stripes kept from frame to frame) stays on the
 * CPU. Render thread only. */
class AidTextures
{
public:
    ~AidTextures()
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (m_tex[0] && ctx && ctx == m_ctx) ctx->functions()->glDeleteTextures(2, m_tex);   /* only its own context's */
    }
    /* i: 0 the peaking's map, 1 the zebra's; the bytes of one cw x ch map */
    bool upload(int i, const uchar *map, const QSize &cells)
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx || !map || cells.isEmpty() || i < 0 || i > 1) return false;
        QOpenGLFunctions *gl = ctx->functions();
        if (ctx != m_ctx) { m_tex[0] = m_tex[1] = 0; m_size[0] = m_size[1] = QSize(); m_ctx = ctx; }   /* a new context: new names */
        if (!m_tex[0]) gl->glGenTextures(2, m_tex);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->glBindTexture(GL_TEXTURE_2D, m_tex[i]);
        if (m_size[i] != cells) {
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, cells.width(), cells.height(), 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, map);
            m_size[i] = cells;
        } else {
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cells.width(), cells.height(), GL_LUMINANCE, GL_UNSIGNED_BYTE, map);
        }
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    }
    GLuint tex(int i) const { return m_tex[i]; }
private:
    GLuint m_tex[2] = { 0, 0 };
    QSize m_size[2];
    QOpenGLContext *m_ctx = nullptr;   /* the context the names belong to */
};
struct AidsDraw {
    bool peak = false, zebra = false;
    int edge = 40, level = 250, extra = 0;
    QSize cells;
};
class AidsMaterial : public QSGMaterial
{
public:
    AidsMaterial(const QSharedPointer<AidTextures> &t, const AidsDraw &d) : tex(t), d(d) { setFlag(Blending, true); }
    QSGMaterialType *type() const override { static QSGMaterialType t; return &t; }
    QSGMaterialShader *createShader() const override;
    int compare(const QSGMaterial *other) const override
    {
        const AidTextures *a = tex.data(), *b = static_cast<const AidsMaterial *>(other)->tex.data();
        return a == b ? 0 : (a < b ? -1 : 1);
    }
    QSharedPointer<AidTextures> tex;
    AidsDraw d;
};
class AidsShader : public QSGMaterialShader
{
public:
    const char *vertexShader() const override
    {
        return "attribute highp vec4 qt_VertexPosition;\n"
               "attribute highp vec2 qt_VertexTexCoord;\n"
               "uniform highp mat4 qt_Matrix;\n"
               "varying highp vec2 vTex;\n"
               "void main() {\n"
               "    vTex = qt_VertexTexCoord;\n"
               "    gl_Position = qt_Matrix * qt_VertexPosition;\n"
               "}\n";
    }
    const char *fragmentShader() const override
    {
        /* the colours of overlayCore, premultiplied as there */
        return "uniform sampler2D lapT;\n"
               "uniform sampler2D valT;\n"
               "uniform highp vec2 size;\n"
               "uniform highp float edge;\n"
               "uniform highp float level;\n"
               "uniform highp float extra;\n"
               "uniform highp float doPeak;\n"
               "uniform highp float doZebra;\n"
               "uniform lowp float qt_Opacity;\n"
               "varying highp vec2 vTex;\n"
               "highp float q(highp vec2 c) { return floor(texture2D(lapT, (c + 0.5) / size).r * 255.0 + 0.5); }\n"
               "bool peakAt(highp vec2 c) {\n"
               "    if (c.x < 1.0 || c.y < 1.0 || c.x > size.x - 2.0 || c.y > size.y - 2.0) return false;\n"
               "    highp float a = q(c);\n"
               "    return a >= edge && a >= q(c - vec2(1.0, 0.0)) && a >= q(c + vec2(1.0, 0.0))\n"
               "        && a >= q(c - vec2(0.0, 1.0)) && a >= q(c + vec2(0.0, 1.0));\n"
               "}\n"
               "void main() {\n"
               "    highp vec2 c = clamp(floor(vTex * size), vec2(0.0), size - 1.0);\n"
               "    lowp vec4 col = vec4(0.0);\n"
               "    if (doPeak > 0.5) {\n"
               "        bool p = peakAt(c);\n"
               "        for (int k = 1; k <= 5; ++k)\n"
               "            if (float(k) <= extra && !p) p = peakAt(c - vec2(float(k), 0.0)) || peakAt(c - vec2(0.0, float(k)));\n"
               "        if (p) col = vec4(1.0, 0.156863, 0.156863, 1.0);\n"
               "    }\n"
               "    if (doZebra > 0.5) {\n"
               "        highp float v = floor(texture2D(valT, (c + 0.5) / size).r * 255.0 + 0.5);\n"
               "        if (v >= level) col = mod(floor((c.x + c.y) / 4.0), 2.0) > 0.5 ? vec4(1.0, 1.0, 1.0, 0.862745) : vec4(0.0, 0.0, 0.0, 0.862745);\n"
               "    }\n"
               "    gl_FragColor = col * qt_Opacity;\n"
               "}\n";
    }
    char const *const *attributeNames() const override
    {
        static const char *const names[] = { "qt_VertexPosition", "qt_VertexTexCoord", nullptr };
        return names;
    }
    void initialize() override
    {
        QOpenGLShaderProgram *p = program();
        m_matrix = p->uniformLocation("qt_Matrix"); m_opacity = p->uniformLocation("qt_Opacity");
        m_lap = p->uniformLocation("lapT"); m_val = p->uniformLocation("valT");
        m_size = p->uniformLocation("size"); m_edge = p->uniformLocation("edge"); m_level = p->uniformLocation("level");
        m_extra = p->uniformLocation("extra"); m_doPeak = p->uniformLocation("doPeak"); m_doZebra = p->uniformLocation("doZebra");
    }
    void updateState(const RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        QOpenGLShaderProgram *p = program();
        if (state.isMatrixDirty()) p->setUniformValue(m_matrix, state.combinedMatrix());
        if (state.isOpacityDirty()) p->setUniformValue(m_opacity, state.opacity());
        const AidsMaterial *m = static_cast<AidsMaterial *>(newMaterial);
        p->setUniformValue(m_lap, 0);
        p->setUniformValue(m_val, 1);
        p->setUniformValue(m_size, QVector2D(m->d.cells.width(), m->d.cells.height()));
        p->setUniformValue(m_edge, GLfloat(m->d.edge));
        p->setUniformValue(m_level, GLfloat(m->d.level));
        p->setUniformValue(m_extra, GLfloat(m->d.extra));
        p->setUniformValue(m_doPeak, GLfloat(m->d.peak ? 1 : 0));
        p->setUniformValue(m_doZebra, GLfloat(m->d.zebra ? 1 : 0));
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindTexture(GL_TEXTURE_2D, m->tex->tex(1));
        gl->glActiveTexture(GL_TEXTURE0);                 /* unit 0 left active, as Qt expects */
        gl->glBindTexture(GL_TEXTURE_2D, m->tex->tex(0));
    }
private:
    int m_matrix = -1, m_opacity = -1, m_lap = -1, m_val = -1, m_size = -1, m_edge = -1, m_level = -1,
        m_extra = -1, m_doPeak = -1, m_doZebra = -1;
};
QSGMaterialShader *AidsMaterial::createShader() const { return new AidsShader; }

/* What the item keeps from one frame to the next on the render thread: the
 * I420 textures (sized once, refilled each frame) and which frame they hold.
 * Its children (the frame, the aids, the magnifier) are rebuilt each time. */
class FrameRoot : public QSGNode
{
public:
    QSharedPointer<YuvPlanes> planes;
    quint64 uploaded = 0;          /* m_frameSerial of the frame in the textures */
    quint64 shownHwb = 0;          /* the camera buffer drawn (zero copy), 0 none */
    QSharedPointer<AidTextures> aids;  /* the aids' maps (zero copy), on the GPU */
    quint64 aidsUploaded = 0;      /* m_aidSerial of the maps in them */
};

static QSGTextureMaterial *textureMaterial(QSGTexture *texture, bool linear)
{
    QSGTextureMaterial *mat = new QSGTextureMaterial;
    mat->setTexture(texture);
    mat->setFlag(QSGMaterial::Blending, texture->hasAlphaChannel());   /* the overlay is transparent, the frame not */
    mat->setFiltering(linear ? QSGTexture::Linear : QSGTexture::Nearest);
    mat->setHorizontalWrapMode(QSGTexture::ClampToEdge);
    mat->setVerticalWrapMode(QSGTexture::ClampToEdge);
    return mat;
}

/* A disc of an image (a triangle fan) drawn with `mat`: centred on `centre`
 * with `radius` (item pixels), showing the image around the point
 * `texCentre` (image pixels of a `texSize` image, which fills `sub` of the
 * texture: an atlas, or 0..1) at `pxPerImagePx` item pixels per image
 * pixel. The node owns the material; the material refers to the texture. */
static QSGGeometryNode *discNode(QSGMaterial *mat, const QRectF &sub, const QSizeF &texSize, const QPointF &centre, qreal radius,
                                 const QPointF &texCentre, qreal pxPerImagePx)
{
    const int n = 64;                                      /* segments: smooth at any size shown */
    QSGGeometry *geo = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), n + 2);
    geo->setDrawingMode(GL_TRIANGLE_FAN);
    QSGGeometry::TexturedPoint2D *v = geo->vertexDataAsTexturedPoint2D();
    const qreal tr = radius / pxPerImagePx;                 /* the radius in image pixels */
    auto put = [&](int k, qreal dx, qreal dy) {
        const qreal u = (texCentre.x() + dx * tr) / texSize.width(), w = (texCentre.y() + dy * tr) / texSize.height();
        v[k].set(float(centre.x() + dx * radius), float(centre.y() + dy * radius),
                 float(sub.x() + u * sub.width()), float(sub.y() + w * sub.height()));
    };
    put(0, 0, 0);
    for (int k = 0; k <= n; ++k) {
        const qreal a = 2 * M_PI * k / n;
        put(k + 1, std::cos(a), std::sin(a));
    }
    QSGGeometryNode *node = new QSGGeometryNode;
    node->setGeometry(geo);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(mat);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

QSGNode *IndrawPreview::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    FrameRoot *root = static_cast<FrameRoot *>(oldNode);
    if (m_hwprobe) m_hwprobe->renderPending(window());   /* zero-copy probe: off screen, GL current */
    if (!hasFrame() || width() <= 0 || height() <= 0 || !window()) {
        delete root;
        return nullptr;
    }
    /* Zero copy: the buffer's external texture (imported the first time).
     * Not there yet (its handle still on its way) or failed: the last
     * picture stays as it is -- its buffer is not released until a newer
     * one is drawn. */
    unsigned oesTex = 0;
    if (m_hwbId) {
        oesTex = m_hwframes ? m_hwframes->texture(m_hwbId) : 0;
        if (!oesTex) {
            if (m_hwframes && m_hwframes->failed()) QMetaObject::invokeMethod(this, "syncVfMode", Qt::QueuedConnection);
            if (root) return root;
            return nullptr;
        }
    }
    if (!root) root = new FrameRoot;
    /* the previous frame's children go; the textures stay */
    while (QSGNode *child = root->firstChild()) { root->removeChildNode(child); delete child; }
    /* the camera buffer drawn before goes back once a newer picture is drawn */
    if (root->shownHwb && root->shownHwb != m_hwbId && m_hwframes) m_hwframes->release(root->shownHwb);
    root->shownHwb = m_hwbId;
    m_hwbShown = m_hwbId;

    /* The frame: an I420 one into the reused textures, drawn by the shader
     * (uploaded once per frame received, not at every repaint -- the aids
     * and the magnifier repaint too); an RGB one as a texture, as before. */
    const QSize frameSize = m_frameSize;
    QSGTexture *texture = nullptr;                       /* the RGB frame's, if any */
    bool yuv = false;
    if (m_hwbId) {
        root->planes.reset();
        root->uploaded = 0;
    } else if (!m_yuv.isEmpty()) {
        if (!root->planes) root->planes.reset(new YuvPlanes);
        yuv = root->uploaded == m_frameSerial || root->planes->upload(m_yuv, frameSize);
        if (yuv) root->uploaded = m_frameSerial;
    }
    if (!yuv && !oesTex) {
        root->planes.reset();                            /* an RGB stream: no textures kept */
        root->uploaded = 0;
        texture = window()->createTextureFromImage(frameRgb());
        if (!texture) { delete root; return nullptr; }
        texture->setFiltering(QSGTexture::Linear);
    }

    /* Source rect: the view window (array-normalised) mapped into this
     * frame through the crop it was really taken with, kept inside the frame.
     * Whatever the HAL's latency in applying a new crop, the part of the
     * scene shown is the one asked for: the display magnification is derived
     * from the frame, never predicted. */
    const QRectF sourceRect = sourceRectF(frameSize, m_viewWindow, m_frameCrop);
    const QSizeF sourceSize = sourceRect.size();

    /* Fit the source into the item: 'fill' covers the item (cropping overflow),
     * otherwise the whole frame fits inside (letterboxed). */
    /* At 90/270 the item is rotated, so its width is fitted against the source
     * HEIGHT and vice versa. Swap the target extents for the fit computation. */
    const bool swap = (m_displayRotation == 90 || m_displayRotation == 270);
    const QRectF target(0, 0, width(), height());
    const qreal tw = swap ? target.height() : target.width();
    const qreal th = swap ? target.width()  : target.height();
    const qreal scale = m_fill
            ? qMax(tw / sourceSize.width(), th / sourceSize.height())
            : qMin(tw / sourceSize.width(), th / sourceSize.height());
    const QSizeF drawSize(sourceSize.width() * scale, sourceSize.height() * scale);
    const QRectF drawRect(target.center().x() - drawSize.width() / 2,
                          target.center().y() - drawSize.height() / 2,
                          drawSize.width(), drawSize.height());

    if (oesTex) {
        QSGGeometryNode *frame = new QSGGeometryNode;
        QSGGeometry *geo = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        QSGGeometry::updateTexturedRectGeometry(geo, drawRect,
                QRectF(sourceRect.x() / frameSize.width(), sourceRect.y() / frameSize.height(),
                       sourceRect.width() / frameSize.width(), sourceRect.height() / frameSize.height()));
        frame->setGeometry(geo);
        frame->setFlag(QSGNode::OwnsGeometry);
        frame->setMaterial(new OesMaterial(oesTex));
        frame->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(frame);
    } else if (yuv) {
        QSGGeometryNode *frame = new QSGGeometryNode;
        QSGGeometry *geo = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        QSGGeometry::updateTexturedRectGeometry(geo, drawRect,
                QRectF(sourceRect.x() / frameSize.width(), sourceRect.y() / frameSize.height(),
                       sourceRect.width() / frameSize.width(), sourceRect.height() / frameSize.height()));
        frame->setGeometry(geo);
        frame->setFlag(QSGNode::OwnsGeometry);
        frame->setMaterial(new YuvMaterial(root->planes));
        frame->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(frame);
    } else {
        QSGSimpleTextureNode *frame = new QSGSimpleTextureNode;
        frame->setOwnsTexture(true);
        frame->setTexture(texture);
        frame->setRect(drawRect);
        frame->setSourceRect(sourceRect);
        root->appendChildNode(frame);
    }

    /* The aids drawn by the GPU from the maps (zero copy): the maps uploaded
     * when new, a node over the frame with the frame's mapping. */
    AidsDraw aidsDraw;
    if (oesTex && m_aidsGpu && !m_aidMaps.isEmpty() && !m_aidCells.isEmpty()) {
        const int n = m_aidCells.width() * m_aidCells.height();
        const uchar *mp = reinterpret_cast<const uchar *>(m_aidMaps.constData());
        const uchar *lapMap = nullptr, *valMap = nullptr;
        int off = 0;
        for (int b = 1; b <= 8; b <<= 1) {
            if (!(m_aidMask & b)) continue;
            if (off + n > m_aidMaps.size()) break;
            if (b == 1) lapMap = mp + off;
            if (b == (m_zebraLuma ? 2 : 4)) valMap = mp + off;
            off += n;
        }
        if (!root->aids) root->aids = QSharedPointer<AidTextures>::create();
        if (root->aidsUploaded != m_aidSerial) {
            if (lapMap) root->aids->upload(0, lapMap, m_aidCells);
            if (valMap) root->aids->upload(1, valMap, m_aidCells);
            root->aidsUploaded = m_aidSerial;
        }
        aidsDraw.peak = m_peaking && lapMap;
        aidsDraw.zebra = m_zebra && valMap;
        aidsDraw.edge = m_peakEdge; aidsDraw.level = m_zebraLevel;
        aidsDraw.extra = qMax(1, (qMin(m_peakWidth, 6) + 1) / 2) - 1;
        aidsDraw.cells = m_aidCells;
        if (aidsDraw.peak || aidsDraw.zebra) {
            QSGGeometryNode *ov = new QSGGeometryNode;
            QSGGeometry *geo = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
            QSGGeometry::updateTexturedRectGeometry(geo, drawRect,
                    QRectF(sourceRect.x() / frameSize.width(), sourceRect.y() / frameSize.height(),
                           sourceRect.width() / frameSize.width(), sourceRect.height() / frameSize.height()));
            ov->setGeometry(geo);
            ov->setFlag(QSGNode::OwnsGeometry);
            ov->setMaterial(new AidsMaterial(root->aids, aidsDraw));
            ov->setFlag(QSGNode::OwnsMaterial);
            root->appendChildNode(ov);
        }
    }
    /* The aids overlay, if any, drawn over the frame with the same mapping
     * (its pixels are the frame's). */
    QSGTexture *ovTexShown = nullptr;                    /* for the magnifier below */
    if (!m_overlay.isNull()) {
        QSGTexture *ovTex = window()->createTextureFromImage(m_overlay);
        if (ovTex) {
            ovTex->setFiltering(QSGTexture::Nearest);
            QSGSimpleTextureNode *ov = new QSGSimpleTextureNode;
            ov->setOwnsTexture(true);
            ov->setTexture(ovTex);
            ov->setRect(drawRect);
            /* the frame's pixels, or the engine's 2x2 cells (zero copy) */
            const qreal ks = frameSize.width() > 0 ? qreal(m_overlay.width()) / frameSize.width() : 1.0;
            ov->setSourceRect(QRectF(sourceRect.x() * ks, sourceRect.y() * ks, sourceRect.width() * ks, sourceRect.height() * ks));
            root->appendChildNode(ov);
            ovTexShown = ovTex;
        }
    }
    /* The focus point's magnifier (bubbleRadius > 0): over everything, a disc
     * of the frame -- and of the aids, peaking included -- around the point,
     * at twice the scale it is shown at. The frame's own pixels: past the
     * view (a loupe's edge) it shows what the frame holds there. */
    if (m_bubbleRadius > 0) {
        const qreal mag = 2.0;
        const QPointF tc(sourceRect.x() + (m_bubbleCentre.x() - drawRect.x()) / scale,
                         sourceRect.y() + (m_bubbleCentre.y() - drawRect.y()) / scale);
        if (oesTex)
            root->appendChildNode(discNode(new OesMaterial(oesTex), QRectF(0, 0, 1, 1), frameSize,
                                           m_bubbleCentre, m_bubbleRadius, tc, scale * mag));
        else if (yuv)
            root->appendChildNode(discNode(new YuvMaterial(root->planes), QRectF(0, 0, 1, 1), frameSize,
                                           m_bubbleCentre, m_bubbleRadius, tc, scale * mag));
        else
            root->appendChildNode(discNode(textureMaterial(texture, true), texture->normalizedTextureSubRect(), frameSize,
                                           m_bubbleCentre, m_bubbleRadius, tc, scale * mag));
        if (aidsDraw.peak || aidsDraw.zebra) {             /* the GPU's aids, at the same scale */
            const qreal ks = frameSize.width() > 0 ? qreal(aidsDraw.cells.width()) / frameSize.width() : 1.0;
            root->appendChildNode(discNode(new AidsMaterial(root->aids, aidsDraw), QRectF(0, 0, 1, 1), aidsDraw.cells,
                                           m_bubbleCentre, m_bubbleRadius, tc * ks, scale * mag / ks));
        }
        if (ovTexShown) {                                  /* the aids, at the same scale */
            const qreal ks = frameSize.width() > 0 ? qreal(m_overlay.width()) / frameSize.width() : 1.0;
            root->appendChildNode(discNode(textureMaterial(ovTexShown, false), ovTexShown->normalizedTextureSubRect(), m_overlay.size(),
                                           m_bubbleCentre, m_bubbleRadius, tc * ks, scale * mag / ks));
        }
    }
    return root;
}

/* --- Process error handling --- */

void IndrawPreview::onProcessError(QProcess::ProcessError error)
{
    Q_UNUSED(error)
    setError(m_process ? m_process->errorString() : tr("process error"));
    setRunning(false);
}

void IndrawPreview::onProcessFinished(int code, QProcess::ExitStatus status)
{
    Q_UNUSED(code)
    Q_UNUSED(status)
    if (sender() != m_process) return;      /* a replaced engine: not ours any more */
    /* The dead process goes: the restart below only starts an engine when
     * there is none (m_process null), and nothing else cleared it after an
     * unexpected exit -- so the "try again" never ran and the viewfinder
     * stayed on "engine stopped" until a camera switch or a restart of the
     * app (ui 185; October 2026, a JP2 user's "many engine stopped"). */
    m_process->disconnect(this);
    m_process->deleteLater();
    m_process = nullptr;
    m_buffer.clear();
    setRunning(false);
    /* Still meant to be active: the engine died. Say so, and try again a few
     * times with a pause (a transient HAL hiccup recovers; a real fault
     * stops after the third try instead of looping). */
    if (m_active) {
        if (!m_gotCaps) m_sizeFallback = true;   /* it never opened: retry at "max" */
        if (!m_gotCaps && m_sessionRaw) m_rawFellBack = true;   /* and as JPEG, if the RAW session died */
        /* A session with the YUV stream that dies twice before its caps is
         * retried without it (JPG = the camera's JPEG); once could be a
         * camera service hiccup, which must not change the photos. */
        if (!m_gotCaps && m_sessionYuv && !m_yuvFellBack && ++m_yuvFails >= 2) {
            m_yuvFellBack = true;
            emit yuvFellBackChanged();
        }
        setError(tr("engine stopped"));
        scheduleRestart();
    }
}

void IndrawPreview::scheduleRestart()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastRestartMs > 60000) m_restarts = 0;   /* a quiet minute resets the count */
    if (m_restarts >= 3) {
        setError(tr("engine stopped (3 restarts failed)"));
        return;
    }
    m_restarts++;
    m_lastRestartMs = now;
    QTimer::singleShot(1000 * m_restarts, this, [this]() {
        if (m_active && !m_process) { setError(tr("restarting engine…")); start(); }
    });
}

/* --- Small setters --- */

void IndrawPreview::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    emit runningChanged();
}

void IndrawPreview::setError(const QString &error)
{
    if (m_errorString == error) return;
    m_errorString = error;
    emit errorStringChanged();
}
