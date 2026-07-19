#include "main_window.h"

#include <QApplication>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("StreamView"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("StreamView"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("streamview.org"));

    if (application.arguments().contains(QStringLiteral("--version"))) {
        QTextStream(stdout) << "StreamView " << QCoreApplication::applicationVersion() << '\n';
        return 0;
    }

    streamview::app::MainWindow window;
    window.show();
    return application.exec();
}
