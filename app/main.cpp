/*
 * main.cpp -- the indRAW Sailfish application entry point.
 *
 * Registers the IndrawPreview viewfinder item as a QML type and shows the main
 * QML page. Uses Sailfish's standard boosted application + view helpers.
 */
#include <QtQuick>
#include <sailfishapp.h>

#include "indrawpreview.h"

int main(int argc, char *argv[])
{
    /* SailfishApp sets up the QGuiApplication and the QML engine the way the
     * platform expects (boost, theming, lifecycle). */
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));

    /* Expose the C++ viewfinder to QML as IndrawPreview 1.0. */
    qmlRegisterType<IndrawPreview>("com.indraw", 1, 0, "IndrawPreview");

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    /* The QML installs to /usr/share/indraw/qml/ (the Sailjail ApplicationName
     * is "indraw", and the sandbox only shows /usr/share/<ApplicationName>; the
     * spec moves it there). SailfishApp::pathTo keys off the binary name
     * (indraw-app), so point at the file directly. */
    view->setSource(QUrl::fromLocalFile(
        QStringLiteral("/usr/share/indraw/qml/indraw.qml")));
    view->show();

    return app->exec();
}
