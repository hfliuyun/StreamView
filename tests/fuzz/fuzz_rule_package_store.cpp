#include "standalone_fuzz_driver.h"

#include <streamview/rules/rule_package.h>
#include <streamview/rules/rule_package_store.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cstddef>
#include <cstdint>
#include <vector>

using streamview::rules::RulePackage;
using streamview::rules::RulePackageFile;
using streamview::rules::RulePackageImportResult;
using streamview::rules::RulePackageLoadResult;
using streamview::rules::RulePackageStore;
using streamview::rules::RulePackageWriteResult;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    const QByteArray inputBytes(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));

    // Test 1: Archive ingestion via RulePackageStore::importArchive.
    // Ingests untrusted binary stream as a .svrule ZIP archive.
    {
        QTemporaryFile tempArchive;
        if (tempArchive.open()) {
            tempArchive.write(inputBytes);
            tempArchive.flush();
            const QString archivePath = tempArchive.fileName();

            const RulePackageImportResult importResult = RulePackageStore::importArchive(archivePath);
            if (importResult.succeeded() && importResult.package.has_value()) {
                const RulePackage& pkg = *importResult.package;
                (void)pkg.identity().toString();
                (void)pkg.manifest().packageId;
                (void)pkg.manifest().packageVersion.text();
                (void)pkg.files().size();

                // Re-serialize imported package to verify write consistency.
                QTemporaryFile roundtripFile;
                if (roundtripFile.open()) {
                    const RulePackageWriteResult writeResult =
                        RulePackageStore::writeArchive(pkg, roundtripFile.fileName());
                    (void)writeResult.succeeded();
                }
            }
        }
    }

    // Test 2: Ingestion of synthetic manifest and rule files via RulePackage::fromFiles.
    // Verifies path normalization, directory traversal rejection, and TOML validation.
    if (size >= 4) {
        const qsizetype split = static_cast<qsizetype>(size / 2);
        const QByteArray manifestPart = inputBytes.left(split);
        const QByteArray formatPart = inputBytes.mid(split);

        std::vector<RulePackageFile> files;
        files.push_back({QStringLiteral("rule.toml"), manifestPart});
        files.push_back({QStringLiteral("src/format.svfmt"), formatPart});

        const RulePackageLoadResult loadResult = RulePackage::fromFiles(std::move(files));
        if (loadResult.succeeded() && loadResult.package.has_value()) {
            (void)loadResult.package->identity().packageId();
        }
    }

    return 0;
}

#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)
int main(int argc, char** argv) {
#if defined(STREAMVIEW_FUZZ_CORPUS_DIR)
    return streamview::fuzz::runStandalone(argc, argv, STREAMVIEW_FUZZ_CORPUS_DIR);
#else
    return streamview::fuzz::runStandalone(argc, argv, nullptr);
#endif
}
#endif
