/* hwprobe (branch zero_copy): can the camera's own viewfinder buffers reach
 * this app's GPU without a copy?
 *
 * The engine (rawfish-ir-cli, Android side) sends, for a few frames, the
 * gralloc handle of each viewfinder buffer (fds with SCM_RIGHTS, ints,
 * size, stride, format, usage) to a unix socket this object listens on
 * (engine command "hwprobe PATH N"). Each one is imported here with
 * libhybris -- eglHybrisCreateRemoteBuffer, what a Wayland compositor does
 * with a client's buffer -- as an EGLImage, bound to an external texture,
 * drawn into a small framebuffer and read back: the mean and spread of
 * what came out tell whether the GPU saw the picture, and the first good
 * one is saved as a PNG beside the log. Each step is reported in the log
 * ("# hwprobe app: ..."). An answer byte lets the engine release the
 * image. Nothing is drawn on the screen; the viewfinder goes on as before.
 *
 * Asked by rawfish-ir.conf: hwProbe=N (frames, 1-20; 0 or absent: off),
 * hwProbeGpuUsage=1 (the engine's viewfinder buffers allocated for GPU
 * sampling too). Read at each engine start. */
#ifndef RAWFISH_IR_HWPROBE_H
#define RAWFISH_IR_HWPROBE_H

#include <QObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

class QSocketNotifier;
class QQuickWindow;

class HwProbe : public QObject
{
    Q_OBJECT
public:
    explicit HwProbe(QObject *parent = nullptr);
    ~HwProbe() override;
    /* Listens on `path` (an old socket file there is removed). */
    bool listen(const QString &path, const QString &pngDir);
    QString path() const { return m_path; }
    /* Render thread, GL context current (updatePaintNode): imports and
     * draws the buffer received, if any, then answers the engine. */
    void renderPending(QQuickWindow *window);
    /* GUI thread: the lines to log since the last call. */
    QStringList takeResults();

signals:
    void bufferArrived();          /* a buffer waits for renderPending()      */

private:
    void onAccept();
    void onReadable();
    void closeClient();
    void addResult(const QString &line);

    struct Pending {
        quint32 seq = 0, width = 0, height = 0, stride = 0, format = 0, layers = 0;
        quint64 usage = 0;
        QVector<int> ints;
        QVector<int> fds;
    };
    QString m_path, m_pngDir;
    int m_listenFd = -1, m_clientFd = -1;
    QSocketNotifier *m_listenNotifier = nullptr, *m_clientNotifier = nullptr;
    QMutex m_mutex;                /* m_pending, m_results, m_clientFd's use from the render thread */
    bool m_hasPending = false;
    Pending m_pending;
    QStringList m_results;
    bool m_pngSaved = false;
};

#endif
