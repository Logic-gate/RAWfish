/*
 * indrawpreview.cpp -- see indrawpreview.h.
 */
#include "indrawpreview.h"

#include <QSGSimpleTextureNode>
#include <QQuickWindow>
#include <QtEndian>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTransform>
#include <signal.h>
#include <QSettings>
#include <QRegularExpression>
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
}

IndrawPreview::~IndrawPreview()
{
    stop(true);
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
    /* The output folders exist before the first photo, so the gallery's
     * folder models watch existing directories from the start. */
    QDir().mkpath(m_jpgDir);
    QDir().mkpath(m_dngDir);
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
    const QString cap = (m_captureSize.isEmpty() || m_sizeFallback) ? QStringLiteral("max")
            : QStringLiteral("%1x%2").arg(m_captureSize.width()).arg(m_captureSize.height());
    const QString prev = m_previewSize.isEmpty() ? QStringLiteral("max")
            : QStringLiteral("%1x%2").arg(m_previewSize.width()).arg(m_previewSize.height());
    /* The RAW stream is always "max": the engine picks the camera's RAW16
     * size, or opens without RAW when it lists none (front camera). Tying it
     * to the JPEG size failed on every camera but the back one ("raw16 size
     * not listed") and on 16:9 JPEG sizes. */
    args << QStringLiteral("--log") << log
         << QStringLiteral("--jpgdir") << m_jpgDir << QStringLiteral("--dngdir") << m_dngDir
         << QStringLiteral("stream")
         << m_cameraId
         << cap << prev << QStringLiteral("raw=max");

    m_buffer.clear();
    /* Inside the Sailjail sandbox /usr/bin only holds the app binary
     * (firejail --private-bin), so "rawfish-ir-cli" in the PATH does not exist
     * there: use the engine's own directory, /usr/libexec/rawfish-ir, visible in the
     * sandbox, and fall back to the PATH for an unsandboxed/dev run. */
    const QString libexec = QStringLiteral("/usr/libexec/rawfish-ir/rawfish-ir-cli");
    const QString engine = QFileInfo(libexec).isExecutable() ? libexec : QStringLiteral("rawfish-ir-cli");
    m_process->start(engine, args);
}

void IndrawPreview::stop(bool wait)
{
    if (!m_process)
        return;
    /* This process is on its way out: none of its signals must reach us any
     * more. Its "finished" (emitted inside waitForFinished, or later after a
     * kill) used to be taken for the NEW engine dying: "engine stopped" on a
     * camera switch while the new engine was streaming. */
    QProcess *old = m_process;
    m_process = nullptr;
    old->disconnect(this);
    if (m_stacking) { m_stacking = false; emit stackingChanged(); }
    /* Ask the engine to quit cleanly (closes the camera), then tear down.
     * Not waited for on the UI thread (ui 168): a stack sub-frame of 0.5 s
     * plus a DNG write could hold the engine for seconds, and the camera
     * switch or the sleep froze that long. The old process is deleted when
     * it ends, and killed if it has not ended in 3 s. The destructor
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
            QTimer::singleShot(3000, old, [old]() { if (old->state() != QProcess::NotRunning) old->kill(); });
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
}

/* Settings: a plain INI file in the app's config directory (whitelisted by
 * Sailjail), so the UI preferences survive restarts without dconf. */
static QString settingsPath()
{
    const QString dir = QDir::homePath() + QStringLiteral("/.config/com.rawfish/rawfish-ir");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/rawfish-ir.conf");
}
/* Output settings, from dconf through the QML (ConfigurationGroup). A folder
 * outside ~/Pictures would make every sandboxed write fail: it is accepted
 * (a developer may run unsandboxed), but an empty or relative value and a
 * prefix outside the engine's safe charset fall back to the defaults. */
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
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    const QString jp = folder(jpgDir), dn = folder(dngDir);
    const QString pf = safe.match(prefix).hasMatch() ? prefix : QStringLiteral("photo");
    if (jp == m_jpgDir && dn == m_dngDir && pf == m_prefix) return;
    m_jpgDir = jp; m_dngDir = dn; m_prefix = pf;
    QDir().mkpath(m_jpgDir);
    QDir().mkpath(m_dngDir);
    emit outputChanged();
}
QString IndrawPreview::jpgDir() const { return m_jpgDir; }
QString IndrawPreview::dngDir() const { return m_dngDir; }
QString IndrawPreview::capturePrefix() const { return m_prefix; }

void IndrawPreview::installExamples()
{
    const QString src = QStringLiteral("/usr/share/rawfish-ir/examples/");
    const QString profiles = QDir::homePath() + QStringLiteral("/.local/share/com.rawfish/rawfish-ir/device-profiles");
    QDir().mkpath(profiles);
    const struct { const char *name; QString dir; } ex[] = {
        { "rawfish-ir.conf.example", configDir() },
        { "override-hal.example.jsonc", profiles },
    };
    for (const auto &e : ex) {
        const QString from = src + QLatin1String(e.name), to = e.dir + QLatin1Char('/') + QLatin1String(e.name);
        if (!QFile::exists(from)) continue;              /* not installed (a dev build) */
        QFile::remove(to);                               /* refreshed: the version's own */
        QFile::copy(from, to);
    }
}

QString IndrawPreview::configDir() const
{
    const QString dir = QDir::homePath() + QStringLiteral("/.config/com.rawfish/rawfish-ir");
    QDir().mkpath(dir);
    return dir;
}
QVariant IndrawPreview::setting(const QString &key, const QVariant &def) const
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(key, def);
}
void IndrawPreview::setSetting(const QString &key, const QVariant &value)
{
    QSettings(settingsPath(), QSettings::IniFormat).setValue(key, value);
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

QVariantMap IndrawPreview::readJsonFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QVariantMap();
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

bool IndrawPreview::deleteFile(const QString &path)
{
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
    if (m_frame.isNull()) return false;
    QTransform t;
    t.rotate(((rotation % 360) + 360) % 360);
    const QImage upright = m_frame.transformed(t);
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
    const int size = m_buffer.size();
    const char *base = m_buffer.constData();
    for (;;) {
        if (size - pos < SF2M_HEADER)
            break;                 /* not even the shorter header yet (an SF2M
                                    * packet needs 8 bytes; it waited for 16
                                    * until ui 183: a short event sat until the
                                    * next frame arrived)                      */

        /* Which packet is at the front? SF2M (metadata/event) or SF2P (frame). */
        const bool isM = memcmp(base + pos, SF2M_MAGIC, 4) == 0;
        const bool isP = memcmp(base + pos, SF2P_MAGIC, 4) == 0;
        if (!isM && !isP) {
            /* Resync: jump to the next occurrence of either magic. */
            int nP = m_buffer.indexOf(QByteArray(SF2P_MAGIC, 4), pos + 1);
            int nM = m_buffer.indexOf(QByteArray(SF2M_MAGIC, 4), pos + 1);
            int next = (nP < 0) ? nM : (nM < 0 ? nP : qMin(nP, nM));
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

        /* Sanity-check the header before trusting the size: the product in
         * 64 bits (w = h = 65536 made w*h*3 wrap to 0 in 32 bits and a frame
         * announcing n = 0 pass, then img.copy() read 12 GB past the buffer:
         * October 2026 audit), and a frame no larger than any sensor's. */
        if (w == 0 || h == 0 || w > 16384 || h > 16384 || quint64(n) != quint64(w) * h * 3u) {
            ++pos;                 /* bad header: slip a byte and resync         */
            continue;
        }
        if (size - pos < SF2P_HEADER + int(n))
            break;                 /* the pixels have not all arrived yet       */

        newest = pos + SF2P_HEADER; newestW = w; newestH = h;   /* the latest so far */
        pos += SF2P_HEADER + int(n);
        gotFrame = true;
    }
    if (newest >= 0) {
        /* Copy the newest frame's RGB888 pixels into a QImage (the buffer's
         * bytes are about to go). */
        QImage img(reinterpret_cast<const uchar *>(base + newest), int(newestW), int(newestH), int(newestW * 3), QImage::Format_RGB888);
        m_frame = img.copy();
        if (m_frameSize != QSize(int(newestW), int(newestH))) {     /* the page sizes the viewfinder from it */
            m_frameSize = QSize(int(newestW), int(newestH));
            emit frameSizeChanged();
        }
    }
    if (pos >= size) m_buffer.clear();
    else if (pos > 0) m_buffer.remove(0, pos);
    if (gotFrame) buildOverlay();

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

void IndrawPreview::parseEvent(const QByteArray &line)
{
    QByteArray name;
    const QHash<QByteArray, QByteArray> f = kv(line, &name);
    if (name == "captured") {
        emit captureFinished();
    } else if (name == "fcrop") {
        /* The crop the next frames were taken with (sent right before them). */
        const QRectF c(f.value("x").toDouble(), f.value("y").toDouble(),
                       f.value("w").toDouble(), f.value("h").toDouble());
        if (c.width() > 0 && c.height() > 0 && c != m_frameCrop) { m_frameCrop = c; emit viewWindowChanged(); }
    } else if (name == "wbpick") {
        emit wbPicked(f.value("ok").toInt() != 0, QString::fromLatin1(f.value("gains")), QString::fromLatin1(f.value("why")));
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
    } else if (name == "caps") {
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
        m_sizeFallback = false;
        emit capsChanged();
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
        m_meterMean = f.value("mean").toDouble();
        m_meterIso = f.value("iso").toInt();
        m_meterExposureNs = f.value("exp_ns").toDouble();
        emit meterChanged();
        if (m_histogramSource == QLatin1String("raw")) takeHistogram(f);
    }
}

/* Zebra + peaking overlay, full resolution.
 * Zebra: diagonal black/white stripes where any channel is at or above
 * zebraLevel.
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
QImage computeOverlay(const QImage &frame, const OverlayParams &prm)
{
    if ((!prm.zebra && !prm.peaking) || frame.isNull()) return QImage();
    const int w = frame.width(), h = frame.height();
    if (w < 4 || h < 4) return QImage();
    QImage overlay(w, h, QImage::Format_ARGB32_Premultiplied);
    const int level = prm.level;
    const int edge = prm.edge * 3;                         /* Laplacian scale: 8x luma span */
    const QRgb zebraA = qRgba(255, 255, 255, 220), zebraB = qRgba(0, 0, 0, 220);
    const QRgb peak = qRgba(255, 40, 40, 255);

    /* Smoothed luma (2x2 box of the green channel), one byte per pixel. */
    QVector<uchar> luma(w * h);
    QVector<short> lap(w * h);
    for (int y = 0; y < h; ++y) {
        const uchar *r0 = frame.constScanLine(y);
        const uchar *r1 = frame.constScanLine(y < h - 1 ? y + 1 : y);
        uchar *out = luma.data() + y * w;
        for (int x = 0; x < w; ++x) {
            const int x1 = x < w - 1 ? x + 1 : x;
            out[x] = uchar((r0[3 * x + 1] + r0[3 * x1 + 1] + r1[3 * x + 1] + r1[3 * x1 + 1]) >> 2);
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
    for (int y = 0; y < h; ++y) {
        const uchar *px = frame.constScanLine(y);
        QRgb *out = reinterpret_cast<QRgb *>(overlay.scanLine(y));
        const short *l = lap.constData() + y * w;
        const short *lu = lap.constData() + (y > 0 ? y - 1 : 0) * w;
        const short *ld = lap.constData() + (y < h - 1 ? y + 1 : y) * w;
        for (int x = 0; x < w; ++x) {
            QRgb v = 0;
            if (prm.peaking && x > 1 && x < w - 2 && y > 1 && y < h - 2) {
                const int a = l[x];
                if (a > edge && a >= l[x - 1] && a >= l[x + 1] && a >= lu[x] && a >= ld[x]) v = peak;
            }
            if (prm.zebra && (px[3 * x] >= level || px[3 * x + 1] >= level || px[3 * x + 2] >= level))
                v = (((x + y) >> 3) & 1) ? zebraA : zebraB;
            out[x] = v;
        }
    }
    /* Line width: thicken the 1-pixel peaking marks by (width - 1) pixels to
     * the right and below (a cheap dilation; width from the device override). */
    if (prm.peaking && prm.width > 1) {
        const int extra = qMin(prm.width, 6) - 1;
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

void OverlayWorker::compute(const QImage &frame, const OverlayParams &p)
{
    emit done(computeOverlay(frame, p));
}

/* Hands the latest frame to the worker, or notes that one waits. Nothing
 * to draw: the overlay is cleared at once. */
void IndrawPreview::buildOverlay()
{
    if ((!m_zebra && !m_peaking) || m_frame.isNull()) { m_overlay = QImage(); m_overlayPending = false; return; }
    if (m_overlayBusy) { m_overlayPending = true; return; }
    OverlayParams p; p.zebra = m_zebra; p.peaking = m_peaking; p.level = m_zebraLevel; p.edge = m_peakEdge; p.width = m_peakWidth;
    m_overlayBusy = true;
    QMetaObject::invokeMethod(m_overlayWorker, "compute", Qt::QueuedConnection, Q_ARG(QImage, m_frame), Q_ARG(OverlayParams, p));
}
void IndrawPreview::onOverlayDone(const QImage &overlay)
{
    m_overlayBusy = false;
    if (m_zebra || m_peaking) { m_overlay = overlay; update(); }
    if (m_overlayPending) { m_overlayPending = false; buildOverlay(); }
}
int IndrawPreview::peakWidth() const { return m_peakWidth; }
void IndrawPreview::setPeakWidth(int w) { w = qBound(1, w, 6); if (m_peakWidth == w) return; m_peakWidth = w; emit aidsChanged(); buildOverlay(); update(); }

bool IndrawPreview::zebra() const { return m_zebra; }
void IndrawPreview::setZebra(bool on) { if (m_zebra == on) return; m_zebra = on; emit aidsChanged(); buildOverlay(); update(); }
bool IndrawPreview::peaking() const { return m_peaking; }
void IndrawPreview::setPeaking(bool on) { if (m_peaking == on) return; m_peaking = on; emit aidsChanged(); buildOverlay(); update(); }
int IndrawPreview::zebraLevel() const { return m_zebraLevel; }
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

QSGNode *IndrawPreview::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    delete oldNode;                /* simplest correct approach: rebuild         */
    if (m_frame.isNull() || width() <= 0 || height() <= 0 || !window())
        return nullptr;

    /* Upload the frame as a GPU texture. */
    QSGTexture *texture = window()->createTextureFromImage(m_frame);
    if (!texture)
        return nullptr;
    texture->setFiltering(QSGTexture::Linear);

    QSGSimpleTextureNode *node = new QSGSimpleTextureNode;
    node->setOwnsTexture(true);
    node->setTexture(texture);

    /* Source rect: the view window (array-normalised) mapped into this
     * frame through the crop it was really taken with, kept inside the frame.
     * Whatever the HAL's latency in applying a new crop, the part of the
     * scene shown is the one asked for: the display magnification is derived
     * from the frame, never predicted. */
    const QSizeF imageSize = m_frame.size();
    const QRectF fc = m_frameCrop.width() > 0 ? m_frameCrop : QRectF(0, 0, 1, 1);
    const qreal sw = qMin<qreal>(1.0, m_viewWindow.width() / fc.width()) * imageSize.width();
    const qreal sh = qMin<qreal>(1.0, m_viewWindow.height() / fc.height()) * imageSize.height();
    const qreal sx = qBound<qreal>(0, (m_viewWindow.x() - fc.x()) / fc.width() * imageSize.width(), imageSize.width() - sw);
    const qreal sy = qBound<qreal>(0, (m_viewWindow.y() - fc.y()) / fc.height() * imageSize.height(), imageSize.height() - sh);
    const QRectF sourceRect(sx, sy, sw, sh);
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

    node->setRect(drawRect);
    node->setSourceRect(sourceRect);

    /* The aids overlay, if any, drawn over the frame with the same mapping
     * (its pixels are the frame's at half resolution). */
    if (!m_overlay.isNull()) {
        QSGTexture *ovTex = window()->createTextureFromImage(m_overlay);
        if (ovTex) {
            ovTex->setFiltering(QSGTexture::Nearest);
            QSGSimpleTextureNode *ov = new QSGSimpleTextureNode;
            ov->setOwnsTexture(true);
            ov->setTexture(ovTex);
            ov->setRect(drawRect);
            ov->setSourceRect(sourceRect);                /* same resolution as the frame */
            node->appendChildNode(ov);
        }
    }
    return node;
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
