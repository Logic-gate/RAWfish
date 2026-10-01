#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QImage>
#include "../src/rawbracket.h"
#include "../src/rawbracketmath.h"
using namespace RawBracket;

class BracketTest : public QObject {
    Q_OBJECT
    Plane uniform(RGB values, int cfa=0, int size=10) {
        Plane p; p.width=p.height=size; p.cfa=cfa; p.white=1023; p.black={{64,64,64,64}};
        p.pixels.resize(size*size);
        for (int y=0;y<size;++y) for(int x=0;x<size;++x)
            p.pixels[y*size+x]=uint16_t(64+values[p.channel((y%2)*2+x%2)]);
        return p;
    }
    QString frame(const QString &dir, const QString &name, bool packed, double time, double stamp) {
        Plane p=uniform({{200,400,600}});
        QFile raw(dir+"/"+name+".raw"); if(!raw.open(QIODevice::WriteOnly)) return {};
        QByteArray bytes;
        for(int y=0;y<p.height;++y) {
            if(packed) for(int x=0;x<p.width;x+=4) {
                unsigned char low=0;
                for(int k=0;k<4;++k) {
                    const uint16_t v=x+k<p.width?p.pixels[y*p.width+x+k]:0;
                    bytes.append(char(v>>2)); low|=(v&3)<<(k*2);
                }
                bytes.append(char(low));
            } else for(int x=0;x<p.width;++x) {
                const uint16_t v=p.pixels[y*p.width+x];bytes.append(char(v&255));bytes.append(char(v>>8));
            }
        }
        raw.write(bytes);raw.close();
        QJsonArray matrix;for(int i=0;i<9;++i) matrix.append(QJsonArray{i%4==0?1:0,1});
        QJsonObject m{{"camera_id","0"},{"width",p.width},{"height",p.height},
            {"format",packed?"RAW10":"RAW16"},{"bits_per_sample",packed?10:16},
            {"row_stride",packed?15:20},{"cfa","RGGB"},{"white_level",1023},
            {"black_level_pattern",QJsonArray{64,64,64,64}}, {"raw_path",raw.fileName()},
            {"iso",100},{"exposure_time_ns",time},{"image_timestamp_ns",stamp},{"sensor_timestamp_ns",stamp},
            {"color_correction_gains",QJsonArray{1,1,1,1}},{"capture_color_transform",matrix},{"render_exposure","1.0"}};
        QFile metadata(dir+"/"+name+".json");if(!metadata.open(QIODevice::WriteOnly))return {};
        metadata.write(QJsonDocument(m).toJson());return metadata.fileName();
    }
private slots:
    void reconstruction() {
        for(int cfa=0;cfa<4;++cfa) {
            Plane p=uniform({{200,400,600}},cfa);
            for(int y=0;y<p.height;++y)for(int x=0;x<p.width;++x) {
                bool clipped=false;const RGB rgb=reconstruct(p,p,x,y,&clipped);
                QVERIFY(!clipped);
                for(int c=0;c<3;++c) QVERIFY(std::abs(rgb[c]-(c+1)*200.0/959)<1e-10);
            }
        }
    }
    void clippingAndLuminance() {
        RGB wb{{2,1,2}}, luma{{.4252,.7152,.1444}};
        RGB l{{1,1,1}}, s{{0,0,0}};
        QVERIFY(merge(l,s,false,wb,luma) == l);
        const RGB capped{{.5,1,.5}};
        QVERIFY(merge(l,s,true,wb,luma) == capped);
        QVERIFY(merge(l,capped,true,wb,luma) == capped); // tie retains corrected long
        s={{.8,0,0}}; // brighter red, lower whole-pixel luminance
        QVERIFY(merge(l,s,true,wb,luma) == capped);
        s={{2,2,2}};
        QVERIFY(merge(l,s,true,wb,luma) == s);
        Plane p=uniform({{200,400,600}});p.pixels[4*p.width+4]=1023;
        bool clipped=false; reconstruct(p,p,5,4,&clipped);QVERIFY(clipped);
        clipped=false;reconstruct(p,p,8,8,&clipped);QVERIFY(!clipped);
    }
    void gainRefinement() {
        Plane l=uniform({{400,400,400}},0,128), s=uniform({{100,100,100}},0,128);
        const Estimate result=estimate(l,s,4.2);
        QVERIFY(result.accepted);QVERIFY(result.samples>=64);QCOMPARE(result.gain,4.0);
        s=uniform({{1,1,1}},0,128);
        const Estimate dark=estimate(l,s,4);QVERIFY(!dark.accepted);QCOMPARE(dark.gain,4.0);
        s=uniform({{200,200,200}},0,128);
        QVERIFY(!estimate(l,s,4).accepted);
    }
    void longSpanNormalization() {
        Plane l=uniform({{100,100,100}}), s=uniform({{25,25,25}});
        s.white=511;s.black={{32,32,32,32}};
        for(auto &v:s.pixels)v=57;
        const RGB actual=reconstruct(s,l,2,2,nullptr);
        QVERIFY(std::abs(actual[0]-25.0/959)<1e-10);
    }
    void rawFormatsAndFailure() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QString error;
        for(bool packed:{false,true}) {
            QStringList paths{frame(dir.path(),"long",packed,40000000,100000000),
                              frame(dir.path(),"short",packed,10000000,150000000)};
            const QString target=dir.path()+"/merged.jpg";
            QVERIFY2(render(target,paths,100,&error),qPrintable(error));
            QImage image(target);QCOMPARE(image.size(),QSize(10,10));
            QFile meta(dir.path()+"/merged.json");QVERIFY(meta.open(QIODevice::ReadOnly));
            auto m=QJsonDocument::fromJson(meta.readAll()).object();
            QCOMPARE(m["nominal_gain"].toDouble(),4.0);QVERIFY(m["pixels_rotated"].toBool());
            // Invalid actual exposure must fail without generating a new output.
            paths[1]=frame(dir.path(),"bad",packed,0,150000000);
            QVERIFY(!render(dir.path()+"/failed.jpg",paths,100,&error));
            QVERIFY(!QFile::exists(dir.path()+"/failed.jpg"));
            paths[1]=frame(dir.path(),"old",packed,10000000,90000000);
            QVERIFY(!render(dir.path()+"/stale.jpg",paths,100,&error));
        }
    }
};
QTEST_GUILESS_MAIN(BracketTest)
#include "rawbracket.moc"
