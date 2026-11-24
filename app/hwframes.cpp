#include "hwframes.h"
#include "hwegl.h"

#include <QSocketNotifier>
#include <QFile>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The handle message (android/rawfish-ir.c, hwb_msg_t). */
struct HwbMsg {
    char magic[4];
    quint32 width, height, stride, format, layers;
    quint64 usage, id;
    qint32 nfds, nints;
    qint32 ints[64];
};

HwFrames::HwFrames(QObject *parent) : QObject(parent) {}

HwFrames::~HwFrames()
{
    closeClient();
    if (m_listenFd >= 0) ::close(m_listenFd);
    if (!m_path.isEmpty()) QFile::remove(m_path);
    QMutexLocker l(&m_mutex);
    for (const Handle &h : m_handles) for (int fd : h.fds) ::close(fd);
    /* the GL objects (m_imported) belong to the render thread's context,
     * gone with the window at this point: left to the process's end */
}

bool HwFrames::listen(const QString &path)
{
    if (m_listenFd >= 0) return true;
    m_path = path;
    const QByteArray p = QFile::encodeName(path);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (size_t(p.size()) >= sizeof a.sun_path) { note(QStringLiteral("error=socket path too long")); return false; }
    memcpy(a.sun_path, p.constData(), size_t(p.size()));
    ::unlink(p.constData());
    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0 || ::bind(m_listenFd, reinterpret_cast<struct sockaddr *>(&a), sizeof a) != 0 || ::listen(m_listenFd, 2) != 0) {
        note(QStringLiteral("error=listen errno=%1").arg(errno));
        if (m_listenFd >= 0) ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }
    m_listenNotifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_listenNotifier, &QSocketNotifier::activated, this, &HwFrames::onAccept);
    return true;
}

void HwFrames::onAccept()
{
    const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    closeClient();                     /* a new engine: its buffers are new */
    {
        QMutexLocker l(&m_mutex);
        m_clientFd = fd;
        m_generation++;
        for (const Handle &h : m_handles) for (int f : h.fds) ::close(f);
        m_handles.clear();
        m_known.clear();
    }
    m_clientNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_clientNotifier, &QSocketNotifier::activated, this, &HwFrames::drain);
}

void HwFrames::closeClient()
{
    delete m_clientNotifier;
    m_clientNotifier = nullptr;
    QMutexLocker l(&m_mutex);
    if (m_clientFd >= 0) { ::close(m_clientFd); m_clientFd = -1; }
}

void HwFrames::drain()
{
    for (;;) {
        int fd;
        {
            QMutexLocker l(&m_mutex);
            fd = m_clientFd;
        }
        if (fd < 0) return;
        HwbMsg m;
        char cbuf[CMSG_SPACE(sizeof(int) * 16)];
        struct iovec iov = { &m, sizeof m };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        const ssize_t n = ::recvmsg(fd, &mh, MSG_CMSG_CLOEXEC | MSG_WAITALL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { closeClient(); return; }      /* the engine went away */
        QVector<int> fds;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                const int k = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                const int *p = reinterpret_cast<const int *>(CMSG_DATA(c));
                for (int i = 0; i < k; ++i) fds << p[i];
            }
        if (n != ssize_t(sizeof m) || memcmp(m.magic, "HWB2", 4) || m.nints < 0 || m.nints > 64 || m.nfds != fds.size()) {
            note(QStringLiteral("error=bad message bytes=%1 fds=%2").arg(n).arg(fds.size()));
            for (int f : fds) ::close(f);
            m_failed.store(true);
            closeClient();
            return;
        }
        Handle h;
        h.width = m.width; h.height = m.height; h.stride = m.stride; h.format = m.format; h.usage = m.usage;
        for (int i = 0; i < m.nints; ++i) h.ints << m.ints[i];
        h.fds = fds;
        QMutexLocker l(&m_mutex);
        if (m_handles.contains(m.id)) for (int f : m_handles[m.id].fds) ::close(f);
        m_handles.insert(m.id, h);
        m_known.insert(m.id);
        if (m_known.size() == 1)
            m_notes << QStringLiteral("first buffer %1x%2 stride=%3 format=0x%4 usage=0x%5")
                       .arg(h.width).arg(h.height).arg(h.stride).arg(h.format, 0, 16).arg(h.usage, 0, 16);
    }
}

bool HwFrames::known(quint64 id)
{
    QMutexLocker l(&m_mutex);
    return m_known.contains(id);
}

void HwFrames::note(const QString &line)
{
    QMutexLocker l(&m_mutex);
    m_notes << line;
}

QStringList HwFrames::takeNotes()
{
    QMutexLocker l(&m_mutex);
    QStringList r = m_notes;
    m_notes.clear();
    return r;
}

unsigned HwFrames::texture(quint64 id)
{
    EglFns &f = egl();
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx || !f.ok) {
        if (!m_failed.exchange(true)) note(QStringLiteral("error=%1").arg(ctx ? QStringLiteral("missing ") + f.missing : QStringLiteral("no GL context")));
        return 0;
    }
    QOpenGLFunctions *gl = ctx->functions();
    const EGLDisplay_ dpy = f.getCurrentDisplay();
    if (ctx != m_ctx) {
        /* another context than the imports': their texture names are not
         * this one's -- forget them without deleting (see m_ctx) */
        dropImports(false);
        m_ctx = ctx;
        connect(ctx, &QOpenGLContext::aboutToBeDestroyed, this, [this, ctx]() {
            if (m_ctx == ctx) { dropImports(QOpenGLContext::currentContext() == ctx); m_ctx = nullptr; }
        }, Qt::DirectConnection);
    }
    Handle h;
    bool have = false;
    {
        QMutexLocker l(&m_mutex);
        if (m_generation != m_importedGeneration) {   /* a new engine: drop the old buffers */
            m_importedGeneration = m_generation;
            l.unlock();
            dropImports(true);
            l.relock();
        }
        if (m_imported.contains(id)) return m_imported.value(id).tex;
        if (m_handles.contains(id)) { h = m_handles.take(id); have = true; }
    }
    if (!have) return 0;               /* not come yet: the frame is skipped */
    /* the handle's fds are closed by the import (native_handle_close) */
    EGLClientBuffer_ buf = nullptr;
    if (!f.createRemoteBuffer(EGLint_(h.width), EGLint_(h.height), EGLint_(quint32(h.usage)), EGLint_(h.format), EGLint_(h.stride),
                              h.ints.size(), h.ints.data(), h.fds.size(), h.fds.data(), &buf) || !buf) {
        if (!m_failed.exchange(true)) note(QStringLiteral("error=eglHybrisCreateRemoteBuffer format=0x%1").arg(h.format, 0, 16));
        return 0;
    }
    const EGLint_ attrs[] = { kEglImagePreserved, 1, kEglNone };
    EGLImage_ image = f.createImage(dpy, nullptr, kEglNativeBufferAndroid, buf, attrs);
    if (!image) {
        if (!m_failed.exchange(true)) note(QStringLiteral("error=eglCreateImageKHR eglError=0x%1").arg(f.getError(), 0, 16));
        f.releaseNativeBuffer(buf);
        return 0;
    }
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(kGlTextureExternal, tex);
    gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (gl->glGetError() != GL_NO_ERROR) {}
    f.imageTargetTexture(kGlTextureExternal, image);
    const GLenum e = gl->glGetError();
    gl->glBindTexture(kGlTextureExternal, 0);
    if (e != GL_NO_ERROR) {
        if (!m_failed.exchange(true)) note(QStringLiteral("error=glEGLImageTargetTexture2DOES glError=0x%1").arg(e, 0, 16));
        gl->glDeleteTextures(1, &tex);
        f.destroyImage(dpy, image);
        f.releaseNativeBuffer(buf);
        return 0;
    }
    Imported im;
    im.buffer = buf; im.image = image; im.tex = tex;
    m_imported.insert(id, im);
    return tex;
}

/* Drops the imported buffers: their textures deleted when they belong to
 * the current context, otherwise only forgotten (their names may already be
 * someone else's); the EGL images and the buffers are released either way
 * (display-wide, not the context's). */
void HwFrames::dropImports(bool deleteTextures)
{
    EglFns &f = egl();
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    const bool del = deleteTextures && ctx && ctx == m_ctx;
    const EGLDisplay_ dpy = f.ok ? f.getCurrentDisplay() : nullptr;
    for (const Imported &im : m_imported) {
        if (del) ctx->functions()->glDeleteTextures(1, &im.tex);
        if (f.ok && dpy) { f.destroyImage(dpy, im.image); f.releaseNativeBuffer(im.buffer); }
    }
    m_imported.clear();
}

void HwFrames::release(quint64 id)
{
    char m[16];
    memcpy(m, "REL1", 4);
    memset(m + 4, 0, 4);
    memcpy(m + 8, &id, 8);
    QMutexLocker l(&m_mutex);
    if (m_clientFd >= 0 && ::send(m_clientFd, m, sizeof m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {}
}
