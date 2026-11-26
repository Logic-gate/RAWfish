/* The viewfinder without a copy (zero copy, branch zero_copy).
 *
 * The engine keeps each frame in the camera's own buffer and names it on
 * the stream (an SF2H packet: the buffer's id). Each buffer's gralloc
 * handle comes once over a unix socket ("HWB2": fds with SCM_RIGHTS, size,
 * stride, format, usage, id); it is imported here with libhybris
 * (eglHybrisCreateRemoteBuffer, as a Wayland compositor does) into an
 * EGLImage bound to an external texture, kept for as long as the engine's
 * connection lasts. A frame shown, the one before is released ("REL1" +
 * its id) and goes back to the camera. hwprobe (0436) showed the import
 * works on the JP2 (15 Nov 2026).
 *
 * Threads: the socket is read on the GUI thread (a notifier, and drain()
 * when a frame names a buffer not seen yet); the GL objects are made, used
 * and destroyed on the render thread only (texture()); a failure is kept
 * (failed()) for the item to fall back on the YUV pipe. */
#ifndef RAWFISH_IR_HWFRAMES_H
#define RAWFISH_IR_HWFRAMES_H

#include <QObject>
#include <QMutex>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>

class QSocketNotifier;

class HwFrames : public QObject
{
    Q_OBJECT
public:
    explicit HwFrames(QObject *parent = nullptr);
    ~HwFrames() override;
    bool listen(const QString &path);
    QString path() const { return m_path; }
    /* GUI thread: reads what waits on the socket now. */
    void drain();
    /* Any thread: the handle of buffer `id` has come. */
    bool known(quint64 id);
    /* Render thread, GL context current: the external texture of buffer
     * `id` (imported the first time); 0 on failure (then failed()). */
    unsigned texture(quint64 id);
    /* Render thread: the engine may take buffer `id` back. */
    void release(quint64 id);
    bool failed() const { return m_failed.load(); }
    void clearFailed() { m_failed.store(false); }
    /* GUI thread: the lines to log since the last call. */
    QStringList takeNotes();

private:
    void onAccept();
    void closeClient();
    void note(const QString &line);

    struct Handle {
        quint32 width = 0, height = 0, stride = 0, format = 0;
        quint64 usage = 0;
        QVector<int> ints, fds;
    };
    struct Imported { void *buffer = nullptr, *image = nullptr; unsigned tex = 0; };

    QString m_path;
    int m_listenFd = -1, m_clientFd = -1;
    QSocketNotifier *m_listenNotifier = nullptr, *m_clientNotifier = nullptr;
    QMutex m_mutex;                    /* m_handles, m_known, m_notes, m_clientFd, m_generation */
    QHash<quint64, Handle> m_handles;  /* received, not imported yet */
    QSet<quint64> m_known;
    QStringList m_notes;
    int m_generation = 0;              /* a new engine connection: the old imports go */
    /* render thread only */
    QHash<quint64, Imported> m_imported;
    int m_importedGeneration = 0;
    /* The GL context the imports' textures belong to. The scene graph can
     * lose its context (the app in the background a while) and make a new
     * one: the old texture names then mean nothing, and the same numbers are
     * soon Qt's own textures (the icon atlas, the glyph cache). Deleting or
     * binding them by those numbers broke Qt's: black icons, text in white
     * blocks. The imports are dropped with their context (render thread). */
    class QOpenGLContext *m_ctx = nullptr;
    void dropImports(bool deleteTextures);
    std::atomic<bool> m_failed { false };
};

#endif
