#include "localization_manager.h"
#include "theme_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

using namespace streamview::app;

class ThemeLocalizationTest : public QObject {
    Q_OBJECT

private slots:
    void testThemeManagerModesAndPalettes();
    void testLocalizationManagerSwitching();
    void testAllAppTrLiteralsHaveChineseTranslations();
};

void ThemeLocalizationTest::testThemeManagerModesAndPalettes() {
    auto& manager = ThemeManager::instance();
    const auto initialMode = manager.themeMode();

    const auto lightPalette = ThemeManager::createLightPalette();
    const auto darkPalette = ThemeManager::createDarkPalette();

    QVERIFY(lightPalette.color(QPalette::Window) != darkPalette.color(QPalette::Window));
    QVERIFY(lightPalette.color(QPalette::Text) != darkPalette.color(QPalette::Text));

    QSignalSpy spy(&manager, &ThemeManager::themeChanged);

    manager.setThemeMode(ThemeMode::Light);
    QCOMPARE(manager.themeMode(), ThemeMode::Light);
    QCOMPARE(manager.isDark(), false);
    QVERIFY(!spy.isEmpty());

    spy.clear();
    manager.setThemeMode(ThemeMode::Dark);
    QCOMPARE(manager.themeMode(), ThemeMode::Dark);
    QCOMPARE(manager.isDark(), true);
    QVERIFY(!spy.isEmpty());

    // Restore initial mode
    manager.setThemeMode(initialMode);
}

void ThemeLocalizationTest::testLocalizationManagerSwitching() {
    auto& loc = LocalizationManager::instance();
    const auto initialLanguage = loc.currentLanguage();

    loc.setLanguage(Language::English);
    QCOMPARE(loc.currentLanguage(), Language::English);

    // In English, translations resolve to original text
    const QString enFile = QCoreApplication::translate("streamview::app::MainWindow", "&File");
    QCOMPARE(enFile, QStringLiteral("&File"));

    QSignalSpy spy(&loc, &LocalizationManager::languageChanged);

    loc.setLanguage(Language::SimplifiedChinese);
    QCOMPARE(loc.currentLanguage(), Language::SimplifiedChinese);
    QCOMPARE(spy.count(), 1);

    // In Chinese, translations resolve to localized text
    const QString zhFile = QCoreApplication::translate("streamview::app::MainWindow", "&File");
    QCOMPARE(zhFile, QString::fromUtf8("文件(&F)"));

    const QString zhDiag = QCoreApplication::translate("streamview::app::DiagnosticsSummaryDock", "Diagnostics");
    QCOMPARE(zhDiag, QString::fromUtf8("诊断"));

    // Switch back to English
    loc.setLanguage(Language::English);
    QCOMPARE(loc.currentLanguage(), Language::English);

    const QString enFileRestored = QCoreApplication::translate("streamview::app::MainWindow", "&File");
    QCOMPARE(enFileRestored, QStringLiteral("&File"));

    // Restore initial language
    loc.setLanguage(initialLanguage);
}

void ThemeLocalizationTest::testAllAppTrLiteralsHaveChineseTranslations() {
    const QString appDir = QString::fromLatin1(STREAMVIEW_SOURCE_DIR "/src/app");
    QDir dir(appDir);
    const QStringList files = dir.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files);
    QVERIFY(!files.isEmpty());

    const QRegularExpression trRegex(QStringLiteral(R"raw(\btr\(\s*"((?:[^"\\]|\\.)*)"\s*\))raw"));

    QStringList missingLiterals;
    int inspectedCount = 0;

    for (const QString& fileName : files) {
        QFile file(dir.filePath(fileName));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(file.readAll());
        auto matchIterator = trRegex.globalMatch(content);
        while (matchIterator.hasNext()) {
            const auto match = matchIterator.next();
            QString rawLiteral = match.captured(1);
            // Unescape C++ string escapes
            rawLiteral.replace(QStringLiteral(R"(\n)"), QStringLiteral("\n"));
            rawLiteral.replace(QStringLiteral(R"(\")"), QStringLiteral("\""));
            rawLiteral.replace(QStringLiteral(R"(\\)"), QStringLiteral("\\"));
            ++inspectedCount;

            const std::string stdLiteral = rawLiteral.toStdString();
            if (!LocalizationManager::hasChineseTranslation(stdLiteral)) {
                missingLiterals.append(QStringLiteral("[%1] in %2").arg(rawLiteral, fileName));
            }
        }
    }

    QVERIFY(inspectedCount >= 140);
    QVERIFY2(missingLiterals.isEmpty(),
             qPrintable(QStringLiteral("Missing Chinese translations for literals:\n%1")
                            .arg(missingLiterals.join(QStringLiteral("\n")))));
}

QTEST_MAIN(ThemeLocalizationTest)
#include "theme_localization_test.moc"
