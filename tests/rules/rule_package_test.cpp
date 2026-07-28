#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_package.h>

#include <QTest>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

using streamview::rules::EngineCompatibilityRange;
using streamview::rules::LanguageContract;
using streamview::rules::RuleCatalogLookupStatus;
using streamview::rules::RuleCatalogRegistrationStatus;
using streamview::rules::RulePackage;
using streamview::rules::RulePackageCatalog;
using streamview::rules::RulePackageFile;
using streamview::rules::RulePackageIdentity;
using streamview::rules::RulePackageLoadResult;
using streamview::rules::RulePackageLoadStatus;
using streamview::rules::SemanticVersion;
using streamview::rules::compareSemanticVersions;

namespace {

[[nodiscard]] QByteArray manifest(const QString& version = QStringLiteral("0.1.0"),
                                  const QString& language = QStringLiteral("0.1"),
                                  const QString& engine = QStringLiteral(">=0.1.0 <0.2.0"),
                                  const QByteArray& extra = {}) {
    QByteArray result = QByteArrayLiteral(
        "manifest-version = 1\n"
        "\n"
        "[package]\n"
        "id = \"org.example.packet\"\n"
        "version = \"");
    result += version.toUtf8();
    result += QByteArrayLiteral(
        "\"\n"
        "authors = [\"Example Author\"]\n"
        "license = \"MIT\"\n"
        "dependencies = []\n"
        "\n"
        "[compatibility]\n"
        "language = \"");
    result += language.toUtf8();
    result += QByteArrayLiteral("\"\nengine = \"");
    result += engine.toUtf8();
    result += QByteArrayLiteral(
        "\"\n"
        "\n"
        "[[entrypoints]]\n"
        "id = \"packet\"\n"
        "format = \"application.example.packet\"\n"
        "source = \"src/packet.svfmt\"\n"
        "profiles = [\"baseline\"]\n"
        "depth = \"header\"\n");
    result += extra;
    return result;
}

[[nodiscard]] std::vector<RulePackageFile>
packageFiles(const QByteArray& manifestBytes = manifest(),
             const QByteArray& source = QByteArrayLiteral(
                 "struct Packet { bits<8> value; }\nentry Packet;\n")) {
    return {
        {QStringLiteral("rule.toml"), manifestBytes},
        {QStringLiteral("src/packet.svfmt"), source},
    };
}

[[nodiscard]] RulePackageLoadResult loadPackage(
    const QString& version = QStringLiteral("0.1.0"),
    const QString& language = QStringLiteral("0.1"),
    const QString& engine = QStringLiteral(">=0.1.0 <0.2.0"),
    const QByteArray& source = QByteArrayLiteral(
        "struct Packet { bits<8> value; }\nentry Packet;\n")) {
    return RulePackage::fromFiles(packageFiles(manifest(version, language, engine), source));
}

} // namespace

class RulePackageTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesAndOrdersSemanticVersions() {
        const auto alpha = SemanticVersion::parse(u"1.0.0-alpha");
        const auto alphaOne = SemanticVersion::parse(u"1.0.0-alpha.1");
        const auto beta = SemanticVersion::parse(u"1.0.0-beta");
        const auto release = SemanticVersion::parse(u"1.0.0");
        QVERIFY(alpha.has_value());
        QVERIFY(alphaOne.has_value());
        QVERIFY(beta.has_value());
        QVERIFY(release.has_value());
        QCOMPARE(alpha->text(), QStringLiteral("1.0.0-alpha"));
        QVERIFY(compareSemanticVersions(*alpha, *alphaOne) < 0);
        QVERIFY(compareSemanticVersions(*alphaOne, *beta) < 0);
        QVERIFY(compareSemanticVersions(*beta, *release) < 0);
        QCOMPARE(compareSemanticVersions(*release, *release), 0);

        for (const QString& invalid : {QStringLiteral("1.0"),
                                       QStringLiteral("01.0.0"),
                                       QStringLiteral("1.0.0-01"),
                                       QStringLiteral("1.0.0-"),
                                       QStringLiteral("1.0.0+build")}) {
            QString error;
            QVERIFY2(!SemanticVersion::parse(invalid, &error).has_value(), qPrintable(invalid));
            QVERIFY(!error.isEmpty());
        }
    }

    void appliesLanguageAndEngineCompatibility() {
        const auto zero = LanguageContract::parse(u"0.1");
        const auto stable = LanguageContract::parse(u"1.2");
        QVERIFY(zero.has_value());
        QVERIFY(stable.has_value());
        QVERIFY(zero->accepts(u"0.1"));
        QVERIFY(!zero->accepts(u"0.2"));
        QVERIFY(stable->accepts(u"1.2"));
        QVERIFY(stable->accepts(u"1.9"));
        QVERIFY(!stable->accepts(u"1.1"));
        QVERIFY(!stable->accepts(u"2.0"));
        QVERIFY(!LanguageContract::parse(u"01.2").has_value());

        const auto range = EngineCompatibilityRange::parse(u">=0.1.0 <0.2.0");
        QVERIFY(range.has_value());
        QVERIFY(range->contains(u"0.1.0"));
        QVERIFY(range->contains(u"0.1.9"));
        QVERIFY(!range->contains(u"0.0.9"));
        QVERIFY(!range->contains(u"0.2.0"));
        QVERIFY(!range->contains(u"0.2.0-alpha"));
        QVERIFY(!EngineCompatibilityRange::parse(u">=0.2.0 <0.1.0").has_value());
        QVERIFY(!EngineCompatibilityRange::parse(u">=0.1.0-alpha <0.2.0").has_value());
    }

    void loadsAStandardTomlManifestAndHashesTheLogicalTree() {
        QByteArray toml = manifest();
        toml.replace("authors = [\"Example Author\"]",
                     "authors = [\n  \"Example Author\", # TOML comment\n]");
        toml += QByteArrayLiteral(
            "\n[[documentation]]\n"
            "language = \"en\"\n"
            "path = \"docs/en/packet.md\"\n");
        auto files = packageFiles(toml);
        files.push_back({QStringLiteral("docs/en/packet.md"), QByteArrayLiteral("# Packet\n")});

        auto loaded = RulePackage::fromFiles(files);

        QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
        QCOMPARE(loaded.package->manifest().packageId, QStringLiteral("org.example.packet"));
        QCOMPARE(loaded.package->manifest().packageVersion.text(), QStringLiteral("0.1.0"));
        QCOMPARE(loaded.package->manifest().entryPoints.size(), std::size_t{1});
        QCOMPARE(loaded.package->manifest().entryPoints.front().sourcePath,
                 QStringLiteral("src/packet.svfmt"));
        QCOMPARE(loaded.package->manifest().documentation.size(), std::size_t{1});
        QCOMPARE(loaded.package->identity().contentHash().size(), qsizetype{32});
        QCOMPARE(loaded.package->identity().contentHashText(),
                 QStringLiteral(
                     "sha256:ce9e80e12d7dcd6ab40237f7acf047dcd27f7f1d748fbd5273c595b8bf567f98"));
        const QByteArray* source = loaded.package->fileContents(u"src/packet.svfmt");
        QVERIFY(source != nullptr);
        QCOMPARE(*source, files.at(1).contents);
        QVERIFY(loaded.package->fileContents(u"src/missing.svfmt") == nullptr);

        std::reverse(files.begin(), files.end());
        const auto reordered = RulePackage::fromFiles(std::move(files));
        QVERIFY(reordered.succeeded());
        QCOMPARE(reordered.package->identity(), loaded.package->identity());
    }

    void changesTheHashForAnyLogicalContentChange() {
        auto original = loadPackage();
        auto changedSource = loadPackage(QStringLiteral("0.1.0"),
                                         QStringLiteral("0.1"),
                                         QStringLiteral(">=0.1.0 <0.2.0"),
                                         QByteArrayLiteral(
                                             "struct Packet { bits<9> value; }\nentry Packet;\n"));
        auto changedManifest = packageFiles(manifest() + QByteArrayLiteral("\n# comment\n"));
        auto changedWhitespace = RulePackage::fromFiles(std::move(changedManifest));

        QVERIFY(original.succeeded());
        QVERIFY(changedSource.succeeded());
        QVERIFY(changedWhitespace.succeeded());
        QVERIFY(original.package->identity().contentHash() !=
                changedSource.package->identity().contentHash());
        QVERIFY(original.package->identity().contentHash() !=
                changedWhitespace.package->identity().contentHash());
    }

    void rejectsNoncanonicalOrDangerousTreePaths() {
        const std::vector<QString> invalidPaths{
            QStringLiteral("../packet.svfmt"),
            QStringLiteral("/src/packet.svfmt"),
            QStringLiteral("src\\packet.svfmt"),
            QString::fromUtf8("src/caf\xC3\xA9.svfmt"),
            QStringLiteral("src/%2e%2e.svfmt"),
            QStringLiteral("other/packet.svfmt"),
            QStringLiteral("tests/payload.exe"),
            QStringLiteral("tests/libpayload.so.1"),
        };
        for (const QString& invalidPath : invalidPaths) {
            auto files = packageFiles();
            files.push_back({invalidPath, QByteArrayLiteral("data")});
            const auto loaded = RulePackage::fromFiles(std::move(files));
            QCOMPARE(loaded.status, RulePackageLoadStatus::InvalidTree);
            QVERIFY2(!loaded.errorMessage.isEmpty(), qPrintable(invalidPath));
        }

        auto collision = packageFiles();
        collision.push_back(
            {QStringLiteral("src/PACKET.SVFMT"), QByteArrayLiteral("different")});
        const auto collided = RulePackage::fromFiles(std::move(collision));
        QCOMPARE(collided.status, RulePackageLoadStatus::InvalidTree);
        QVERIFY(collided.errorMessage.contains(QStringLiteral("collide")));
    }

    void enforcesTreeAndReferenceBounds() {
        auto oversizedFile = packageFiles();
        oversizedFile.push_back(
            {QStringLiteral("tests/large.bin"), QByteArray(8 * 1024 * 1024 + 1, 'x')});
        QCOMPARE(RulePackage::fromFiles(std::move(oversizedFile)).status,
                 RulePackageLoadStatus::InvalidTree);

        auto missingManifest = packageFiles();
        missingManifest.erase(missingManifest.begin());
        QCOMPARE(RulePackage::fromFiles(std::move(missingManifest)).status,
                 RulePackageLoadStatus::InvalidTree);

        auto missingSource = packageFiles();
        missingSource.pop_back();
        QCOMPARE(RulePackage::fromFiles(std::move(missingSource)).status,
                 RulePackageLoadStatus::InvalidManifest);

        QByteArray withDocumentation = manifest();
        withDocumentation += QByteArrayLiteral(
            "\n[[documentation]]\n"
            "language = \"en\"\n"
            "path = \"docs/en/missing.md\"\n");
        QCOMPARE(RulePackage::fromFiles(packageFiles(withDocumentation)).status,
                 RulePackageLoadStatus::InvalidManifest);
    }

    void rejectsMalformedOrUnsupportedManifestFields() {
        const std::vector<QByteArray> invalidManifests{
            manifest() + QByteArrayLiteral("\nunknown = true\n"),
            QByteArrayLiteral("\xEF\xBB\xBF") + manifest(),
            manifest(QStringLiteral("0.1.0+rebuilt")),
            manifest(QStringLiteral("0.1.0"), QStringLiteral("00.1")),
            manifest(QStringLiteral("0.1.0"),
                     QStringLiteral("0.1"),
                     QStringLiteral(">=0.2.0 <0.1.0")),
        };
        for (const QByteArray& invalidManifest : invalidManifests) {
            const auto loaded = RulePackage::fromFiles(packageFiles(invalidManifest));
            QCOMPARE(loaded.status, RulePackageLoadStatus::InvalidManifest);
            QVERIFY(!loaded.errorMessage.isEmpty());
        }

        QByteArray dependencies = manifest();
        dependencies.replace("dependencies = []", "dependencies = [\"org.example.other\"]");
        QCOMPARE(RulePackage::fromFiles(packageFiles(dependencies)).status,
                 RulePackageLoadStatus::InvalidManifest);

        QByteArray duplicateProfiles = manifest();
        duplicateProfiles.replace("profiles = [\"baseline\"]",
                                  "profiles = [\"baseline\", \"baseline\"]");
        QCOMPARE(RulePackage::fromFiles(packageFiles(duplicateProfiles)).status,
                 RulePackageLoadStatus::InvalidManifest);
    }

    void catalogsVersionsAndRejectsRepublishedContentTransactionally() {
        auto first = loadPackage(QStringLiteral("0.1.0"));
        auto duplicate = loadPackage(QStringLiteral("0.1.0"));
        auto second = loadPackage(QStringLiteral("0.2.0"));
        auto conflict = loadPackage(QStringLiteral("0.1.0"),
                                    QStringLiteral("0.1"),
                                    QStringLiteral(">=0.1.0 <0.2.0"),
                                    QByteArrayLiteral(
                                        "struct Packet { bits<16> value; }\nentry Packet;\n"));
        QVERIFY(first.succeeded());
        QVERIFY(duplicate.succeeded());
        QVERIFY(second.succeeded());
        QVERIFY(conflict.succeeded());

        RulePackageCatalog catalog;
        QCOMPARE(catalog.registerPackage(std::move(*first.package)).status,
                 RuleCatalogRegistrationStatus::Registered);
        QCOMPARE(catalog.registerPackage(std::move(*duplicate.package)).status,
                 RuleCatalogRegistrationStatus::AlreadyRegistered);
        QCOMPARE(catalog.registerPackage(std::move(*second.package)).status,
                 RuleCatalogRegistrationStatus::Registered);
        QCOMPARE(catalog.packageCount(), qsizetype{2});

        const auto rejected = catalog.registerPackage(std::move(*conflict.package));
        QCOMPARE(rejected.status, RuleCatalogRegistrationStatus::VersionConflict);
        QVERIFY(!rejected.errorMessage.isEmpty());
        QCOMPARE(catalog.packageCount(), qsizetype{2});
    }

    void resolvesOnlyAnExactCompatibleCatalogIdentity() {
        auto loaded = loadPackage();
        QVERIFY(loaded.succeeded());
        const auto identity = loaded.package->identity();
        RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*loaded.package)).succeeded());

        const auto found = catalog.resolve(identity, u"packet", u"0.1", u"0.1.0");
        QCOMPARE(found.status, RuleCatalogLookupStatus::Found);
        QVERIFY(found.succeeded());
        QCOMPARE(found.entryPoint->sourcePath, QStringLiteral("src/packet.svfmt"));

        auto wrongHash = RulePackageIdentity::create(
            identity.packageId(), identity.packageVersion(), QByteArray(32, '\0'));
        QVERIFY(wrongHash.has_value());
        QCOMPARE(catalog.resolve(*wrongHash, u"packet", u"0.1", u"0.1.0").status,
                 RuleCatalogLookupStatus::VersionConflict);

        auto missingVersion = RulePackageIdentity::create(
            identity.packageId(), QStringLiteral("0.2.0"), identity.contentHash());
        QVERIFY(missingVersion.has_value());
        QCOMPARE(catalog.resolve(*missingVersion, u"packet", u"0.1", u"0.1.0").status,
                 RuleCatalogLookupStatus::MissingContent);
        QVERIFY(!RulePackageIdentity::create(identity.packageId(),
                                             identity.packageVersion(),
                                             QByteArray(31, '\0'))
                     .has_value());
        QCOMPARE(catalog.resolve(identity, u"missing", u"0.1", u"0.1.0").status,
                 RuleCatalogLookupStatus::UnknownEntryPoint);
        QCOMPARE(catalog.resolve(identity, u"packet", u"0.2", u"0.1.0").status,
                 RuleCatalogLookupStatus::IncompatibleLanguage);
        QCOMPARE(catalog.resolve(identity, u"packet", u"0.1", u"0.2.0").status,
                 RuleCatalogLookupStatus::IncompatibleEngine);
    }
};

QTEST_GUILESS_MAIN(RulePackageTest)

#include "rule_package_test.moc"
