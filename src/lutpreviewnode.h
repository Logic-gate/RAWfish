// SPDX-License-Identifier: BSD-3-Clause
//
// GPU colour look for the live viewfinder. The camera frame is drawn through a
// fragment shader that reads the 3D look table from a 2D strip texture and
// interpolates between its two nearest blue slices, so a look costs a few texture
// reads per pixel at the full preview resolution instead of a CPU pass over a
// shrunken copy. OpenGL ES 2 has no 3D textures, hence the strip.
#pragma once

#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSGGeometryNode>
#include <QSGSimpleMaterial>
#include <QSGSimpleMaterialShader>
#include <QSGTexture>
#include <QScopedPointer>

#include "colorlut.h"

namespace LutPreview {

// A strip is size*size wide and size high; texel (blue * size + red, green).
// Wider than most GPUs' guaranteed texture size is not worth the risk.
inline bool supported(const ColorLut &lut)
{
    return lut.isValid() && lut.size() * lut.size() <= 4096;
}

inline QImage buildStrip(const ColorLut &lut)
{
    const int n = lut.size();
    QImage strip(n * n, n, QImage::Format_RGB32);
    const std::vector<float> &table = lut.table();
    auto byte = [](float v) { return uint32_t(qBound(0.0f, v, 1.0f) * 255.0f + 0.5f); };
    for (int g = 0; g < n; ++g) {
        uint32_t *row = reinterpret_cast<uint32_t *>(strip.scanLine(g));
        for (int b = 0; b < n; ++b) {
            for (int r = 0; r < n; ++r) {
                const float *c = table.data() + ((size_t(b) * n + g) * n + r) * 3;
                row[b * n + r] = 0xff000000u | (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]);
            }
        }
    }
    return strip;
}

struct State {
    QSGTexture *frame = nullptr;
    QSGTexture *lut = nullptr;
    float size = 2.0f;
    float strength = 1.0f;
};

class Shader : public QSGSimpleMaterialShader<State>
{
    QSG_DECLARE_SIMPLE_SHADER(Shader, State)

public:
    QList<QByteArray> attributes() const override
    {
        return QList<QByteArray>() << "aVertex" << "aTexCoord";
    }

    const char *vertexShader() const override
    {
        return "uniform highp mat4 qt_Matrix;\n"
               "attribute highp vec4 aVertex;\n"
               "attribute highp vec2 aTexCoord;\n"
               "varying highp vec2 vTexCoord;\n"
               "void main() {\n"
               "    vTexCoord = aTexCoord;\n"
               "    gl_Position = qt_Matrix * aVertex;\n"
               "}\n";
    }

    const char *fragmentShader() const override
    {
        return "uniform sampler2D frame;\n"
               "uniform sampler2D lut;\n"
               "uniform highp float lutSize;\n"
               "uniform highp float strength;\n"
               "uniform lowp float qt_Opacity;\n"
               "varying highp vec2 vTexCoord;\n"
               "void main() {\n"
               "    highp vec3 c = clamp(texture2D(frame, vTexCoord).rgb, 0.0, 1.0);\n"
               "    highp float n = lutSize;\n"
               "    highp float b = c.b * (n - 1.0);\n"
               "    highp float b0 = floor(b);\n"
               "    highp float b1 = min(b0 + 1.0, n - 1.0);\n"
               "    highp float v = (c.g * (n - 1.0) + 0.5) / n;\n"
               "    highp float u = c.r * (n - 1.0) + 0.5;\n"
               "    highp vec3 a = texture2D(lut, vec2((b0 * n + u) / (n * n), v)).rgb;\n"
               "    highp vec3 d = texture2D(lut, vec2((b1 * n + u) / (n * n), v)).rgb;\n"
               "    highp vec3 graded = mix(a, d, b - b0);\n"
               "    gl_FragColor = vec4(mix(c, graded, strength), 1.0) * qt_Opacity;\n"
               "}\n";
    }

    void resolveUniforms() override
    {
        m_frame = program()->uniformLocation("frame");
        m_lut = program()->uniformLocation("lut");
        m_size = program()->uniformLocation("lutSize");
        m_strength = program()->uniformLocation("strength");
    }

    void updateState(const State *state, const State *) override
    {
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        gl->glActiveTexture(GL_TEXTURE1);
        state->lut->bind();
        gl->glActiveTexture(GL_TEXTURE0);
        state->frame->bind();
        program()->setUniformValue(m_frame, 0);
        program()->setUniformValue(m_lut, 1);
        program()->setUniformValue(m_size, state->size);
        program()->setUniformValue(m_strength, state->strength);
    }

private:
    int m_frame = -1;
    int m_lut = -1;
    int m_size = -1;
    int m_strength = -1;
};

// One textured quad. The node owns both textures and is rebuilt for every frame.
class Node : public QSGGeometryNode
{
public:
    Node(QSGTexture *frame, QSGTexture *lut, float size, float strength,
         const QRectF &drawRect, const QRectF &normalizedSource)
        : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4)
        , m_frame(frame)
        , m_lut(lut)
    {
        QSGSimpleMaterial<State> *material = Shader::createMaterial();
        material->state()->frame = frame;
        material->state()->lut = lut;
        material->state()->size = size;
        material->state()->strength = strength;
        material->setFlag(QSGMaterial::Blending, false);
        setMaterial(material);
        setFlag(OwnsMaterial);
        QSGGeometry::updateTexturedRectGeometry(&m_geometry, drawRect, normalizedSource);
        setGeometry(&m_geometry);
    }

private:
    QSGGeometry m_geometry;
    QScopedPointer<QSGTexture> m_frame;
    QScopedPointer<QSGTexture> m_lut;
};

} // namespace LutPreview
