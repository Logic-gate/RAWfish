// SPDX-License-Identifier: BSD-3-Clause
#include "latestcapture.h"

#include <QDir>
#include <QFileInfo>

LatestCapture::LatestCapture(QObject *parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(400);
    connect(&m_debounce, &QTimer::timeout, this, &LatestCapture::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce,
            static_cast<void (QTimer::*)()>(&QTimer::start));
}

void LatestCapture::setFolder(const QString &folder)
{
    if (folder == m_folder)
        return;
    if (!m_folder.isEmpty())
        m_watcher.removePath(m_folder);
    m_folder = folder;
    if (!m_folder.isEmpty()) {
        QDir().mkpath(m_folder);
        m_watcher.addPath(m_folder);
    }
    emit folderChanged();
    refresh();
}

void LatestCapture::refresh()
{
    QUrl newest;
    if (!m_folder.isEmpty()) {
        const QFileInfoList files = QDir(m_folder).entryInfoList(
                    QStringList() << QStringLiteral("*.jpg") << QStringLiteral("*.jpeg"),
                    QDir::Files | QDir::NoDotAndDotDot, QDir::Time);
        for (const QFileInfo &info : files) {
            if (info.size() > 0 && !info.fileName().startsWith(QLatin1Char('.'))) {
                newest = QUrl::fromLocalFile(info.absoluteFilePath());
                break;
            }
        }
    }
    if (newest != m_url) {
        m_url = newest;
        emit urlChanged();
    }
}
