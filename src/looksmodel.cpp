// SPDX-License-Identifier: BSD-3-Clause
#include "looksmodel.h"

#include "colorlut.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QStandardPaths>

#include <memory>

LooksModel::LooksModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_folder(defaultFolder())
{
    refresh();
}

QString LooksModel::defaultFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
            + QStringLiteral("/RAWfish/looks");
}

QString LooksModel::displayName(const QString &fileName)
{
    QString name = QFileInfo(fileName).completeBaseName();
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    return name;
}

void LooksModel::setFolder(const QString &folder)
{
    if (m_folder == folder)
        return;
    m_folder = folder;
    emit folderChanged();
    refresh();
}

void LooksModel::refresh()
{
    beginResetModel();
    m_looks.clear();
    m_looks.append({ QStringLiteral("Standard"), QString() });
    if (!m_folder.isEmpty()) {
        QDir dir(m_folder);
        if (!dir.exists())
            QDir().mkpath(m_folder); // lets the user see where to drop looks
        const QFileInfoList files = dir.entryInfoList(
                    QStringList() << QStringLiteral("*.cube") << QStringLiteral("*.plut"),
                    QDir::Files | QDir::Readable, QDir::IgnoreCase | QDir::Name);
        for (const QFileInfo &info : files)
            m_looks.append({ displayName(info.fileName()), info.absoluteFilePath() });
    }
    endResetModel();
    emit countChanged();
}

int LooksModel::indexOfPath(const QString &path) const
{
    for (int i = 0; i < m_looks.size(); ++i) {
        if (m_looks.at(i).path == path)
            return i;
    }
    return 0;
}

QString LooksModel::nameForPath(const QString &path) const
{
    return m_looks.at(indexOfPath(path)).name;
}

int LooksModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_looks.size();
}

QVariant LooksModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_looks.size())
        return QVariant();
    const Look &look = m_looks.at(index.row());
    switch (role) {
    case NameRole:
        return look.name;
    case PathRole:
        return look.path;
    case ThumbnailRole:
        return QStringLiteral("image://looks/") + look.path;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> LooksModel::roleNames() const
{
    return { { NameRole, "name" }, { PathRole, "path" }, { ThumbnailRole, "thumbnail" } };
}

LookThumbnailProvider::LookThumbnailProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

// A synthetic scene with sky, warm light, foliage, a skin-toned subject and a grey
// ramp, so every look shows how it treats the main colour families.
QImage LookThumbnailProvider::sampleScene(int side)
{
    QImage image(side, side, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient sky(0, 0, 0, side * 0.62);
    sky.setColorAt(0.0, QColor(70, 120, 190));
    sky.setColorAt(1.0, QColor(240, 200, 150));
    p.fillRect(QRectF(0, 0, side, side * 0.62), sky);
    QLinearGradient ground(0, side * 0.62, 0, side * 0.86);
    ground.setColorAt(0.0, QColor(70, 110, 55));
    ground.setColorAt(1.0, QColor(40, 70, 35));
    p.fillRect(QRectF(0, side * 0.62, side, side * 0.24), ground);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(224, 172, 140));
    p.drawEllipse(QPointF(side * 0.34, side * 0.50), side * 0.13, side * 0.15);
    p.setBrush(QColor(190, 60, 50));
    p.drawRect(QRectF(side * 0.58, side * 0.40, side * 0.18, side * 0.22));
    p.setBrush(QColor(235, 235, 235));
    p.drawEllipse(QPointF(side * 0.78, side * 0.20), side * 0.07, side * 0.07);
    const int steps = 8;
    for (int i = 0; i < steps; ++i) {
        const int v = i * 255 / (steps - 1);
        p.setBrush(QColor(v, v, v));
        p.drawRect(QRectF(side * i / steps, side * 0.86, side / steps + 1, side * 0.14));
    }
    p.end();
    return image;
}

QImage LookThumbnailProvider::requestImage(const QString &id, QSize *size,
                                           const QSize &requestedSize)
{
    const int side = requestedSize.isValid() && requestedSize.width() > 0
            ? qBound(32, requestedSize.width(), 256) : 96;
    QImage image = sampleScene(side);
    if (!id.isEmpty()) {
        QFile file(id);
        if (file.open(QIODevice::ReadOnly) && file.size() < 32 * 1024 * 1024) {
            const QByteArray data = file.readAll();
            ColorLut lut;
            std::string error;
            if (ColorLut::parse(data.constData(), size_t(data.size()), &lut, &error)) {
                for (int y = 0; y < image.height(); ++y) {
                    lut.apply(reinterpret_cast<uint32_t *>(image.scanLine(y)),
                              size_t(image.width()));
                }
            }
        }
    }
    if (size)
        *size = image.size();
    return image;
}
