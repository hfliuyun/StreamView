#include "main_window.h"

#include <streamview/core/version.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QSqlError>
#include <QTextStream>

#include <utility>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("StreamView"));
    QCoreApplication::setApplicationVersion(streamview::core::version());
    QCoreApplication::setOrganizationName(QStringLiteral("StreamView"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("streamview.org"));

    if (application.arguments().contains(QStringLiteral("--version"))) {
        QTextStream(stdout) << "StreamView " << QCoreApplication::applicationVersion() << '\n';
        return 0;
    }

    if (application.arguments().contains(QStringLiteral("--check-runtime"))) {
        const bool rulesAvailable =
            streamview::rules::loadH264AnnexBRulePackage().succeeded() &&
            streamview::rules::loadAacAdtsRulePackage().succeeded() &&
            streamview::rules::loadMp4IsobmffRulePackage().succeeded();
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
        database.setDatabaseName(QStringLiteral(":memory:"));
        if (!rulesAvailable || !database.open()) {
            QTextStream(stderr) << "StreamView runtime check failed: "
                                << database.lastError().text() << '\n';
            return 1;
        }
        QTextStream(stdout) << "StreamView runtime OK\n";
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
