// Numerical adapter for tests/bracket_reference.py; no image encoding.
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "../src/rawbracketmath.h"
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QFile input;input.open(stdin,QIODevice::ReadOnly);
    const auto m=QJsonDocument::fromJson(input.readAll()).object();
    RawBracket::Plane l,s;
    l.width=s.width=m["width"].toInt();l.height=s.height=m["height"].toInt();
    l.cfa=s.cfa=m["cfa"].toInt();l.white=s.white=m["white"].toDouble();
    for(int k=0;k<4;++k){l.black[k]=m["lb"].toArray()[k].toDouble();s.black[k]=m["sb"].toArray()[k].toDouble();}
    for(auto v:m["long"].toArray())l.pixels.push_back(uint16_t(v.toInt()));
    for(auto v:m["short"].toArray())s.pixels.push_back(uint16_t(v.toInt()));
    RawBracket::RGB wb,luma;
    for(int c=0;c<3;++c){wb[c]=m["wb"].toArray()[c].toDouble();luma[c]=m["luma"].toArray()[c].toDouble();}
    QJsonArray out;
    for(int y=0;y<l.height;++y)for(int x=0;x<l.width;++x){
        bool clipped=false;auto rgb=RawBracket::reconstruct(l,l,x,y,&clipped);
        auto shortRgb=RawBracket::reconstruct(s,l,x,y,nullptr);
        for(double &v:shortRgb)v*=m["gain"].toDouble();
        for(double v:RawBracket::merge(rgb,shortRgb,clipped,wb,luma))out.append(v);
    }
    QFile output;output.open(stdout,QIODevice::WriteOnly);output.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
}
