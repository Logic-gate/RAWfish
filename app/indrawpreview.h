/*
 * indrawpreview.h -- the viewfinder, a QQuickItem shown in QML.
 *
 * It launches the indraw engine in 'stream' mode as a child process, reads the
 * continuous SF2P frame stream from its stdout, turns each frame into a QImage,
 * and draws it on the GPU. Commands (focus, capture, exposure...) are written to
 * the process's stdin; the engine's JSON replies go to its log, not here.
 *
 * This is the phase-3 skeleton: display the live viewfinder. Overlays (peaking,
 * zebra), the histogram and the controls come later. Note: unlike RAWfish, there
 * is NO render-exposure display gain -- the viewfinder shows the real frame.
 */
#ifndef INDRAWPREVIEW_H
#define INDRAWPREVIEW_H

#include <QImage>
#include <QProcess>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVariant>
#include <QTimer>
#include <QThread>

/* The zebra / peaking overlay is computed off the UI thread: OverlayWorker
 * lives in its own QThread, takes the latest frame and the aids' settings,
 * and hands the ARGB overlay back (ui 167). While it works, newer frames
 * only replace the one waiting: it never falls behind, it skips. */
struct OverlayParams { bool zebra = false, peaking = false; int level = 250, edge = 40, width = 2; };
class OverlayWorker : public QObject
{
    Q_OBJECT
public slots:
    void compute(const QImage &frame, const OverlayParams &p);
signals:
    void done(const QImage &overlay);
};
/* The pure computation (no shared state: safe on any thread). */
QImage computeOverlay(const QImage &frame, const OverlayParams &p);

class IndrawPreview : public QQuickItem
{
    Q_OBJECT
    /* active: start/stop the engine and the stream.                           */
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    /* running: true once the engine process is up and frames are arriving.     */
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    /* cameraId / previewSize / captureSize: what to open the engine with.      */
    Q_PROPERTY(QString cameraId READ cameraId WRITE setCameraId NOTIFY cameraIdChanged)
    Q_PROPERTY(QSize previewSize READ previewSize WRITE setPreviewSize NOTIFY previewSizeChanged)
    Q_PROPERTY(QSize captureSize READ captureSize WRITE setCaptureSize NOTIFY captureSizeChanged)
    /* The size of the frames the engine streams (sensor orientation): their
     * proportions are the photo's; the page shapes the viewfinder on them. */
    Q_PROPERTY(QSize frameSize READ frameSize NOTIFY frameSizeChanged)
    /* fill: true = cover the item (crop), false = fit the whole frame inside.  */
    Q_PROPERTY(bool fill READ fill WRITE setFill NOTIFY fillChanged)
    /* viewWindow: the part of the SENSOR to show, normalised in the active
     * array (x, y, w, h; default the whole array). Each frame comes with the
     * crop it was really taken with (engine "fcrop", frameCrop); the item
     * maps the view window into THAT frame at paint time, so zoom, loupe and
     * pan stay exact even while the HAL is still catching up with a new crop
     * request (no zoom/unzoom jumps). The photo is never affected. */
    Q_PROPERTY(QRectF viewWindow READ viewWindow WRITE setViewWindow NOTIFY viewWindowChanged)
    Q_PROPERTY(QRectF frameCrop READ frameCrop NOTIFY viewWindowChanged)
    /* sensorOrientation: the camera's mounting angle in degrees clockwise
     * (0/90/180/270). The JP2 is 90. QML uses it, together with the screen
     * orientation, to rotate the image upright and to map touch points back to
     * sensor coordinates. Default 90; later read live from the engine. */
    Q_PROPERTY(int sensorOrientation READ sensorOrientation WRITE setSensorOrientation NOTIFY sensorOrientationChanged)
    /* displayRotation: the degrees (0/90/180/270) the QML rotates this item by,
     * so fit math can account for the swapped w/h at 90/270. */
    Q_PROPERTY(int displayRotation READ displayRotation WRITE setDisplayRotation NOTIFY displayRotationChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    /* ~/.config/com.indraw/indraw (settings, simple.json). */
    Q_PROPERTY(QString configDir READ configDir CONSTANT)
    /* Where captures go and how they are named: set by the QML from dconf
     * (/apps/indraw/output, edited in the system Settings app) through
     * setOutput(); absolute folders, "~/" for HOME; defaults
     * ~/Pictures/indRAW and "photo". Used at every engine start. */
    Q_PROPERTY(QString jpgDir READ jpgDir NOTIFY outputChanged)
    Q_PROPERTY(QString dngDir READ dngDir NOTIFY outputChanged)
    Q_PROPERTY(QString capturePrefix READ capturePrefix NOTIFY outputChanged)
    /* True between the stack command and its stack_done: the engine is kept
     * running through inactivity (screen off) until the stack ends. */
    Q_PROPERTY(bool stacking READ stacking NOTIFY stackingChanged)

    /* caps: the sensor limits from the engine's SF2M "caps" line (isoMin,
     * isoMax, isoAnalogMax, expMinNs, expMaxNs, shutterMaxNs, evMin, evMax,
     * evStep (EV per step), orientation, black, white). Empty until streaming. */
    Q_PROPERTY(QVariantMap caps READ caps NOTIFY capsChanged)
    /* live*: the preview's real exposure, from the SF2M "ae" line (~10 Hz):
     * the HAL's choice in auto, ours in manual. Nanoseconds as a double. */
    Q_PROPERTY(int liveIso READ liveIso NOTIFY liveChanged)
    Q_PROPERTY(qreal liveExposureNs READ liveExposureNs NOTIFY liveChanged)
    /* meter*: the RAW16 exposure meter (SF2M "meter"): linear centre-weighted
     * mean (0 black .. 1 white), the ISO/exposure of that frame, clipping %. */
    Q_PROPERTY(qreal meterMean READ meterMean NOTIFY meterChanged)
    Q_PROPERTY(int meterIso READ meterIso NOTIFY meterChanged)
    Q_PROPERTY(qreal meterExposureNs READ meterExposureNs NOTIFY meterChanged)
    /* vfMean: the same measure taken on the viewfinder frames (8-bit, gamma
     * removed with a nominal 2.2), for the jpg types, where the HAL's JPEG
     * shares the preview's processing. Refined by measurement later. */
    Q_PROPERTY(qreal vfMean READ vfMean NOTIFY vfMeterChanged)
    /* The white balance in force, estimated by the engine from the HAL's
     * colour gains and the sensor's calibration: correlated colour
     * temperature in kelvin (0 = unknown) and Duv (+ = green side). */
    Q_PROPERTY(int wbCct READ wbCct NOTIFY liveChanged)
    Q_PROPERTY(qreal wbDuv READ wbDuv NOTIFY liveChanged)
    /* The controller's exposure state ("xpstate" event, docs/ARCHITECTURE.md
     * step 3): what the wheels and indicators show. Keys: iso, expNs,
     * isoManual, shutterManual, fullAuto, fullManual, fixed, spot, ev,
     * offset, offsetValid, ev100 (NaN when unknown). */
    Q_PROPERTY(QVariantMap exposureState READ exposureState NOTIFY exposureStateChanged)
    /* The controller's focus state ("fcstate" event, step 4): mode (0 cont,
     * 1 once, 2 manual), diopters, circle (0 none, 1 searching, 4 sharp,
     * 5 unsure, 6 failed, 7 sun), searching, light. */
    Q_PROPERTY(QVariantMap focusState READ focusState NOTIFY focusStateChanged)
    /* The controller's view geometry ("vwstate", step 5): zoom, loupe, the
     * view window (array-normalised, applied to viewWindow by the item
     * itself), and the reference point in display-window coordinates
     * (hasRef, refU, refV). */
    Q_PROPERTY(QVariantMap viewState READ viewState NOTIFY viewStateChanged)
    /* Histogram: 64 bins per colour + clipped-pixel percentages, from the RAW
     * meter ("raw") or the viewfinder frames ("vf"), chosen by histogramSource. */
    Q_PROPERTY(QString histogramSource READ histogramSource WRITE setHistogramSource NOTIFY histogramSourceChanged)
    Q_PROPERTY(QVariantList histR READ histR NOTIFY histogramChanged)
    Q_PROPERTY(QVariantList histG READ histG NOTIFY histogramChanged)
    Q_PROPERTY(QVariantList histB READ histB NOTIFY histogramChanged)
    /* The spot's own histogram (the light reference's box), empty without one. */
    Q_PROPERTY(QVariantList spotHistR READ spotHistR NOTIFY histogramChanged)
    Q_PROPERTY(QVariantList spotHistG READ spotHistG NOTIFY histogramChanged)
    Q_PROPERTY(QVariantList spotHistB READ spotHistB NOTIFY histogramChanged)
    Q_PROPERTY(qreal histClipLow READ histClipLow NOTIFY histogramChanged)
    Q_PROPERTY(qreal histClipHigh READ histClipHigh NOTIFY histogramChanged)
    /* Preview aids, drawn over the frame (display only): zebra stripes on
     * pixels at/over zebraLevel (0..255), focus peaking on strong edges. */
    Q_PROPERTY(bool zebra READ zebra WRITE setZebra NOTIFY aidsChanged)
    Q_PROPERTY(bool peaking READ peaking WRITE setPeaking NOTIFY aidsChanged)
    Q_PROPERTY(int zebraLevel READ zebraLevel WRITE setZebraLevel NOTIFY aidsChanged)
    Q_PROPERTY(int peakEdge READ peakEdge WRITE setPeakEdge NOTIFY aidsChanged)
    Q_PROPERTY(int peakWidth READ peakWidth WRITE setPeakWidth NOTIFY aidsChanged)

public:
    explicit IndrawPreview(QQuickItem *parent = nullptr);
    ~IndrawPreview() override;

    bool active() const;
    void setActive(bool active);

    bool running() const;

    QString cameraId() const;
    void setCameraId(const QString &id);

    QSize previewSize() const;
    void setPreviewSize(const QSize &size);

    QSize captureSize() const;
    void setCaptureSize(const QSize &size);

    bool fill() const;
    void setFill(bool fill);

    QRectF viewWindow() const;
    void setViewWindow(const QRectF &w);
    QRectF frameCrop() const;

    int sensorOrientation() const;
    void setSensorOrientation(int degrees);

    int displayRotation() const;
    void setDisplayRotation(int degrees);
    QString errorString() const;
    QString configDir() const;
    bool stacking() const;
    QString jpgDir() const;
    QString dngDir() const;
    QString capturePrefix() const;

    QVariantMap caps() const;
    int liveIso() const;
    qreal liveExposureNs() const;
    qreal meterMean() const;
    int meterIso() const;
    qreal meterExposureNs() const;
    qreal vfMean() const;
    int wbCct() const;
    qreal wbDuv() const;
    QVariantMap exposureState() const;
    QSize frameSize() const;
    QVariantMap focusState() const;
    QVariantMap viewState() const;
    QString histogramSource() const;
    void setHistogramSource(const QString &source);
    QVariantList histR() const;
    QVariantList histG() const;
    QVariantList histB() const;
    qreal histClipLow() const;
    QVariantList spotHistR() const;
    QVariantList spotHistG() const;
    QVariantList spotHistB() const;
    qreal histClipHigh() const;
    bool zebra() const;
    void setZebra(bool on);
    bool peaking() const;
    void setPeaking(bool on);
    int zebraLevel() const;
    void setZebraLevel(int level);
    int peakEdge() const;
    void setPeakEdge(int edge);
    int peakWidth() const;
    void setPeakWidth(int w);

    /* Sends one command line to the engine's stdin (e.g. "focus 0.5m",
     * "jpeg photo off_noise"). Available from QML. */
    Q_INVOKABLE void sendCommand(const QString &command);
    /* Restarts the engine (new sizes take effect); no-op while inactive. */
    Q_INVOKABLE void restart();
    /* Asks the engine to stop a running stack at its next sub-frame (SIGUSR1):
     * it writes what it accumulated, labelled with the exposure reached. */
    Q_INVOKABLE void abortCapture();
    /* Runs "indraw probe" (a separate, short engine process: it only reads the
     * cameras' characteristics) to write the device profile files; returns
     * true when it exited cleanly. Requested from the system Settings page. */
    Q_INVOKABLE bool runProbe();
    /* Grabs the app's own window and writes it as a PNG at `path` (creating
     * the directory). Used for the volume double-press screenshot: inside the
     * Sailjail sandbox the app cannot reach lipstick's screenshot service, but
     * it can write under ~/Pictures. Returns true on success. */
    Q_INVOKABLE bool saveScreenshot(const QString &path);
    /* Saves the latest viewfinder frame, upright (rotated by `rotation`
     * degrees clockwise), as a JPEG at `path`: the preview written next to a
     * DNG so galleries (ours, the system's) can show something for it. */
    Q_INVOKABLE bool saveFramePreview(const QString &path, int rotation);
    /* Turns the engine's NAME_vf.ppm (a half-size RGB render of the DNG's own
     * pixels) into NAME_vf.jpg, rotated upright by the DNG's orientation
     * (TIFF 1..8), and deletes the .ppm. Returns true on success. */
    Q_INVOKABLE bool convertDngPreview(const QString &ppmPath, int orientation);
    /* UI preferences, persisted in ~/.config/com.indraw/indraw/indraw.conf. */
    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &def) const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);
    /* Output settings (validated here; invalid values fall back to the
     * defaults): the prefix charset is the engine's, folders are absolute. */
    Q_INVOKABLE void setOutput(const QString &prefix, const QString &jpgDir, const QString &dngDir);
    /* Deletes a capture (the gallery's delete action). */
    Q_INVOKABLE bool deleteFile(const QString &path);
    /* Reads a JSON object file (e.g. the simple-mode layout), {} if absent. */
    Q_INVOKABLE QVariantMap readJsonFile(const QString &path);
    /* Copies the commented reference files (installed under
     * /usr/share/indraw/examples) next to the real ones: indraw.conf.example
     * in the config dir, override-hal.example.jsonc in device-profiles. The
     * copies are refreshed at each start; the real files are never touched. */
    Q_INVOKABLE void installExamples();

signals:
    void activeChanged();
    void runningChanged();
    void cameraIdChanged();
    void previewSizeChanged();
    void captureSizeChanged();
    void fillChanged();
    void viewWindowChanged();
    void sensorOrientationChanged();
    void displayRotationChanged();
    void errorStringChanged();
    /* Emitted when the engine reports a capture finished (SF2M "captured"),
     * before the file is written -- the UI flashes on this. */
    void captureFinished();
    /* Emitted when the engine reports a file on disk (SF2M "written"):
     * kind = jpeg | dng | raw16 | stack, path = the file. */
    void fileWritten(const QString &kind, const QString &path);
    /* A stack's progress: frame of n, elapsed ms (SF2M "progress"). */
    void stackStarted(int n);                  /* the engine began a stack of n frames */
    /* A white-balance pick's outcome: ok with the gains "R,Ge,Go,B", or the
     * reason it failed (clipped, dark, timeout, no_raw, apply). */
    void wbPicked(bool ok, const QString &gains, const QString &why);
    /* The lens-shading calibration (engine 'shading calibrate'): its
     * progress (captures done / total), the exposure it settled on, and
     * its outcome (the file written, or why it failed). */
    void calProgress(int done, int total);
    void calExposure(qint64 exposureNs, int iso);
    void calDone(bool ok, const QString &path, const QString &why);
    void stackProgress(int frame, int n, int elapsedMs);
    /* A stack ended: frames added of n, aborted, total ms (SF2M "stack_done"). */
    void stackDone(int frames, int n, bool aborted, int ms);
    void stackingChanged();
    void outputChanged();
    void capsChanged();
    void liveChanged();
    void meterChanged();
    void vfMeterChanged();
    void exposureStateChanged();
    void frameSizeChanged();
    void focusStateChanged();
    void viewStateChanged();
    void histogramSourceChanged();
    void histogramChanged();
    void aidsChanged();

protected:
    /* Qt calls this on the render thread to (re)build what is drawn. */
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private slots:
    void onReadyRead();            /* bytes available on the engine's stdout    */
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int code, QProcess::ExitStatus status);

private:
    void start();                  /* launch the engine in stream mode          */
    void stop(bool wait = false);  /* quit the engine; wait only when closing   */
    void parseFrames();            /* pull whole SF2P frames out of m_buffer     */
    void parseEvent(const QByteArray &line);   /* one SF2M line                 */
    void scheduleRestart();        /* after an unexpected engine exit            */
    void buildOverlay();           /* asks the worker for m_frame's overlay      */
    void onOverlayDone(const QImage &overlay);
    void setRunning(bool running);
    void setError(const QString &error);

    QProcess *m_process = nullptr; /* the indraw engine                         */
    QByteArray m_buffer;           /* accumulates stdout until a full frame      */
    QImage m_frame;                /* the latest decoded frame (what we draw)    */

    bool m_active = false;
    bool m_running = false;
    QString m_cameraId = QStringLiteral("0");
    QSize m_previewSize = QSize(1280, 960);
    QSize m_captureSize = QSize(4096, 3072);
    bool m_fill = true;
    QRectF m_viewWindow{0, 0, 1, 1};   /* what to show, array-normalised         */
    QRectF m_frameCrop{0, 0, 1, 1};    /* the next frames' real crop ("fcrop")    */
    int m_sensorOrientation = 90;  /* JP2 default; see the property doc above   */
    int m_displayRotation = 0;     /* QML item rotation, for fit math           */
    QString m_errorString;
    int m_restarts = 0;            /* unexpected exits restarted in a row        */
    bool m_stacking = false;       /* a stack is running in the engine          */
    QString m_jpgDir, m_dngDir, m_prefix;   /* output settings in force         */
    bool m_stopWanted = false;     /* setActive(false) arrived during a stack   */
    qint64 m_lastRestartMs = 0;

    QVariantMap m_caps;
    /* The live pair, from the engine's "ae" line: the deck's fallback before
     * the controller's first state (the AE/AF states and the lens position
     * are the controller's business since steps 3-4). */
    int m_liveIso = -1;
    qreal m_liveExposureNs = -1;
    int m_wbCct = 0;
    qreal m_wbDuv = 0;
    qreal m_meterMean = -1, m_meterExposureNs = -1;
    int m_meterIso = -1;
    qreal m_vfMean = -1;
    QVariantMap m_exposureState;
    QSize m_frameSize;
    QVariantMap m_focusState;
    QVariantMap m_viewState;
    QString m_histogramSource = QStringLiteral("vf");
    QVariantList m_histR, m_histG, m_histB;
    QVariantList m_spotHistR, m_spotHistG, m_spotHistB;
    void takeHistogram(const QHash<QByteArray, QByteArray> &f);
    qreal m_histClipLow = 0, m_histClipHigh = 0;
    bool m_zebra = false, m_peaking = false;
    int m_zebraLevel = 250;
    int m_peakEdge = 40;
    int m_peakWidth = 2;
    QImage m_overlay;              /* half-resolution ARGB aids, or null         */
    QThread *m_overlayThread = nullptr;
    OverlayWorker *m_overlayWorker = nullptr;
    bool m_overlayBusy = false;    /* the worker is on a frame                   */
    bool m_overlayPending = false; /* a newer frame waits for it                 */
};

Q_DECLARE_METATYPE(OverlayParams)

#endif /* INDRAWPREVIEW_H */
