#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <iostream>

// Canonical LLVM libFuzzer entry point
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)

namespace streamview::fuzz {

inline bool runFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "[FUZZ ERROR] Cannot open file: " << qPrintable(filePath) << std::endl;
        return false;
    }
    const QByteArray data = file.readAll();
    const int result = LLVMFuzzerTestOneInput(
        reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<size_t>(data.size()));
    if (result != 0) {
        std::cerr << "[FUZZ ERROR] LLVMFuzzerTestOneInput returned " << result
                  << " for " << qPrintable(filePath) << std::endl;
        return false;
    }
    return true;
}

inline int runDirectory(const QString& dirPath) {
    QDir dir(dirPath);
    if (!dir.exists()) {
        std::cerr << "[FUZZ WARNING] Corpus directory not found: " << qPrintable(dirPath) << std::endl;
        return 0;
    }
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    int executed = 0;
    int failed = 0;
    for (const QFileInfo& info : entries) {
        ++executed;
        if (!runFile(info.absoluteFilePath())) {
            ++failed;
        }
    }
    std::cout << "[FUZZ] Executed " << executed << " corpus files from "
              << qPrintable(dirPath) << " (" << failed << " failed)" << std::endl;
    return (failed == 0 && executed > 0) ? 0 : (failed > 0 ? 1 : 0);
}

inline int runStandalone(int argc, char** argv, const char* defaultCorpusDir = nullptr) {
    if (argc > 1) {
        int failed = 0;
        int executed = 0;
        for (int i = 1; i < argc; ++i) {
            const QString path = QString::fromLocal8Bit(argv[i]);
            const QFileInfo info(path);
            if (info.isDir()) {
                if (runDirectory(path) != 0) {
                    ++failed;
                } else {
                    ++executed;
                }
            } else if (info.isFile()) {
                ++executed;
                if (!runFile(path)) {
                    ++failed;
                }
            } else {
                std::cerr << "[FUZZ ERROR] Invalid path: " << qPrintable(path) << std::endl;
                ++failed;
            }
        }
        return (failed == 0 && executed > 0) ? 0 : (failed > 0 ? 1 : 0);
    }

    if (defaultCorpusDir != nullptr && defaultCorpusDir[0] != '\0') {
        const QString corpusPath = QString::fromUtf8(defaultCorpusDir);
        if (QDir(corpusPath).exists()) {
            return runDirectory(corpusPath);
        }
    }

    std::cout << "[FUZZ] No input files specified and default corpus directory missing." << std::endl;
    return 0;
}

} // namespace streamview::fuzz

#endif // !STREAMVIEW_ENABLE_LIBFUZZER
