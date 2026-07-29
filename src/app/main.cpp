#include "main_window.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>

#include <utility>

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

    streamview::app::AnalysisSessionCacheOptions cacheOptions;
    const QString cacheLocation =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cacheLocation.isEmpty()) {
        cacheOptions.databasePath =
            QDir(cacheLocation).filePath(QStringLiteral("analysis-cache.sqlite"));
    }
    streamview::app::MainWindow window(std::move(cacheOptions));
    window.show();
    return application.exec();
}
