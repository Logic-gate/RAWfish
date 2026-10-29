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
    /* The QML installs to /usr/share/rawfish-ir/qml/ (the Sailjail
     * ApplicationName is "rawfish-ir", the sandbox only shows
     * /usr/share/<ApplicationName>, and the binary has that name too). The
     * QML files keep the engine's development name, indraw. */
    view->setSource(QUrl::fromLocalFile(
        QStringLiteral("/usr/share/rawfish-ir/qml/indraw.qml")));
    view->show();

    return app->exec();
}
