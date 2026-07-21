#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

class SvtoolTest final : public QObject {
    Q_OBJECT

private slots:
    void checksAValidRule() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rulePath = directory.filePath(QStringLiteral("valid.svfmt"));
        QFile ruleFile(rulePath);
        QVERIFY(ruleFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray ruleSource =
            "struct Header { bits<8> value; }\n"
            "entry Header;\n";
        QCOMPARE(ruleFile.write(ruleSource), static_cast<qint64>(ruleSource.size()));
        ruleFile.close();

        QProcess process;
        process.start(QStringLiteral(SVTOOL_PATH),
                      {QStringLiteral("rule"), QStringLiteral("check"), rulePath});
        QVERIFY2(process.waitForFinished(), qPrintable(process.errorString()));

        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY(standardOutput.contains(QStringLiteral("Rule OK:")));
        QVERIFY(standardOutput.contains(rulePath));
        QVERIFY(process.readAllStandardError().isEmpty());
    }

    void reportsRuleDiagnosticsWithSourcePositions() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rulePath = directory.filePath(QStringLiteral("invalid.svfmt"));
        QFile ruleFile(rulePath);
        QVERIFY(ruleFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray ruleSource =
            "struct Header { bits<0> value; }\n"
            "entry Header;\n";
        QCOMPARE(ruleFile.write(ruleSource), static_cast<qint64>(ruleSource.size()));
        ruleFile.close();

        QProcess process;
        process.start(QStringLiteral(SVTOOL_PATH),
                      {QStringLiteral("rule"), QStringLiteral("check"), rulePath});
        QVERIFY2(process.waitForFinished(), qPrintable(process.errorString()));

        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardOutput().isEmpty());
        const QString standardError = QString::fromUtf8(process.readAllStandardError());
        QVERIFY(standardError.contains(rulePath + QStringLiteral(":")));
        QVERIFY(standardError.contains(QStringLiteral(":1:")));
        QVERIFY(standardError.contains(QStringLiteral(": error: ")));
        QVERIFY(standardError.contains(QStringLiteral("1..64")));
    }

    void analyzesAnnexBHeadersWithTheSharedRunner() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("sample.h264"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        const QByteArray sourceBytes("\x00\x00\x01\x65", 4);
        QCOMPARE(sourceFile.write(sourceBytes), static_cast<qint64>(sourceBytes.size()));
        sourceFile.close();

        QProcess process;
        process.start(QStringLiteral(SVTOOL_PATH),
                      {QStringLiteral("analyze"), sourcePath});
        QVERIFY2(process.waitForFinished(), qPrintable(process.errorString()));

        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY(standardOutput.contains(QStringLiteral("nal_unit[0]")));
        QVERIFY(standardOutput.contains(QStringLiteral("forbidden_zero_bit = 0")));
        QVERIFY(standardOutput.contains(QStringLiteral("nal_ref_idc = 3")));
        QVERIFY(standardOutput.contains(QStringLiteral("nal_unit_type = 5")));
        QVERIFY(standardOutput.contains(QStringLiteral("source bits [24, 25)")));
        QVERIFY(process.readAllStandardError().isEmpty());
    }

    void printsPartialResultsAndDiagnosticsForTruncatedInput() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("truncated.h264"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        const QByteArray sourceBytes("\x00\x00\x01", 3);
        QCOMPARE(sourceFile.write(sourceBytes), static_cast<qint64>(sourceBytes.size()));
        sourceFile.close();

        QProcess process;
        process.start(QStringLiteral(SVTOOL_PATH),
                      {QStringLiteral("analyze"), sourcePath});
        QVERIFY2(process.waitForFinished(), qPrintable(process.errorString()));

        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 1);
        const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY(standardOutput.contains(QStringLiteral("nal_unit[0]")));
        QVERIFY(standardOutput.contains(QStringLiteral("start_code")));
        QVERIFY(standardOutput.contains(QStringLiteral("NalUnitHeader")));
        const QString standardError = QString::fromUtf8(process.readAllStandardError());
        QVERIFY(standardError.contains(QStringLiteral("truncated-source")));
        QVERIFY(standardError.contains(QStringLiteral("NalUnitHeader.forbidden_zero_bit")));
    }

    void rejectsInputWithoutAnAnnexBStartCode() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("not-h264.bin"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        const QByteArray sourceBytes("\x12\x34\x56", 3);
        QCOMPARE(sourceFile.write(sourceBytes), static_cast<qint64>(sourceBytes.size()));
        sourceFile.close();

        QProcess process;
        process.start(QStringLiteral(SVTOOL_PATH),
                      {QStringLiteral("analyze"), sourcePath});
        QVERIFY2(process.waitForFinished(), qPrintable(process.errorString()));

        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 1);
        const QString standardError = QString::fromUtf8(process.readAllStandardError());
        QVERIFY(standardError.contains(QStringLiteral("invalid-syntax")));
        QVERIFY(standardError.contains(QStringLiteral("No H.264 Annex B start code")));
    }
};

QTEST_GUILESS_MAIN(SvtoolTest)

#include "svtool_test.moc"
