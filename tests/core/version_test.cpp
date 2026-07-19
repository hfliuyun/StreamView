#include <streamview/core/version.h>
#include <streamview/rules/language_version.h>

#include <QTest>

class VersionTest final : public QObject {
    Q_OBJECT

private slots:
    void exposesIndependentVersions() {
        QCOMPARE(streamview::core::version(), QStringLiteral("0.1.0"));
        QCOMPARE(streamview::rules::languageVersion(), QStringLiteral("0.1"));
    }
};

QTEST_GUILESS_MAIN(VersionTest)

#include "version_test.moc"
