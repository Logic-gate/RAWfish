// SPDX-License-Identifier: BSD-3-Clause
//
// 3D colour lookup tables (.cube text and PLUT binary) applied to 8-bit RGB.
// The file format reader follows the layout used by PhotonCamera's .plut files
// (version 1-3, sRGB input only, 8- or 16-bit samples).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

class ColorLut
{
public:
    static const int kMaxSize = 65;

    int size() const { return m_size; }
    bool isValid() const { return m_size >= 2; }

    // Detects PLUT (binary) or .cube (text). Returns false and fills *error on failure.
    static bool parse(const char *data, size_t length, ColorLut *out, std::string *error)
    {
        if (!out || !data || length == 0)
            return fail(error, "empty LUT file");
        if (length >= 4 && std::memcmp(data, "PLUT", 4) == 0)
            return parsePlut(reinterpret_cast<const uint8_t *>(data), length, out, error);
        return parseCube(std::string(data, length), out, error);
    }

    // Applies the LUT in place to 0xAARRGGBB pixels. strength 0..1 blends with the
    // original. Alpha is preserved.
    void apply(uint32_t *pixels, size_t count, float strength = 1.0f) const
    {
        if (!isValid() || !pixels || strength <= 0.0f)
            return;
        strength = std::min(strength, 1.0f);
        const int n = m_size;
        const float scale = float(n - 1) / 255.0f;
        int base[256];
        float frac[256];
        for (int v = 0; v < 256; ++v) {
            const float p = v * scale;
            int i = int(p);
            if (i >= n - 1)
                i = n - 2;
            base[v] = i;
            frac[v] = p - float(i);
        }
        const int strideG = n * 3;
        const int strideB = n * n * 3;
        const float *t = m_table.data();
        const bool blend = strength < 1.0f;
        for (size_t k = 0; k < count; ++k) {
            const uint32_t px = pixels[k];
            const int r = (px >> 16) & 0xff;
            const int g = (px >> 8) & 0xff;
            const int b = px & 0xff;
            const float fr = frac[r], fg = frac[g], fb = frac[b];
            const float *c0 = t + base[r] * 3 + base[g] * strideG + base[b] * strideB;
            // Tetrahedral interpolation: walk from the cell origin along the axes in
            // descending fraction order, so only four table corners are read.
            const float *p1, *p2;
            float w1, w2, w3;
            if (fr >= fg) {
                if (fg >= fb) {            // r >= g >= b
                    p1 = c0 + 3; p2 = c0 + 3 + strideG;
                    w1 = fr; w2 = fg; w3 = fb;
                } else if (fr >= fb) {     // r >= b > g
                    p1 = c0 + 3; p2 = c0 + 3 + strideB;
                    w1 = fr; w2 = fb; w3 = fg;
                } else {                   // b > r >= g
                    p1 = c0 + strideB; p2 = c0 + strideB + 3;
                    w1 = fb; w2 = fr; w3 = fg;
                }
            } else {
                if (fb >= fg) {            // b >= g > r
                    p1 = c0 + strideB; p2 = c0 + strideB + strideG;
                    w1 = fb; w2 = fg; w3 = fr;
                } else if (fb >= fr) {     // g > b >= r
                    p1 = c0 + strideG; p2 = c0 + strideG + strideB;
                    w1 = fg; w2 = fb; w3 = fr;
                } else {                   // g > r > b
                    p1 = c0 + strideG; p2 = c0 + strideG + 3;
                    w1 = fg; w2 = fr; w3 = fb;
                }
            }
            const float *c3 = c0 + 3 + strideG + strideB;
            const float orig[3] = { float(r), float(g), float(b) };
            int res[3];
            for (int ch = 0; ch < 3; ++ch) {
                const float v0 = c0[ch];
                const float v1 = p1[ch];
                const float v2 = p2[ch];
                float v = (v0 + w1 * (v1 - v0) + w2 * (v2 - v1) + w3 * (c3[ch] - v2))
                        * 255.0f;
                if (blend)
                    v = orig[ch] + (v - orig[ch]) * strength;
                res[ch] = v <= 0.0f ? 0 : (v >= 255.0f ? 255 : int(v + 0.5f));
            }
            pixels[k] = (px & 0xff000000u) | (uint32_t(res[0]) << 16)
                    | (uint32_t(res[1]) << 8) | uint32_t(res[2]);
        }
    }

    // Process-wide active LUT, consulted by ImageAdjustments for the preview and
    // for every saved image.
    static void setActive(std::shared_ptr<const ColorLut> lut, float strength = 1.0f)
    {
        std::lock_guard<std::mutex> lock(activeMutex());
        activeLut() = std::move(lut);
        activeStrength() = strength;
    }

    static std::shared_ptr<const ColorLut> active(float *strength = nullptr)
    {
        std::lock_guard<std::mutex> lock(activeMutex());
        if (strength)
            *strength = activeStrength();
        return activeLut();
    }

    static bool hasActive()
    {
        std::lock_guard<std::mutex> lock(activeMutex());
        return activeLut() && activeStrength() > 0.0f;
    }

private:
    int m_size = 0;
    // size^3 entries of RGB floats in 0..1; red varies fastest, then green, then blue.
    std::vector<float> m_table;

    static bool fail(std::string *error, const char *message)
    {
        if (error)
            *error = message;
        return false;
    }

    static std::mutex &activeMutex() { static std::mutex m; return m; }
    static std::shared_ptr<const ColorLut> &activeLut()
    {
        static std::shared_ptr<const ColorLut> lut;
        return lut;
    }
    static float &activeStrength() { static float s = 1.0f; return s; }

    static uint32_t readLe32(const uint8_t *p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)
                | (uint32_t(p[3]) << 24);
    }

    static bool parsePlut(const uint8_t *d, size_t length, ColorLut *out, std::string *error)
    {
        size_t pos = 4;
        auto next = [&](uint32_t *v) {
            if (pos + 4 > length)
                return false;
            *v = readLe32(d + pos);
            pos += 4;
            return true;
        };
        uint32_t version = 0, size = 0, dataType = 0;
        if (!next(&version) || !next(&size) || !next(&dataType))
            return fail(error, "truncated PLUT header");
        if (version < 1 || version > 3)
            return fail(error, "unsupported PLUT version");
        if (size < 2 || size > uint32_t(kMaxSize))
            return fail(error, "invalid PLUT cube size");
        if (dataType > 1)
            return fail(error, "unsupported PLUT sample type");
        if (version >= 2) {
            uint32_t curve = 0;
            if (!next(&curve))
                return fail(error, "truncated PLUT header");
            if (curve != 0)
                return fail(error, "PLUT expects a non-sRGB input curve (unsupported)");
        }
        if (version >= 3) {
            uint32_t gamut = 0;
            if (!next(&gamut))
                return fail(error, "truncated PLUT header");
            if (gamut != 0)
                return fail(error, "PLUT expects a non-sRGB gamut (unsupported)");
        }
        const size_t count = size_t(size) * size * size * 3;
        const size_t bytes = count * (dataType == 1 ? 2 : 1);
        if (length - pos < bytes)
            return fail(error, "truncated PLUT data");
        std::vector<float> table(count);
        if (dataType == 1) {
            for (size_t i = 0; i < count; ++i)
                table[i] = float(d[pos + i * 2] | (d[pos + i * 2 + 1] << 8)) / 65535.0f;
        } else {
            for (size_t i = 0; i < count; ++i)
                table[i] = float(d[pos + i]) / 255.0f;
        }
        out->m_size = int(size);
        out->m_table = std::move(table);
        return true;
    }

    static bool parseCube(const std::string &text, ColorLut *out, std::string *error)
    {
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        std::string line;
        int size = 0;
        float dmin[3] = { 0, 0, 0 }, dmax[3] = { 1, 1, 1 };
        std::vector<float> table;
        size_t filled = 0;
        while (std::getline(in, line)) {
            size_t a = line.find_first_not_of(" \t\r");
            if (a == std::string::npos || line[a] == '#')
                continue;
            std::istringstream ls(line.substr(a));
            ls.imbue(std::locale::classic());
            std::string key;
            ls >> key;
            if (key == "TITLE") {
                continue;
            } else if (key == "LUT_1D_SIZE") {
                return fail(error, "1D .cube LUTs are not supported");
            } else if (key == "LUT_3D_SIZE") {
                ls >> size;
                if (size < 2 || size > kMaxSize)
                    return fail(error, "invalid LUT_3D_SIZE");
                table.assign(size_t(size) * size * size * 3, 0.0f);
            } else if (key == "DOMAIN_MIN") {
                ls >> dmin[0] >> dmin[1] >> dmin[2];
            } else if (key == "DOMAIN_MAX") {
                ls >> dmax[0] >> dmax[1] >> dmax[2];
            } else {
                std::istringstream vs(line.substr(a));
                vs.imbue(std::locale::classic());
                float v[3];
                if (!(vs >> v[0] >> v[1] >> v[2]))
                    continue; // unknown keyword
                if (table.empty())
                    return fail(error, "LUT data before LUT_3D_SIZE");
                if (filled + 3 > table.size())
                    return fail(error, ".cube has more data than LUT_3D_SIZE implies");
                for (int ch = 0; ch < 3; ++ch) {
                    const float range = dmax[ch] - dmin[ch];
                    const float n = range != 0.0f ? (v[ch] - dmin[ch]) / range : 0.0f;
                    table[filled + ch] = std::min(1.0f, std::max(0.0f, n));
                }
                filled += 3;
            }
        }
        if (size == 0)
            return fail(error, "LUT_3D_SIZE not specified");
        if (filled != table.size())
            return fail(error, ".cube data size does not match LUT_3D_SIZE");
        out->m_size = size;
        out->m_table = std::move(table);
        return true;
    }
};
