// SPDX-License-Identifier: BSD-3-Clause
#ifndef LOOKSMODEL_H
#define LOOKSMODEL_H

#include <QAbstractListModel>
#include <QQuickImageProvider>
#include <QString>
#include <QStringList>
#include <QVector>

// Lists colour looks (.cube / .plut files) from a folder for the looks strip. The
// first row is always "Standard" (no LUT, empty path).
class LooksModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles { NameRole = Qt::UserRole + 1, PathRole, ThumbnailRole };

    explicit LooksModel(QObject *parent = nullptr);

    QString folder() const { return m_folder; }
    void setFolder(const QString &folder);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE int indexOfPath(const QString &path) const;
    Q_INVOKABLE QString nameForPath(const QString &path) const;
    static QString defaultFolder();
    static QString displayName(const QString &fileName);

signals:
    void folderChanged();
    void countChanged();

private:
    struct Look {
        QString name;
        QString path;
    };
    QString m_folder;
    QVector<Look> m_looks;
};

// image://looks/<absolute path of a LUT file>: a small sample picture graded with
// that look. An empty id renders the ungraded sample.
class LookThumbnailProvider : public QQuickImageProvider
{
public:
    LookThumbnailProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    static QImage sampleScene(int side);
};

#endif
