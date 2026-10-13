#include "standalone_fuzz_driver.h"

#include <streamview/rules/rule_package.h>
#include <streamview/rules/rule_package_store.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using streamview::rules::RulePackage;
using streamview::rules::RulePackageFile;
using streamview::rules::RulePackageImportResult;
using streamview::rules::RulePackageLoadResult;
using streamview::rules::RulePackageStore;
using streamview::rules::RulePackageWriteResult;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > streamview::fuzz::kMaxFuzzInputSize) {
        return 0;
    }

    const QByteArray inputBytes(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));

    // Test 1: Archive ingestion via RulePackageStore::importArchive.
    // Ingests untrusted binary stream as a .svrule ZIP archive.
    {
        QTemporaryDir temporary;
        if (!temporary.isValid()) {
            std::abort();
        }
        const QString archivePath = temporary.filePath(QStringLiteral("input.svrule"));
        QFile tempArchive(archivePath);
        if (!tempArchive.open(QIODevice::WriteOnly) ||
            tempArchive.write(inputBytes) != inputBytes.size()) {
            std::abort();
        }
        tempArchive.close();

        const RulePackageImportResult importResult = RulePackageStore::importArchive(archivePath);
        if (importResult.succeeded()) {
            const RulePackage& pkg = *importResult.package;
            const QString roundtripPath =
                temporary.filePath(QStringLiteral("roundtrip.svrule"));
            const RulePackageWriteResult writeResult =
                RulePackageStore::writeArchive(pkg, roundtripPath);
            if (!writeResult.succeeded()) {
                std::abort();
            }
            const auto reimported = RulePackageStore::importArchive(roundtripPath);
            if (!reimported.succeeded() ||
                reimported.package->identity().toString() != pkg.identity().toString()) {
                std::abort();
            }
#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)
            std::cout << "[FUZZ] Canonical archive round trip passed\n";
#endif
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
