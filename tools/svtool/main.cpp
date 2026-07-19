#include <streamview/core/version.h>
#include <streamview/rules/language_version.h>

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();

    if (arguments.size() == 2 &&
        (arguments.at(1) == QStringLiteral("--version") ||
         arguments.at(1) == QStringLiteral("version"))) {
        QTextStream(stdout) << "svtool " << streamview::core::version() << " (DSL "
                            << streamview::rules::languageVersion() << ")\n";
        return 0;
    }

    QTextStream(stderr) << "Usage: svtool --version\n";
    return 2;
}
