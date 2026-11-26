#include "hwprobe.h"

#include <QSocketNotifier>
#include <QLibrary>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <QImage>
#include <QFile>
#include <QDir>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The message the engine sends (android/rawfish-ir.c, hwp_msg_t). */
struct HwpMsg {
    char magic[4];
    quint32 seq, width, height, stride, format, layers;
    quint64 usage;
    qint32 nfds, nints;
    qint32 ints[64];
};

#include "hwegl.h"

HwProbe::HwProbe(QObject *parent) : QObject(parent) {}

HwProbe::~HwProbe()
{
    closeClient();
    if (m_listenFd >= 0) { ::close(m_listenFd); m_listenFd = -1; }
    if (!m_path.isEmpty()) QFile::remove(m_path);
    QMutexLocker l(&m_mutex);
    for (int fd : m_pending.fds) ::close(fd);
}

bool HwProbe::listen(const QString &path, const QString &pngDir)
{
    if (m_listenFd >= 0) return true;
    m_path = path;
    m_pngDir = pngDir;
    const QByteArray p = QFile::encodeName(path);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (size_t(p.size()) >= sizeof a.sun_path) { addResult(QStringLiteral("error=socket path too long")); return false; }
    memcpy(a.sun_path, p.constData(), size_t(p.size()));
    ::unlink(p.constData());
    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0 || ::bind(m_listenFd, reinterpret_cast<struct sockaddr *>(&a), sizeof a) != 0 || ::listen(m_listenFd, 2) != 0) {
        addResult(QStringLiteral("error=listen errno=%1 path=%2").arg(errno).arg(path));
        if (m_listenFd >= 0) ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }
    m_listenNotifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_listenNotifier, &QSocketNotifier::activated, this, &HwProbe::onAccept);
    addResult(QStringLiteral("listening on %1").arg(path));
    return true;
}

void HwProbe::onAccept()
{
    const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) return;
    closeClient();                 /* one engine at a time: a new one replaces the old */
    {
        QMutexLocker l(&m_mutex);
        m_clientFd = fd;
    }
    m_clientNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_clientNotifier, &QSocketNotifier::activated, this, &HwProbe::onReadable);
}

void HwProbe::closeClient()
{
    delete m_clientNotifier;
    m_clientNotifier = nullptr;
    QMutexLocker l(&m_mutex);
    if (m_clientFd >= 0) { ::close(m_clientFd); m_clientFd = -1; }
}

void HwProbe::onReadable()
{
    HwpMsg m;
    char cbuf[CMSG_SPACE(sizeof(int) * 16)];
    struct iovec iov = { &m, sizeof m };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    int fd;
    {
        QMutexLocker l(&m_mutex);
        fd = m_clientFd;
    }
    const ssize_t n = ::recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
    if (n <= 0) { closeClient(); return; }    /* the engine went away */
    QVector<int> fds;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            const int k = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            const int *p = reinterpret_cast<const int *>(CMSG_DATA(c));
            for (int i = 0; i < k; ++i) fds << p[i];
        }
    if (n != ssize_t(sizeof m) || memcmp(m.magic, "HWB1", 4) || m.nints < 0 || m.nints > 64 || m.nfds != fds.size()) {
        addResult(QStringLiteral("error=bad message bytes=%1 fds=%2/%3").arg(n).arg(fds.size()).arg(m.nfds));
        for (int f : fds) ::close(f);
        const char nak = 'n';
        if (::write(fd, &nak, 1) < 0) {}
        return;
    }
    Pending p;
    p.seq = m.seq; p.width = m.width; p.height = m.height; p.stride = m.stride;
    p.format = m.format; p.layers = m.layers; p.usage = m.usage;
    for (int i = 0; i < m.nints; ++i) p.ints << m.ints[i];
    p.fds = fds;
    {
        QMutexLocker l(&m_mutex);
        for (int f : m_pending.fds) ::close(f);      /* an older one never drawn */
        m_pending = p;
        m_hasPending = true;
    }
    emit bufferArrived();
}

void HwProbe::addResult(const QString &line)
{
    QMutexLocker l(&m_mutex);
    m_results << line;
}

QStringList HwProbe::takeResults()
{
    QMutexLocker l(&m_mutex);
    QStringList r = m_results;
    m_results.clear();
    return r;
}

/* Draws an external (EGLImage) texture into a small framebuffer and reads
 * it back. */
static QImage drawExternal(QOpenGLFunctions *gl, unsigned tex, int w, int h, QString *err)
{
    static const char *vs =
        "attribute highp vec2 pos;\n"
        "varying highp vec2 tc;\n"
        "void main() { tc = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "uniform samplerExternalOES tex;\n"
        "varying highp vec2 tc;\n"
        "void main() { gl_FragColor = texture2D(tex, tc); }\n";
    QOpenGLShaderProgram prog;
    if (!prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vs) || !prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fs)) {
        *err = QStringLiteral("shader: ") + prog.log().simplified().left(160);
        return QImage();
    }
    prog.bindAttributeLocation("pos", 0);
    if (!prog.link()) { *err = QStringLiteral("link: ") + prog.log().simplified().left(160); return QImage(); }
    QOpenGLFramebufferObject fbo(w, h);
    if (!fbo.isValid() || !fbo.bind()) { *err = QStringLiteral("fbo"); return QImage(); }
    gl->glViewport(0, 0, w, h);
    gl->glDisable(GL_BLEND);
    gl->glClearColor(1.0f, 0.0f, 1.0f, 1.0f);      /* magenta: nothing drawn */
    gl->glClear(GL_COLOR_BUFFER_BIT);
    prog.bind();
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(kGlTextureExternal, tex);
    prog.setUniformValue("tex", 0);
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->glDisableVertexAttribArray(0);
    const GLenum e = gl->glGetError();
    gl->glBindTexture(kGlTextureExternal, 0);
    prog.release();
    QImage img = fbo.toImage();
    fbo.release();
    if (e != GL_NO_ERROR) *err = QStringLiteral("draw glError=0x%1").arg(e, 0, 16);
    return img;
}

void HwProbe::renderPending(QQuickWindow *window)
{
    Pending p;
    int clientFd;
    {
        QMutexLocker l(&m_mutex);
        if (!m_hasPending) return;
        p = m_pending;
        m_pending = Pending();
        m_hasPending = false;
        clientFd = m_clientFd;
    }
    QElapsedTimer t;
    t.start();
    QString head = QStringLiteral("seq=%1 %2x%3 stride=%4 format=0x%5 usage=0x%6 fds=%7 ints=%8")
            .arg(p.seq).arg(p.width).arg(p.height).arg(p.stride).arg(p.format, 0, 16).arg(p.usage, 0, 16)
            .arg(p.fds.size()).arg(p.ints.size());
    QStringList steps;
    bool good = false;
    EglFns &f = egl();
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx) steps << QStringLiteral("error=no GL context");
    else if (!f.ok) steps << QStringLiteral("error=missing ") + f.missing;
    else {
        QOpenGLFunctions *gl = ctx->functions();
        const EGLDisplay_ dpy = f.getCurrentDisplay();
        steps << QStringLiteral("gl=\"%1\"").arg(QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER))).left(40));
        /* The format the buffer declares first; a flexible YUV (0x23) may
         * stand for the driver's own layout: NV21 (0x11), NV12 (0x100 on
         * some MediaTek grallocs is not standard: skipped), YV12. */
        QVector<quint32> formats;
        formats << p.format;
        for (quint32 alt : { 0x11u, 0x32315659u, 0x23u }) if (!formats.contains(alt)) formats << alt;
        for (quint32 fmt : formats) {
            QString s = QStringLiteral("try fmt=0x%1:").arg(fmt, 0, 16);
            QVector<int> dupFds;
            for (int fd : p.fds) dupFds << ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
            QVector<int> ints = p.ints;
            EGLClientBuffer_ buf = nullptr;
            /* the handle's fds are closed by the import (native_handle_close): our duplicates */
            if (!f.createRemoteBuffer(EGLint_(p.width), EGLint_(p.height), EGLint_(quint32(p.usage)), EGLint_(fmt), EGLint_(p.stride),
                                      ints.size(), ints.data(), dupFds.size(), dupFds.data(), &buf) || !buf) {
                for (int fd : dupFds) ::close(fd);       /* may already be closed: harmless */
                steps << s + QStringLiteral(" remote_buffer=fail");
                continue;
            }
            s += QStringLiteral(" remote_buffer=ok");
            const EGLint_ attrs[] = { kEglImagePreserved, 1, kEglNone };
            EGLImage_ image = f.createImage(dpy, nullptr, kEglNativeBufferAndroid, buf, attrs);
            if (!image) {
                steps << s + QStringLiteral(" egl_image=fail eglError=0x%1").arg(f.getError(), 0, 16);
                f.releaseNativeBuffer(buf);
                continue;
            }
            s += QStringLiteral(" egl_image=ok");
            GLuint tex = 0;
            gl->glGenTextures(1, &tex);
            gl->glBindTexture(kGlTextureExternal, tex);
            gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(kGlTextureExternal, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            while (gl->glGetError() != GL_NO_ERROR) {}
            f.imageTargetTexture(kGlTextureExternal, image);
            const GLenum te = gl->glGetError();
            if (te != GL_NO_ERROR) s += QStringLiteral(" texture=fail glError=0x%1").arg(te, 0, 16);
            else {
                s += QStringLiteral(" texture=ok");
                QString err;
                const QImage out = drawExternal(gl, tex, 320, 240, &err);
                if (out.isNull()) s += QStringLiteral(" draw=fail ") + err;
                else {
                    /* what came out: its mean and spread (a flat or magenta
                     * image is no picture), the mean per channel */
                    double sum = 0, sum2 = 0, r = 0, g = 0, b = 0; int magenta = 0;
                    const int n = out.width() * out.height();
                    for (int y = 0; y < out.height(); ++y)
                        for (int x = 0; x < out.width(); ++x) {
                            const QRgb c = out.pixel(x, y);
                            const double l = 0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c);
                            sum += l; sum2 += l * l; r += qRed(c); g += qGreen(c); b += qBlue(c);
                            if (qRed(c) > 250 && qGreen(c) < 5 && qBlue(c) > 250) ++magenta;
                        }
                    const double mean = sum / n, sd = std::sqrt(qMax(0.0, sum2 / n - mean * mean));
                    s += QStringLiteral(" draw=ok%1 mean=%2 sd=%3 rgb=%4/%5/%6 magenta=%7%")
                            .arg(err.isEmpty() ? QString() : QStringLiteral("(") + err + QLatin1Char(')'))
                            .arg(mean, 0, 'f', 1).arg(sd, 0, 'f', 1)
                            .arg(r / n, 0, 'f', 0).arg(g / n, 0, 'f', 0).arg(b / n, 0, 'f', 0)
                            .arg(100.0 * magenta / n, 0, 'f', 1);
                    if (sd > 3.0 && magenta < n / 10) {
                        good = true;
                        if (!m_pngSaved) {
                            const QString png = m_pngDir + QStringLiteral("/hwprobe-%1-fmt%2.png").arg(p.seq).arg(fmt, 0, 16);
                            if (out.save(png)) { m_pngSaved = true; s += QStringLiteral(" saved=") + png; }
                        }
                    }
                }
            }
            gl->glBindTexture(kGlTextureExternal, 0);
            gl->glDeleteTextures(1, &tex);
            f.destroyImage(dpy, image);
            f.releaseNativeBuffer(buf);
            steps << s;
            if (good) break;
        }
    }
    for (int fd : p.fds) ::close(fd);
    if (window) window->resetOpenGLState();
    addResult(head + QStringLiteral(" | ") + steps.join(QStringLiteral(" | "))
              + QStringLiteral(" | %1 ms").arg(t.elapsed()) + (good ? QStringLiteral(" | RESULT=picture") : QStringLiteral(" | RESULT=none")));
    const char ack = good ? 'k' : 'n';
    QMutexLocker l(&m_mutex);
    if (clientFd >= 0 && clientFd == m_clientFd && ::write(clientFd, &ack, 1) < 0) {}
}
