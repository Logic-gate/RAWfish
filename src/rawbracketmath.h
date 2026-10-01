// SPDX-FileCopyrightText: 2026 Sean Hoyt
// SPDX-License-Identifier: MIT
// Adapted from RawBracket RawMath.java and merge.frag; see licenses/RawBracket-MIT.txt.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace RawBracket {
using RGB = std::array<double, 3>;
struct Plane {
    int width = 0, height = 0, cfa = 0;
    double white = 0;
    std::array<double, 4> black{{0, 0, 0, 0}};
    std::vector<uint16_t> pixels;
    int channel(int k) const {
        static const int channels[4][4] = {{0,1,1,2},{1,0,2,1},{1,2,0,1},{2,1,1,0}};
        return channels[cfa][k];
    }
    double at(int x, int y) const { return pixels[std::size_t(y) * width + x]; }
};
struct Estimate {
    double gain, scatter;
    int samples;
    bool accepted;
};
inline Estimate estimate(const Plane &l, const Plane &s, double nominal) {
    std::vector<double> ratios;
    for (int y = 0; y + 8 < l.height && ratios.size() < 4096; y += std::max(8,l.height/32))
        for (int x = 0; x + 8 < l.width && ratios.size() < 4096; x += std::max(8,l.width/32))
            for (int cy = 0; cy < 2; ++cy) for (int cx = 0; cx < 2; ++cx) {
                const int p = ((y+cy)&1)*2 + ((x+cx)&1);
                const double lr = l.white-l.black[p], sr = s.white-s.black[p];
                double lv=0, sv=0, residual=0;
                bool valid=true;
                for (int oy=cy; oy<8; oy+=2) for (int ox=cx; ox<8; ox+=2) {
                    int xx=x+ox, yy=y+oy;
                    if (xx+2>=l.width || yy+2>=l.height) { valid=false; continue; }
                    const double peak=std::max(std::max(l.at(xx,yy),l.at(xx+2,yy)),
                                               std::max(l.at(xx,yy+2),l.at(xx+2,yy+2)));
                    const double ld=l.at(xx,yy)-l.black[p], sd=s.at(xx,yy)-s.black[p];
                    if (peak>=l.white || sd>=sr*.8) valid=false;
                    lv+=ld; sv+=sd; residual+=std::abs(ld-nominal*sd);
                }
                if (!valid) continue;
                lv/=16; sv/=16;
                if (residual/16 > lv*.20+nominal*2) continue;
                const double floor=std::max(2.0,std::min(16.0,lr*.20/nominal));
                if (sv<floor || lv<lr*.03 || lv>lr*.80) continue;
                const double ratio=lv/sv;
                if (ratio>nominal*.5 && ratio<nominal*2 && ratios.size()<4096) ratios.push_back(ratio);
            }
    const int n=int(ratios.size());
    if (n<64) return {nominal,0,n,false};
    std::sort(ratios.begin(),ratios.end());
    const double median=ratios[n/2];
    for (double &v:ratios) v=std::abs(v-median);
    std::sort(ratios.begin(),ratios.end());
    const double scatter=ratios[n/2]/median;
    const bool accepted=scatter<.06 && median>nominal*.75 && median<nominal*1.25;
    return {accepted?median:nominal,scatter,n,accepted};
}
inline int reflect(int p, int size) {
    if (p<0) p=-p;
    if (p>=size) p=2*size-2-p;
    return p;
}
inline RGB reconstruct(const Plane &frame, const Plane &longFrame, int x, int y, bool *clipped) {
    RGB rgb{{0,0,0}};
    for (int k=0;k<4;++k) {
        const double gx=(x-(k&1))*.5, gy=(y-(k>>1))*.5;
        const int bx=int(std::floor(gx))*2+(k&1), by=int(std::floor(gy))*2+(k>>1);
        const double fx=gx-std::floor(gx), fy=gy-std::floor(gy);
        double value=0;
        for (int yy=0;yy<2;++yy) for (int xx=0;xx<2;++xx) {
            const double weight=(xx?fx:1-fx)*(yy?fy:1-fy);
            if (weight<=.00001) continue;
            const double dn=frame.at(reflect(bx+2*xx,frame.width),reflect(by+2*yy,frame.height));
            value+=weight*std::max(0.0,dn-frame.black[k])/(longFrame.white-longFrame.black[k]);
            if (clipped && dn>=frame.white) *clipped=true;
        }
        const int c=frame.channel(k);
        rgb[c]+=value*(c==1?.5:1);
    }
    return rgb;
}
inline RGB merge(RGB longRgb, const RGB &shortRgb, bool clipped, const RGB &wb,
                 const RGB &luma) {
    if (!clipped) return longRgb;
    const double white=std::min(wb[0],std::min(wb[1],wb[2]));
    double ly=0,sy=0;
    for (int c=0;c<3;++c) {
        longRgb[c]=std::min(longRgb[c],white/wb[c]);
        ly+=luma[c]*longRgb[c]; sy+=luma[c]*shortRgb[c];
    }
    if (sy>ly) for (int c=0;c<3;++c) longRgb[c]=std::max(longRgb[c],shortRgb[c]);
    return longRgb;
}
inline double srgb(double value) {
    return value<=.0031308 ? 12.92*value : 1.055*std::pow(value,1/2.4)-.055;
}
}
