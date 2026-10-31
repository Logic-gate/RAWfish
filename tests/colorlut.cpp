// SPDX-License-Identifier: BSD-3-Clause
#include "../src/colorlut.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

static std::string identityCube(int n)
{
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "TITLE \"id\"\n# comment\nLUT_3D_SIZE " << n << "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
                s << float(r) / (n - 1) << ' ' << float(g) / (n - 1) << ' '
                  << float(b) / (n - 1) << '\n';
    return s.str();
}

static uint32_t px(int r, int g, int b) { return 0xff000000u | (r << 16) | (g << 8) | b; }

static void le32(std::string *s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s->push_back(char((v >> (8 * i)) & 0xff));
}

int main(int argc, char **argv)
{
    std::string err;
    ColorLut lut;

    // Identity cube is a no-op for every 8-bit value, alpha preserved.
    const std::string cube = identityCube(17);
    assert(ColorLut::parse(cube.data(), cube.size(), &lut, &err));
    assert(lut.size() == 17);
    std::vector<uint32_t> ramp;
    for (int v = 0; v < 256; ++v)
        ramp.push_back((0x80u << 24) | (v << 16) | (((v * 7) & 255) << 8) | (255 - v));
    std::vector<uint32_t> copy = ramp;
    lut.apply(copy.data(), copy.size());
    for (size_t i = 0; i < ramp.size(); ++i) {
        assert((copy[i] >> 24) == 0x80);
        for (int sh = 0; sh <= 16; sh += 8)
            assert(std::abs(int((copy[i] >> sh) & 255) - int((ramp[i] >> sh) & 255)) <= 1);
    }

    // Channel-swapping cube (out = bgr) proves the red-fastest axis order.
    {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    s << b << ' ' << g << ' ' << r << '\n';
        const std::string swap = s.str();
        ColorLut sw;
        assert(ColorLut::parse(swap.data(), swap.size(), &sw, &err));
        uint32_t p[1] = { px(255, 0, 0) };
        sw.apply(p, 1);
        assert(p[0] == px(0, 0, 255));
        uint32_t q[1] = { px(200, 100, 50) };
        sw.apply(q, 1);
        assert(q[0] == px(50, 100, 200));
    }

    // Strength blends with the original.
    {
        std::string inv = "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    inv += std::to_string(1 - r) + " " + std::to_string(1 - g) + " "
                            + std::to_string(1 - b) + "\n";
        ColorLut il;
        assert(ColorLut::parse(inv.data(), inv.size(), &il, &err));
        uint32_t p[1] = { px(0, 0, 0) };
        il.apply(p, 1, 0.5f);
        assert(p[0] == px(128, 128, 128));
        uint32_t z[1] = { px(10, 20, 30) };
        il.apply(z, 1, 0.0f);
        assert(z[0] == px(10, 20, 30));
    }

    // PLUT v1, uint8 and uint16 identity 2x2x2 cubes.
    for (int type = 0; type < 2; ++type) {
        std::string f = "PLUT";
        le32(&f, 1); le32(&f, 2); le32(&f, type);
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    for (int v : { r, g, b}) {
                        if (type == 0) f.push_back(char(v ? 255 : 0));
                        else { f.push_back(char(v ? 255 : 0)); f.push_back(char(v ? 255 : 0)); }
                    }
        ColorLut pl;
        assert(ColorLut::parse(f.data(), f.size(), &pl, &err));
        uint32_t p[1] = { px(64, 128, 192) };
        pl.apply(p, 1);
        assert(p[0] == px(64, 128, 192));
        // truncation is rejected
        assert(!ColorLut::parse(f.data(), f.size() - 1, &pl, &err));
    }

    // Unsupported inputs fail with a message instead of crashing.
    {
        std::string f = "PLUT";
        le32(&f, 4); le32(&f, 2); le32(&f, 0);
        assert(!ColorLut::parse(f.data(), f.size(), &lut, &err) && !err.empty());
        std::string f2 = "PLUT";
        le32(&f2, 2); le32(&f2, 2); le32(&f2, 0); le32(&f2, 3);
        assert(!ColorLut::parse(f2.data(), f2.size(), &lut, &err));
        const std::string d1 = "LUT_1D_SIZE 4\n0 0 0\n";
        assert(!ColorLut::parse(d1.data(), d1.size(), &lut, &err));
        const std::string bad = "LUT_3D_SIZE 3\n0 0 0\n";
        assert(!ColorLut::parse(bad.data(), bad.size(), &lut, &err));
        assert(!ColorLut::parse("", 0, &lut, &err));
    }

    // Active LUT holder.
    assert(!ColorLut::hasActive());
    ColorLut::setActive(std::make_shared<ColorLut>(lut));
    assert(ColorLut::hasActive());
    ColorLut::setActive(nullptr);
    assert(!ColorLut::hasActive());

    // Optional: real PhotonCamera .plut files given on the command line. "standard"
    // looks should stay close to identity, which validates axis order and sample type.
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ColorLut real;
        const bool ok = ColorLut::parse(data.data(), data.size(), &real, &err);
        std::printf("%s: %s", argv[i], ok ? "ok" : err.c_str());
        if (ok) {
            uint32_t g[3] = { px(255, 0, 0), px(0, 255, 0), px(0, 0, 255) };
            real.apply(g, 3);
            std::printf(" size=%d  red->(%u,%u,%u) green->(%u,%u,%u) blue->(%u,%u,%u)",
                        real.size(), (g[0] >> 16) & 255, (g[0] >> 8) & 255, g[0] & 255,
                        (g[1] >> 16) & 255, (g[1] >> 8) & 255, g[1] & 255,
                        (g[2] >> 16) & 255, (g[2] >> 8) & 255, g[2] & 255);
        }
        std::printf("\n");
    }
    std::puts("colorlut: all checks passed");
    return 0;
}
