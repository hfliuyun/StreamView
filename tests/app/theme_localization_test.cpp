#include "analysis_tree_model.h"
#include "field_inspector.h"
#include "format_override_dialog.h"
#include "localization_manager.h"
#include "main_window.h"
#include "raw_data_view.h"
#include "rule_manager_dialog.h"
#include "theme_manager.h"
#include "timeline_table_model.h"

#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTableView>
#include <QTableWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

using namespace streamview::app;

class ThemeLocalizationTest : public QObject {
    Q_OBJECT

private slots:
    void testThemeManagerModesAndPalettes();
    void testLocalizationManagerSwitching();
    void testAllAppTrLiteralsHaveChineseTranslations();
    void testDynamicRetranslationPreservesWidgetStateAndUpdatesText();
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

    QStringList missingLiterals;
    QSet<QString> uniqueScannedLiterals;
    int totalCalls = 0;

    for (const QString& fileName : files) {
        QFile file(dir.filePath(fileName));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(file.readAll());
        const qsizetype n = content.size();
        qsizetype i = 0;
        while (i < n) {
            if (content.mid(i, 3) == QStringLiteral("tr(") &&
                (i == 0 || (!content.at(i - 1).isLetterOrNumber() && content.at(i - 1) != u'_'))) {
                i += 3;
                QStringList literalsInCall;
                QString curLit;
                bool inQuote = false;
                bool escaped = false;
                int parenDepth = 1;
                bool hasComma = false;

                while (i < n && parenDepth > 0) {
                    const QChar ch = content.at(i);
                    if (inQuote) {
                        if (escaped) {
                            if (ch == u'n') curLit.append(u'\n');
                            else if (ch == u'"') curLit.append(u'"');
                            else if (ch == u'\\') curLit.append(u'\\');
                            else if (ch == u't') curLit.append(u'\t');
                            else if (ch == u'r') curLit.append(u'\r');
                            else { curLit.append(u'\\'); curLit.append(ch); }
                            escaped = false;
                        } else if (ch == u'\\') {
                            escaped = true;
                        } else if (ch == u'"') {
                            inQuote = false;
                            if (!hasComma) {
                                literalsInCall.append(curLit);
                            }
                            curLit.clear();
                        } else {
                            curLit.append(ch);
                        }
                    } else {
                        if (ch == u'"') {
                            inQuote = true;
                        } else if (ch == u',') {
                            hasComma = true;
                        } else if (ch == u'(') {
                            ++parenDepth;
                        } else if (ch == u')') {
                            --parenDepth;
                            if (parenDepth == 0) {
                                break;
                            }
                        }
                    }
                    ++i;
                }
                if (!literalsInCall.isEmpty()) {
                    const QString fullLiteral = literalsInCall.join(QString{});
                    ++totalCalls;
                    uniqueScannedLiterals.insert(fullLiteral);
                    const std::string stdLit = fullLiteral.toStdString();
                    if (!LocalizationManager::hasChineseTranslation(stdLit)) {
                        missingLiterals.append(QStringLiteral("[%1] in %2").arg(fullLiteral, fileName));
                    }
                }
            } else {
                ++i;
            }
        }
    }

    QVERIFY2(missingLiterals.isEmpty(),
             qPrintable(QStringLiteral("Missing Chinese translations for literals:\n%1")
                            .arg(missingLiterals.join(QStringLiteral("\n")))));
    QCOMPARE(uniqueScannedLiterals.size(), static_cast<qsizetype>(LocalizationManager::chineseDictionarySize()));
    QCOMPARE(uniqueScannedLiterals.size(), qsizetype{155});
    QCOMPARE(totalCalls, 267);
}

static QModelIndex findIndexByName(const QAbstractItemModel& model,
                                   const QString& targetName,
                                   const QModelIndex& parent = {}) {
    const int count = model.rowCount(parent);
    for (int row = 0; row < count; ++row) {
        const QModelIndex idx = model.index(row, 0, parent);
        if (idx.data(Qt::DisplayRole).toString() == targetName) {
            return idx;
        }
        const QModelIndex child = findIndexByName(model, targetName, idx);
        if (child.isValid()) {
            return child;
        }
    }
    return {};
}

void ThemeLocalizationTest::testDynamicRetranslationPreservesWidgetStateAndUpdatesText() {
    auto& loc = LocalizationManager::instance();
    loc.setLanguage(Language::English);

    const QString mp4Path = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
    MainWindow window;
    QString err;
    QVERIFY2(window.openMediaSource(mp4Path, &err), qPrintable(err));

    auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
    QVERIFY(treeView != nullptr);
    auto* treeModel = qobject_cast<AnalysisTreeModel*>(treeView->model());
    QVERIFY(treeModel != nullptr);

    QModelIndex boxIndex;
    QTRY_VERIFY((boxIndex = findIndexByName(*treeModel, QStringLiteral("box[0]"))).isValid());
    treeView->setCurrentIndex(boxIndex);

    auto* fieldInspector = window.findChild<FieldInspector*>();
    QVERIFY(fieldInspector != nullptr);
    auto* form = qobject_cast<QFormLayout*>(fieldInspector->layout());
    QVERIFY(form != nullptr);

    auto* nameVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorName"));
    auto* valVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorValue"));
    auto* typeVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorType"));
    auto* widthVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorWidth"));
    auto* spansVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorSourceSpans"));
    auto* rangeVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorLogicalRange"));
    auto* descVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorDescription"));
    auto* specVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorSpecification"));
    auto* diagVal = fieldInspector->findChild<QLabel*>(QStringLiteral("fieldInspectorDiagnostics"));

    auto* nameLbl = qobject_cast<QLabel*>(form->labelForField(nameVal));
    auto* valLbl = qobject_cast<QLabel*>(form->labelForField(valVal));
    auto* typeLbl = qobject_cast<QLabel*>(form->labelForField(typeVal));
    auto* widthLbl = qobject_cast<QLabel*>(form->labelForField(widthVal));
    auto* spansLbl = qobject_cast<QLabel*>(form->labelForField(spansVal));
    auto* rangeLbl = qobject_cast<QLabel*>(form->labelForField(rangeVal));
    auto* descLbl = qobject_cast<QLabel*>(form->labelForField(descVal));
    auto* specLbl = qobject_cast<QLabel*>(form->labelForField(specVal));
    auto* diagLbl = qobject_cast<QLabel*>(form->labelForField(diagVal));
    QVERIFY(nameLbl && valLbl && typeLbl && widthLbl && spansLbl && rangeLbl && descLbl && specLbl && diagLbl);

    auto* rawDataView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
    QVERIFY(rawDataView != nullptr);
    auto* rawPageLabel = rawDataView->findChild<QLabel*>(QStringLiteral("rawPageLabel"));
    auto* prevPageBtn = rawDataView->findChild<QToolButton*>(QStringLiteral("previousPageButton"));
    auto* nextPageBtn = rawDataView->findChild<QToolButton*>(QStringLiteral("nextPageButton"));
    auto* hexBtn = rawDataView->findChild<QToolButton*>(QStringLiteral("hexModeButton"));
    auto* binBtn = rawDataView->findChild<QToolButton*>(QStringLiteral("binaryModeButton"));
    auto* combBtn = rawDataView->findChild<QToolButton*>(QStringLiteral("combinedModeButton"));
    QVERIFY(rawPageLabel && prevPageBtn && nextPageBtn && hexBtn && binBtn && combBtn);

    auto* timelineTableView = window.findChild<QTableView*>(QStringLiteral("timelineTableView"));
    QVERIFY(timelineTableView != nullptr);
    auto* timelineModel = qobject_cast<TimelineTableModel*>(timelineTableView->model());
    QVERIFY(timelineModel != nullptr);
    auto* timelineTrackCombo = window.findChild<QComboBox*>(QStringLiteral("timelineTrackComboBox"));
    QVERIFY(timelineTrackCombo != nullptr);
    auto* timelinePageLabel = window.findChild<QLabel*>(QStringLiteral("timelinePageLabel"));
    QVERIFY(timelinePageLabel != nullptr);
    auto* timelinePrevBtn = window.findChild<QToolButton*>(QStringLiteral("timelinePrevPageButton"));
    auto* timelineNextBtn = window.findChild<QToolButton*>(QStringLiteral("timelineNextPageButton"));
    QVERIFY(timelinePrevBtn && timelineNextBtn);

    QTRY_COMPARE(timelineTrackCombo->count(), 2);

    // Select Track 2 and row 1 in timeline before language switch to test selection preservation
    timelineTrackCombo->setCurrentIndex(1);
    QTRY_VERIFY(timelineModel->rowCount() > 0);
    timelineTableView->selectRow(1);
    QCOMPARE(timelineTrackCombo->currentIndex(), 1);
    QCOMPARE(timelineTableView->currentIndex().row(), 1);

    streamview::rules::RulePackageCatalog catalog;
    auto mp4Pkg = streamview::rules::loadMp4IsobmffRulePackage();
    auto h264Pkg = streamview::rules::loadH264AnnexBRulePackage();
    auto aacPkg = streamview::rules::loadAacAdtsRulePackage();
    QVERIFY(mp4Pkg.succeeded() && h264Pkg.succeeded() && aacPkg.succeeded());
    const auto mp4Identity = mp4Pkg.package->identity();
    const QByteArray adtsSource = *aacPkg.package->fileContents(QStringLiteral("src/aac_adts.svfmt"));
    QVERIFY(!adtsSource.isEmpty());
    static_cast<void>(catalog.registerPackage(std::move(*mp4Pkg.package)));
    static_cast<void>(catalog.registerPackage(std::move(*h264Pkg.package)));
    static_cast<void>(catalog.registerPackage(std::move(*aacPkg.package)));
    auto mp4Entry = streamview::rules::RuleEntryPointIdentity::create(mp4Identity, QStringLiteral("main"));
    QVERIFY(mp4Entry.has_value());

    // Register a valid third-party rule package
    const QByteArray thirdPartyManifest = QByteArrayLiteral(
        "manifest-version = 1\n\n"
        "[package]\n"
        "id = \"org.custom.adts\"\n"
        "version = \"1.0.0\"\n"
        "authors = [\"Custom\"]\n"
        "license = \"MIT\"\n"
        "dependencies = []\n\n"
        "[compatibility]\n"
        "language = \"0.1\"\n"
        "engine = \">=0.1.0 <0.2.0\"\n\n"
        "[[entrypoints]]\n"
        "id = \"adts\"\n"
        "format = \"audio.aac.adts\"\n"
        "source = \"src/adts.svfmt\"\n"
        "profiles = [\"lc\"]\n"
        "depth = \"adts-frame\"\n"
        "detector = \"aac-adts\"\n");
    auto thirdPartyPkg = streamview::rules::RulePackage::fromFiles({
        {QStringLiteral("rule.toml"), thirdPartyManifest},
        {QStringLiteral("src/adts.svfmt"), adtsSource},
    });
    QVERIFY(thirdPartyPkg.succeeded() && thirdPartyPkg.package.has_value());
    static_cast<void>(catalog.registerPackage(std::move(*thirdPartyPkg.package)));

    streamview::rules::FormatSelection fmtSel;
    fmtSel.format = streamview::rules::DetectedFormat::Mp4Isobmff;

    FormatOverrideDialog overrideDlg(&window, catalog, fmtSel, *mp4Entry);
    overrideDlg.show();
    auto* overrideList = overrideDlg.findChild<QListWidget*>(QStringLiteral("optionsListWidget"));
    QVERIFY(overrideList != nullptr && overrideList->count() >= 3);
    // Find row for third-party package
    int thirdPartyRow = -1;
    for (int r = 0; r < overrideList->count(); ++r) {
        if (overrideList->item(r)->text().contains(QStringLiteral("org.custom.adts"))) {
            thirdPartyRow = r;
            break;
        }
    }
    QVERIFY(thirdPartyRow >= 0);
    overrideList->setCurrentRow(thirdPartyRow);
    const auto selectedRuleBefore = overrideDlg.selectedRuleEntryPoint();
    QVERIFY(selectedRuleBefore.has_value());
    auto* overrideDescLabel = overrideDlg.findChild<QLabel*>(QStringLiteral("descriptionLabel"));
    QVERIFY(overrideDescLabel != nullptr);

    RuleManagerDialog ruleDlg(&window, catalog, QString(),
                              {QStringLiteral("org.streamview.mp4"),
                               QStringLiteral("org.streamview.h264"),
                               QStringLiteral("org.streamview.aac")});
    ruleDlg.show();
    auto* ruleTable = ruleDlg.findChild<QTableWidget*>(QStringLiteral("rulePackageTable"));
    QVERIFY(ruleTable != nullptr && ruleTable->rowCount() > 0);
    ruleTable->setCurrentCell(1, 0);

    // 1. Verify initial English state
    QCOMPARE(nameLbl->text(), QStringLiteral("Field"));
    QCOMPARE(valLbl->text(), QStringLiteral("Value"));
    QCOMPARE(typeLbl->text(), QStringLiteral("Type"));
    QCOMPARE(widthLbl->text(), QStringLiteral("Width"));
    QCOMPARE(spansLbl->text(), QStringLiteral("Source spans"));
    QCOMPARE(rangeLbl->text(), QStringLiteral("Logical range"));
    QCOMPARE(descLbl->text(), QStringLiteral("Description"));
    QCOMPARE(specLbl->text(), QStringLiteral("Specification"));
    QCOMPARE(diagLbl->text(), QStringLiteral("Diagnostics"));
    QCOMPARE(nameVal->text(), QStringLiteral("box[0]"));

    QCOMPARE(prevPageBtn->toolTip(), QStringLiteral("Previous source page"));
    QCOMPARE(nextPageBtn->toolTip(), QStringLiteral("Next source page"));
    QCOMPARE(hexBtn->text(), QStringLiteral("Hex"));
    QCOMPARE(binBtn->text(), QStringLiteral("Binary"));
    QCOMPARE(combBtn->text(), QStringLiteral("Combined"));
    QVERIFY(rawPageLabel->text().startsWith(QStringLiteral("Page 1 of")));
    QCOMPARE(rawDataView->model()->headerData(RawDataModel::Offset, Qt::Horizontal).toString(), QStringLiteral("Offset"));

    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Name, Qt::Horizontal).toString(), QStringLiteral("Name"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Value, Qt::Horizontal).toString(), QStringLiteral("Value"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Bits, Qt::Horizontal).toString(), QStringLiteral("Source Bits"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::State, Qt::Horizontal).toString(), QStringLiteral("State"));
    QCOMPARE(treeView->currentIndex(), boxIndex);

    QCOMPARE(timelineModel->headerData(TimelineTableModel::SampleIndex, Qt::Horizontal).toString(), QStringLiteral("Sample #"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::SyncType, Qt::Horizontal).toString(), QStringLiteral("Type"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Dts, Qt::Horizontal).toString(), QStringLiteral("DTS"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Pts, Qt::Horizontal).toString(), QStringLiteral("PTS"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Duration, Qt::Horizontal).toString(), QStringLiteral("Duration"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::SizeBytes, Qt::Horizontal).toString(), QStringLiteral("Size (B)"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::BitOffset, Qt::Horizontal).toString(), QStringLiteral("Bit Offset"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::DescriptionIndex, Qt::Horizontal).toString(), QStringLiteral("Desc Idx"));
    QCOMPARE(timelineTrackCombo->currentIndex(), 1);
    QVERIFY(timelineTrackCombo->currentText().contains(QStringLiteral("Track 2")));
    QVERIFY(timelineTrackCombo->currentText().contains(QStringLiteral("samples")));
    QCOMPARE(timelineTableView->currentIndex().row(), 1);

    auto* timelineTrackLabel = window.findChild<QLabel*>(QStringLiteral("timelineTrackLabel"));
    QVERIFY(timelineTrackLabel != nullptr);
    QCOMPARE(timelineTrackLabel->text(), QStringLiteral("Track:"));
    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Analysis finished with partial results:")));

    QCOMPARE(overrideDlg.windowTitle(), QStringLiteral("Override Format"));
    QVERIFY(overrideDlg.findChild<QLabel*>(QStringLiteral("headerLabel"))->text().contains(QStringLiteral("Select the target format")));
    QCOMPARE(overrideDlg.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))->button(QDialogButtonBox::Ok)->text(), QStringLiteral("Override"));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Package: org.custom.adts")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Version: 1.0.0")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Entry Point: adts")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Format: audio.aac.adts")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Hash:")));

    QCOMPARE(ruleDlg.windowTitle(), QStringLiteral("Rule Package Manager"));
    QCOMPARE(ruleTable->currentRow(), 1);

    // 2. Switch to Chinese
    loc.setLanguage(Language::SimplifiedChinese);
    QCOMPARE(loc.currentLanguage(), Language::SimplifiedChinese);

    QCOMPARE(nameLbl->text(), QString::fromUtf8("字段"));
    QCOMPARE(valLbl->text(), QString::fromUtf8("值"));
    QCOMPARE(typeLbl->text(), QString::fromUtf8("类型"));
    QCOMPARE(widthLbl->text(), QString::fromUtf8("位宽"));
    QCOMPARE(spansLbl->text(), QString::fromUtf8("源区间"));
    QCOMPARE(rangeLbl->text(), QString::fromUtf8("逻辑范围"));
    QCOMPARE(descLbl->text(), QString::fromUtf8("描述"));
    QCOMPARE(specLbl->text(), QString::fromUtf8("规范"));
    QCOMPARE(diagLbl->text(), QString::fromUtf8("诊断"));
    QCOMPARE(nameVal->text(), QStringLiteral("box[0]"));

    QCOMPARE(prevPageBtn->toolTip(), QString::fromUtf8("上一源页面"));
    QCOMPARE(nextPageBtn->toolTip(), QString::fromUtf8("下一源页面"));
    QCOMPARE(hexBtn->text(), QString::fromUtf8("十六进制"));
    QCOMPARE(binBtn->text(), QString::fromUtf8("二进制"));
    QCOMPARE(combBtn->text(), QString::fromUtf8("组合"));
    QVERIFY(rawPageLabel->text().startsWith(QString::fromUtf8("第 1 页，共")));
    QCOMPARE(rawDataView->model()->headerData(RawDataModel::Offset, Qt::Horizontal).toString(), QString::fromUtf8("偏移"));

    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Name, Qt::Horizontal).toString(), QString::fromUtf8("名称"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Value, Qt::Horizontal).toString(), QString::fromUtf8("值"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Bits, Qt::Horizontal).toString(), QString::fromUtf8("源比特"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::State, Qt::Horizontal).toString(), QString::fromUtf8("状态"));
    QCOMPARE(treeView->currentIndex(), boxIndex);

    QCOMPARE(timelineModel->headerData(TimelineTableModel::SampleIndex, Qt::Horizontal).toString(), QString::fromUtf8("样本序号"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::SyncType, Qt::Horizontal).toString(), QString::fromUtf8("类型"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Dts, Qt::Horizontal).toString(), QString::fromUtf8("DTS"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Pts, Qt::Horizontal).toString(), QString::fromUtf8("PTS"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Duration, Qt::Horizontal).toString(), QString::fromUtf8("时长"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::SizeBytes, Qt::Horizontal).toString(), QString::fromUtf8("大小 (字节)"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::BitOffset, Qt::Horizontal).toString(), QString::fromUtf8("位偏移"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::DescriptionIndex, Qt::Horizontal).toString(), QString::fromUtf8("描述符索引"));

    // Verify track selection, combo text, and table selection row are preserved in Chinese
    QCOMPARE(timelineTrackCombo->currentIndex(), 1);
    QVERIFY(timelineTrackCombo->currentText().contains(QString::fromUtf8("轨道 2")));
    QVERIFY(timelineTrackCombo->currentText().contains(QString::fromUtf8("样本")));
    QCOMPARE(timelineTableView->currentIndex().row(), 1);
    QCOMPARE(timelineTrackLabel->text(), QString::fromUtf8("轨道:"));
    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QString::fromUtf8("分析完成（部分结果）：")));

    QCOMPARE(overrideDlg.windowTitle(), QString::fromUtf8("覆盖格式"));
    QVERIFY(overrideDlg.findChild<QLabel*>(QStringLiteral("headerLabel"))->text().contains(QString::fromUtf8("选择目标格式")));
    QCoreApplication::processEvents();
    QCOMPARE(overrideDlg.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))->button(QDialogButtonBox::Ok)->text(), QString::fromUtf8("覆盖"));
    auto selectedRuleChinese = overrideDlg.selectedRuleEntryPoint();
    QVERIFY(selectedRuleChinese.has_value());
    QCOMPARE(*selectedRuleChinese, *selectedRuleBefore);
    // Verify third-party package description labels are translated to Chinese
    QVERIFY(overrideDescLabel->text().contains(QString::fromUtf8("规则包: org.custom.adts")));
    QVERIFY(overrideDescLabel->text().contains(QString::fromUtf8("版本: 1.0.0")));
    QVERIFY(overrideDescLabel->text().contains(QString::fromUtf8("入口点: adts")));
    QVERIFY(overrideDescLabel->text().contains(QString::fromUtf8("格式: audio.aac.adts")));
    QVERIFY(overrideDescLabel->text().contains(QString::fromUtf8("哈希:")));

    QCOMPARE(ruleDlg.windowTitle(), QString::fromUtf8("规则包管理器"));
    QVERIFY(ruleDlg.findChild<QLabel*>(QStringLiteral("headerLabel"))->text().contains(QString::fromUtf8("管理格式规则包")));
    QCOMPARE(ruleDlg.findChild<QPushButton*>(QStringLiteral("installPackageButton"))->text(), QString::fromUtf8("安装规则包 (.svrule)..."));
    QCOMPARE(ruleTable->horizontalHeaderItem(0)->text(), QStringLiteral("ID"));
    QCOMPARE(ruleTable->horizontalHeaderItem(2)->text(), QString::fromUtf8("来源"));

    int bundledRow = -1;
    int installedRow = -1;
    for (int r = 0; r < ruleTable->rowCount(); ++r) {
        if (ruleTable->item(r, 0)->text() == QStringLiteral("org.streamview.mp4")) {
            bundledRow = r;
        } else if (ruleTable->item(r, 0)->text() == QStringLiteral("org.custom.adts")) {
            installedRow = r;
        }
    }
    QVERIFY(bundledRow >= 0 && installedRow >= 0);
    QCOMPARE(ruleTable->item(bundledRow, 2)->text(), QString::fromUtf8("[内置]"));
    QCOMPARE(ruleTable->item(installedRow, 2)->text(), QString::fromUtf8("[已安装]"));
    QCOMPARE(ruleTable->currentRow(), 1);

    // 3. Switch back to English
    loc.setLanguage(Language::English);
    QCOMPARE(loc.currentLanguage(), Language::English);

    QCOMPARE(ruleTable->item(bundledRow, 2)->text(), QStringLiteral("[Bundled]"));
    QCOMPARE(ruleTable->item(installedRow, 2)->text(), QStringLiteral("[Installed]"));

    QCOMPARE(nameLbl->text(), QStringLiteral("Field"));
    QCOMPARE(valLbl->text(), QStringLiteral("Value"));
    QCOMPARE(typeLbl->text(), QStringLiteral("Type"));
    QCOMPARE(widthLbl->text(), QStringLiteral("Width"));
    QCOMPARE(spansLbl->text(), QStringLiteral("Source spans"));
    QCOMPARE(rangeLbl->text(), QStringLiteral("Logical range"));
    QCOMPARE(descLbl->text(), QStringLiteral("Description"));
    QCOMPARE(specLbl->text(), QStringLiteral("Specification"));
    QCOMPARE(diagLbl->text(), QStringLiteral("Diagnostics"));
    QCOMPARE(nameVal->text(), QStringLiteral("box[0]"));

    QCOMPARE(prevPageBtn->toolTip(), QStringLiteral("Previous source page"));
    QCOMPARE(nextPageBtn->toolTip(), QStringLiteral("Next source page"));
    QCOMPARE(hexBtn->text(), QStringLiteral("Hex"));
    QCOMPARE(binBtn->text(), QStringLiteral("Binary"));
    QCOMPARE(combBtn->text(), QStringLiteral("Combined"));
    QVERIFY(rawPageLabel->text().startsWith(QStringLiteral("Page 1 of")));
    QCOMPARE(rawDataView->model()->headerData(RawDataModel::Offset, Qt::Horizontal).toString(), QStringLiteral("Offset"));

    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Name, Qt::Horizontal).toString(), QStringLiteral("Name"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Value, Qt::Horizontal).toString(), QStringLiteral("Value"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::Bits, Qt::Horizontal).toString(), QStringLiteral("Source Bits"));
    QCOMPARE(treeModel->headerData(AnalysisTreeModel::State, Qt::Horizontal).toString(), QStringLiteral("State"));
    QCOMPARE(treeView->currentIndex(), boxIndex);

    QCOMPARE(timelineModel->headerData(TimelineTableModel::SampleIndex, Qt::Horizontal).toString(), QStringLiteral("Sample #"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::SyncType, Qt::Horizontal).toString(), QStringLiteral("Type"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Dts, Qt::Horizontal).toString(), QStringLiteral("DTS"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Pts, Qt::Horizontal).toString(), QStringLiteral("PTS"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::Duration, Qt::Horizontal).toString(), QStringLiteral("Duration"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::SizeBytes, Qt::Horizontal).toString(), QStringLiteral("Size (B)"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::BitOffset, Qt::Horizontal).toString(), QStringLiteral("Bit Offset"));
    QCOMPARE(timelineModel->headerData(TimelineTableModel::DescriptionIndex, Qt::Horizontal).toString(), QStringLiteral("Desc Idx"));

    // Verify track selection, combo text, and table selection row are preserved in English
    QCOMPARE(timelineTrackCombo->currentIndex(), 1);
    QVERIFY(timelineTrackCombo->currentText().contains(QStringLiteral("Track 2")));
    QVERIFY(timelineTrackCombo->currentText().contains(QStringLiteral("samples")));
    QCOMPARE(timelineTableView->currentIndex().row(), 1);
    QCOMPARE(timelineTrackLabel->text(), QStringLiteral("Track:"));
    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Analysis finished with partial results:")));

    QCoreApplication::processEvents();
    QCOMPARE(overrideDlg.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))->button(QDialogButtonBox::Ok)->text(), QStringLiteral("Override"));
    auto selectedRuleEnglish = overrideDlg.selectedRuleEntryPoint();
    QVERIFY(selectedRuleEnglish.has_value());
    QCOMPARE(*selectedRuleEnglish, *selectedRuleBefore);
    QCOMPARE(overrideDlg.windowTitle(), QStringLiteral("Override Format"));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Package: org.custom.adts")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Version: 1.0.0")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Entry Point: adts")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Format: audio.aac.adts")));
    QVERIFY(overrideDescLabel->text().contains(QStringLiteral("Hash:")));

    QCOMPARE(ruleDlg.windowTitle(), QStringLiteral("Rule Package Manager"));
    QCOMPARE(ruleTable->currentRow(), 1);

    // 4. Test FieldInspector clear placeholder retranslation
    fieldInspector->clear();
    QCOMPARE(nameVal->text(), QStringLiteral("No field selected."));
    loc.setLanguage(Language::SimplifiedChinese);
    QCOMPARE(nameVal->text(), QString::fromUtf8("未选择字段。"));
    loc.setLanguage(Language::English);
    QCOMPARE(nameVal->text(), QStringLiteral("No field selected."));

    // 5. Test Elementary Stream timeline status label retranslation
    QTemporaryDir esDir;
    QVERIFY(esDir.isValid());
    const QString esPath = esDir.filePath(QStringLiteral("valid.264"));
    {
        QFile file(esPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("00000165"));
    }
    MainWindow esWindow;
    QString esErr;
    QVERIFY2(esWindow.openMediaSource(esPath, &esErr), qPrintable(esErr));
    auto* esStatusLabel = esWindow.findChild<QLabel*>(QStringLiteral("timelineStatusLabel"));
    QVERIFY(esStatusLabel != nullptr);
    QCOMPARE(esStatusLabel->text(), QStringLiteral("Container tracks unavailable for elementary stream"));

    loc.setLanguage(Language::SimplifiedChinese);
    QCOMPARE(esStatusLabel->text(), QString::fromUtf8("基本流无容器轨道"));

    loc.setLanguage(Language::English);
    QCOMPARE(esStatusLabel->text(), QStringLiteral("Container tracks unavailable for elementary stream"));
}

QTEST_MAIN(ThemeLocalizationTest)
#include "theme_localization_test.moc"
