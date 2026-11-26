/* EGL and libhybris's native-buffer extension for the zero-copy viewfinder
 * (hwframes.cpp) and its probe (hwprobe.cpp): looked up at run time
 * (QLibrary), no EGL headers or link needed, and a missing piece is
 * reported, not a crash. */
#ifndef RAWFISH_IR_HWEGL_H
#define RAWFISH_IR_HWEGL_H

#include <QLibrary>
#include <QString>
#include <QStringList>

typedef void *EGLDisplay_;
typedef void *EGLImage_;
typedef void *EGLClientBuffer_;
typedef unsigned int EGLBoolean_;
typedef int EGLint_;
const EGLint_ kEglNativeBufferAndroid = 0x3140;     /* EGL_NATIVE_BUFFER_ANDROID */
const EGLint_ kEglImagePreserved = 0x30D2;          /* EGL_IMAGE_PRESERVED_KHR   */
const EGLint_ kEglNone = 0x3038;
const unsigned kGlTextureExternal = 0x8D65;         /* GL_TEXTURE_EXTERNAL_OES   */

struct EglFns {
    bool loaded = false, ok = false;
    QString missing;
    EGLDisplay_ (*getCurrentDisplay)() = nullptr;
    void *(*getProcAddress)(const char *) = nullptr;
    EGLint_ (*getError)() = nullptr;
    EGLBoolean_ (*createRemoteBuffer)(EGLint_, EGLint_, EGLint_, EGLint_, EGLint_, int, int *, int, int *, EGLClientBuffer_ *) = nullptr;
    EGLBoolean_ (*releaseNativeBuffer)(EGLClientBuffer_) = nullptr;
    EGLImage_ (*createImage)(EGLDisplay_, void *, EGLint_, EGLClientBuffer_, const EGLint_ *) = nullptr;
    EGLBoolean_ (*destroyImage)(EGLDisplay_, EGLImage_) = nullptr;
    void (*imageTargetTexture)(unsigned, void *) = nullptr;
};
inline EglFns &egl()
{
    static EglFns f;
    if (f.loaded) return f;
    f.loaded = true;
    QLibrary lib(QStringLiteral("EGL"), 1);
    if (!lib.load()) { f.missing = QStringLiteral("libEGL.so.1"); return f; }
    f.getCurrentDisplay = reinterpret_cast<EGLDisplay_ (*)()>(lib.resolve("eglGetCurrentDisplay"));
    f.getProcAddress = reinterpret_cast<void *(*)(const char *)>(lib.resolve("eglGetProcAddress"));
    f.getError = reinterpret_cast<EGLint_ (*)()>(lib.resolve("eglGetError"));
    if (!f.getCurrentDisplay || !f.getProcAddress || !f.getError) { f.missing = QStringLiteral("egl core"); return f; }
    f.createRemoteBuffer = reinterpret_cast<decltype(f.createRemoteBuffer)>(f.getProcAddress("eglHybrisCreateRemoteBuffer"));
    f.releaseNativeBuffer = reinterpret_cast<decltype(f.releaseNativeBuffer)>(f.getProcAddress("eglHybrisReleaseNativeBuffer"));
    f.createImage = reinterpret_cast<decltype(f.createImage)>(f.getProcAddress("eglCreateImageKHR"));
    f.destroyImage = reinterpret_cast<decltype(f.destroyImage)>(f.getProcAddress("eglDestroyImageKHR"));
    f.imageTargetTexture = reinterpret_cast<decltype(f.imageTargetTexture)>(f.getProcAddress("glEGLImageTargetTexture2DOES"));
    QStringList m;
    if (!f.createRemoteBuffer) m << QStringLiteral("eglHybrisCreateRemoteBuffer");
    if (!f.releaseNativeBuffer) m << QStringLiteral("eglHybrisReleaseNativeBuffer");
    if (!f.createImage) m << QStringLiteral("eglCreateImageKHR");
    if (!f.destroyImage) m << QStringLiteral("eglDestroyImageKHR");
    if (!f.imageTargetTexture) m << QStringLiteral("glEGLImageTargetTexture2DOES");
    f.missing = m.join(QLatin1Char(','));
    f.ok = m.isEmpty();
    return f;
}

#endif
