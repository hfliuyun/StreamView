#include "analysis_session.h"
#include "main_window.h"
#include "session_document.h"

#include <streamview/core/source.h>
#include <streamview/core/source_fingerprint.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/rule_catalog.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <io.h>
#include <winioctl.h>
#endif

using namespace streamview::app;
using namespace streamview::core;
using namespace streamview::rules;

namespace {

bool makeFileSparse(QFile& file) {
#if defined(Q_OS_WIN)
    const int descriptor = file.handle();
    if (descriptor < 0) {
        return false;
    }
    const intptr_t native = _get_osfhandle(descriptor);
    if (native == -1) {
        return false;
    }
    DWORD bytesReturned = 0;
    return DeviceIoControl(reinterpret_cast<HANDLE>(native),
                           FSCTL_SET_SPARSE,
                           nullptr, 0,
                           nullptr, 0,
                           &bytesReturned,
                           nullptr) != 0;
#else
    Q_UNUSED(file);
    return true;
#endif
}

QByteArray makeAmbiguousMp4Bytes() {
    constexpr quint32 firstBoxSize = 0x0105U;
    QByteArray bytes;
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x01));
    bytes.append(char(0x05));
    bytes.append("free", 4);
    for (quint32 i = 0; i < firstBoxSize - 8U; ++i) {
        if (i == 100U) {
            bytes.append(char(0x00));
            bytes.append(char(0x00));
            bytes.append(char(0x01));
            bytes.append(char(0x67));
            i += 3U;
            continue;
        }
        bytes.append(char(0xEE));
    }
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x18));
    bytes.append("ftyp", 4);
    bytes.append(QByteArray(16, '\0'));
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x28));
    bytes.append("mdat", 4);
    bytes.append(QByteArray(32, '3'));
    return bytes;
}

} // namespace

class SessionPersistenceRegressionTest final : public QObject {
    Q_OBJECT

private slots:
    void testGoldenV1SessionPermanentlyReadable();
    void test100GbVirtualSparseSourceSessionPersistenceAndRestoration();
    void testFormatOverrideFullLifecyclePersistenceAndReplay();
    void testUnsavedModificationsInteractiveGuardrailBranches();
};

void SessionPersistenceRegressionTest::testGoldenV1SessionPermanentlyReadable() {
    const QString goldenPath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/v1_golden.svsession");
    QVERIFY2(QFile::exists(goldenPath), "v1_golden.svsession fixture must exist permanently in tests/fixtures/");

    const auto loaded = SessionDocument::load(goldenPath);
    QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));

    const auto& doc = *loaded.document;
    QCOMPARE(doc.schemaVersion(), 1U);
    QCOMPARE(doc.sourcePath(), QStringLiteral("sample_stream.264"));
    QCOMPARE(doc.sourceIdentity(), QStringLiteral("sample_stream.264"));

    const auto& fp = doc.sourceFingerprint();
    QCOMPARE(fp.version(), 1U);
    QCOMPARE(fp.mode(), SourceFingerprintMode::FullContentSha256);
    QCOMPARE(fp.sizeBytes(), 1024ULL);
    QCOMPARE(fp.digestText(), QStringLiteral("4b227777d4dd1fc61c6f884f48641d02b4d121d3fd328cb08b5531fcacdabf8a"));

    const auto& rule = doc.ruleIdentity();
    QCOMPARE(rule.packageIdentity().packageId(), QStringLiteral("org.streamview.h264"));
    QCOMPARE(rule.packageIdentity().packageVersion(), QStringLiteral("0.1.40"));
    QCOMPARE(rule.entryPointId(), QStringLiteral("annex-b"));

    const auto& user = doc.userState();
    QCOMPARE(user.bookmarks.size(), std::size_t{1});
    QCOMPARE(user.bookmarks[0].label, QStringLiteral("NAL Header"));
    QCOMPARE(user.bookmarks[0].sourceBitOffset, 32ULL);

    QCOMPARE(user.annotations.size(), std::size_t{1});
    QCOMPARE(user.annotations[0].text, QStringLiteral("Sequence parameter set"));
    QCOMPARE(user.annotations[0].sourceBitOffset, 32ULL);
    QCOMPARE(user.annotations[0].bitLength, 8ULL);

    QCOMPARE(user.expandedPaths.size(), 2);
    QCOMPARE(user.expandedPaths[0], QStringLiteral("/0"));
    QCOMPARE(user.expandedPaths[1], QStringLiteral("/0/0"));

    QCOMPARE(user.view.rawDisplayMode, RawDisplayMode::Hex);
    QCOMPARE(user.view.rawPageIndex, 0ULL);
    QCOMPARE(user.view.selectedSourceBitOffset, std::optional<quint64>{32ULL});
    QCOMPARE(user.view.selectedAnalysisPath, std::optional<QString>{QStringLiteral("/0/0")});

    // Verify idempotence: re-serializing and re-parsing yields identical document
    const QByteArray reSerialized = doc.toJson();
    const auto reParsed = SessionDocument::parse(reSerialized);
    QVERIFY2(reParsed.succeeded(), qPrintable(reParsed.errorMessage));
    QCOMPARE(reParsed.document->sourcePath(), doc.sourcePath());
    QCOMPARE(reParsed.document->sourceIdentity(), doc.sourceIdentity());
    QCOMPARE(reParsed.document->sourceFingerprint(), doc.sourceFingerprint());
    QCOMPARE(reParsed.document->ruleIdentity(), doc.ruleIdentity());
    QVERIFY(reParsed.document->userState() == doc.userState());
}

void SessionPersistenceRegressionTest::test100GbVirtualSparseSourceSessionPersistenceAndRestoration() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString sparsePath = tempDir.filePath(QStringLiteral("virtual_100gb.bin"));

    // Construct OS-sparse file of exactly 100 GiB with prefix data
    constexpr quint64 hundredGb = 100ULL * 1024ULL * 1024ULL * 1024ULL;
    {
        QFile sparseFile(sparsePath);
        QVERIFY(sparseFile.open(QIODevice::ReadWrite));
        makeFileSparse(sparseFile);

        QFile prefixFile(QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j6_100gb_sparse_prefix.bin"));
        QVERIFY(prefixFile.open(QIODevice::ReadOnly));
        const QByteArray prefixData = prefixFile.readAll();
        QVERIFY(!prefixData.isEmpty());
        sparseFile.write(prefixData);

        // Sparse resize to 100 GiB
        const bool resized = sparseFile.resize(static_cast<qint64>(hundredGb));
        if (!resized) {
            QSKIP("Underlying filesystem cannot allocate 100 GiB sparse file");
        }
        sparseFile.seek(static_cast<qint64>(hundredGb - 4));
        sparseFile.write("TAIL");
        sparseFile.close();
    }

    QCOMPARE(static_cast<quint64>(QFileInfo(sparsePath).size()), hundredGb);

    // Compute sampled fingerprint without allocating 100 GB memory
    auto fileSource = FileSource::open(sparsePath);
    QVERIFY(fileSource != nullptr);
    const auto fpResult = fileSource->fingerprint();
    QVERIFY(fpResult.succeeded());
    QCOMPARE(fpResult.fingerprint->sizeBytes(), hundredGb);
    QCOMPARE(fpResult.fingerprint->mode(), SourceFingerprintMode::SampledSha256);

    // Build session document
    auto package = RulePackageIdentity::create(QStringLiteral("org.streamview.mp4"),
                                               QStringLiteral("0.1.0"),
                                               QByteArray(32, '\x44'));
    QVERIFY(package.has_value());
    auto ruleEntry = RuleEntryPointIdentity::create(*package, QStringLiteral("isobmff"));
    QVERIFY(ruleEntry.has_value());

    SessionUserState state;
    state.bookmarks = {SessionBookmark{QStringLiteral("100GB Sparse Marker"), 64}};
    state.view.rawPageIndex = 0;
    state.view.rawDisplayMode = RawDisplayMode::Hex;

    QString errorMessage;
    auto doc = SessionDocument::create(sparsePath, sparsePath, *fpResult.fingerprint, *ruleEntry, state, &errorMessage);
    QVERIFY2(doc.has_value(), qPrintable(errorMessage));

    const QString sessionPath = tempDir.filePath(QStringLiteral("sparse_100gb.svsession"));
    QVERIFY2(doc->save(sessionPath, &errorMessage), qPrintable(errorMessage));
    QVERIFY(QFile::exists(sessionPath));

    // Load session document and verify full restoration
    const auto loaded = SessionDocument::load(sessionPath);
    QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
    QCOMPARE(loaded.document->sourceFingerprint().sizeBytes(), hundredGb);
    QCOMPARE(loaded.document->sourceFingerprint().mode(), SourceFingerprintMode::SampledSha256);
    QCOMPARE(loaded.document->userState().bookmarks.size(), std::size_t{1});
    QCOMPARE(loaded.document->userState().bookmarks[0].label, QStringLiteral("100GB Sparse Marker"));

    // Tamper with sparse tail and verify fingerprint mismatch detection
    {
        QFile sparseFile(sparsePath);
        QVERIFY(sparseFile.open(QIODevice::ReadWrite));
        makeFileSparse(sparseFile);
        sparseFile.seek(static_cast<qint64>(hundredGb - 2));
        sparseFile.write("ZZ");
        sparseFile.close();
    }
    auto tamperedSource = FileSource::open(sparsePath);
    QVERIFY(tamperedSource != nullptr);
    const auto tamperedFp = tamperedSource->fingerprint();
    QVERIFY(tamperedFp.succeeded());
    QVERIFY(*tamperedFp.fingerprint != *fpResult.fingerprint);
}

void SessionPersistenceRegressionTest::testFormatOverrideFullLifecyclePersistenceAndReplay() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString fixturePath = tempDir.filePath(QStringLiteral("ambiguous.mp4"));
    {
        QFile file(fixturePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(makeAmbiguousMp4Bytes());
        file.close();
    }
    const QString sessionPath = tempDir.filePath(QStringLiteral("override_lifecycle.svsession"));

    std::optional<RuleEntryPointIdentity> overriddenRule;
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        QVERIFY(!window.isWindowModified());

        // Obtain official H.264 rule to override MP4
        const auto h264Pkg = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY(h264Pkg.succeeded() && h264Pkg.package.has_value());
        overriddenRule = RuleEntryPointIdentity::create(
            h264Pkg.package->identity(), QStringLiteral("annex-b"));
        QVERIFY(overriddenRule.has_value());

        window.setFormatOverrideDialogHandlerForTesting(
            [&](QWidget*,
                const RulePackageCatalog&,
                const FormatSelection&,
                const RuleEntryPointIdentity&)
                -> std::optional<RuleEntryPointIdentity> {
                return overriddenRule;
            });
        window.overrideFormat();

        // Modified dirty state must be set upon override
        QVERIFY(window.isWindowModified());

        // Add user bookmark
        window.addBookmark({.label = QStringLiteral("Overridden H264 Sync"), .sourceBitOffset = 32});

        // Save session
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return sessionPath;
        });
        QVERIFY(window.saveSessionAs());
        QVERIFY(!window.isWindowModified());
        QVERIFY(QFile::exists(sessionPath));
    }

    // Now restore in a fresh MainWindow instance
    {
        MainWindow window2;
        QString errorMessage;
        QVERIFY2(window2.openSessionFile(sessionPath, &errorMessage), qPrintable(errorMessage));

        // Window must not be modified upon pristine session load
        QVERIFY(!window2.isWindowModified());

        // Bookmarks must be restored
        QCOMPARE(window2.bookmarks().size(), std::size_t{1});
        QCOMPARE(window2.bookmarks()[0].label, QStringLiteral("Overridden H264 Sync"));
        QCOMPARE(window2.bookmarks()[0].sourceBitOffset, 32ULL);

        // Active session rule must match overridden rule directly (P2-31)
        const auto activeRule = window2.activeRuleIdentity();
        QVERIFY(activeRule.has_value());
        QCOMPARE(activeRule->packageIdentity().packageId(), QStringLiteral("org.streamview.h264"));
        QCOMPARE(activeRule->entryPointId(), QStringLiteral("annex-b"));

        auto* treeView = window2.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(treeView != nullptr);
        QVERIFY(treeView->model() != nullptr);
        QVERIFY(treeView->model()->rowCount() >= 1);

        // Format ambiguity banner must remain hidden (no re-prompt, P2-30 unconditional assertion)
        auto* banner = window2.findChild<QWidget*>(QStringLiteral("formatAmbiguityBanner"));
        QVERIFY(banner != nullptr);
        QVERIFY(banner->isHidden());
    }
}

void SessionPersistenceRegressionTest::testUnsavedModificationsInteractiveGuardrailBranches() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
    const QString sessionPath = tempDir.filePath(QStringLiteral("guardrail.svsession"));

    // Branch 1: Clean session proceeds without prompt
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        QVERIFY(!window.isWindowModified());

        bool promptCalled = false;
        window.setSavePromptHandlerForTesting([&](QWidget*) {
            promptCalled = true;
            return QMessageBox::Cancel;
        });
        QVERIFY(window.maybeSave());
        QVERIFY(!promptCalled);
    }

    // Branch 2: Dirty session with Save accepted
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B1"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Save; });
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) { return sessionPath; });

        QVERIFY(window.maybeSave());
        QVERIFY(!window.isWindowModified());
        QVERIFY(QFile::exists(sessionPath));
    }

    // Branch 3: Dirty session with Discard accepted
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B2"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Discard; });
        QVERIFY(window.maybeSave());
    }

    // Branch 4: Dirty session with Cancel rejected
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B3"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Cancel; });
        QVERIFY(!window.maybeSave());
        QVERIFY(window.isWindowModified());
    }

    // Branch 5: Dirty session with SaveAs file dialog cancelled
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B4"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Save; });
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) { return QString{}; });

        QVERIFY(!window.maybeSave());
        QVERIFY(window.isWindowModified());
    }

    // Branch 6: Dirty session with I/O error displays message dialog and safely aborts
    {
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B5"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        bool ioErrorDialogShown = false;
        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Save; });
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return QStringLiteral("/non_existent_directory_cannot_write_here/test.svsession");
        });
        window.setMessageDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            ioErrorDialogShown = true;
        });

        QVERIFY(!window.maybeSave());
        QVERIFY(ioErrorDialogShown);
        QVERIFY(window.isWindowModified());
    }
}

QTEST_MAIN(SessionPersistenceRegressionTest)
#include "session_persistence_regression_test.moc"
