// SPDX-License-Identifier: BSD-3-Clause
#ifndef LATESTCAPTURE_H
#define LATESTCAPTURE_H

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

// Tracks the newest JPEG in the capture folder, for the gallery thumbnail beside the
// shutter. Watches the folder and re-scans (debounced) when it changes.
class LatestCapture : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QUrl url READ url NOTIFY urlChanged)

public:
    explicit LatestCapture(QObject *parent = nullptr);

    QString folder() const { return m_folder; }
    void setFolder(const QString &folder);
    QUrl url() const { return m_url; }

    Q_INVOKABLE void refresh();

signals:
    void folderChanged();
    void urlChanged();

private:
    QString m_folder;
    QUrl m_url;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};

#endif
