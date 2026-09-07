#include "standalone_fuzz_driver.h"

#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <QString>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr) {
        return 0;
    }

    const QString source = QString::fromUtf8(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
    const auto parseResult = streamview::rules::DslParser::parse(source);

    // Unconditionally submit the parsed program (whether valid or containing diagnostics)
    const auto compileResult = streamview::rules::DslCompiler::compile(parseResult.program);
    static_cast<void>(compileResult);

    // Also exercise compileForTarget when an entry is declared
    if (parseResult.program.hasEntry) {
        const auto targetResult = streamview::rules::DslCompiler::compileForTarget(
            parseResult.program, parseResult.program.entry.targetName);
        static_cast<void>(targetResult);
    }

    return 0;
}

#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)
int main(int argc, char** argv) {
    return streamview::fuzz::runStandalone(argc, argv, STREAMVIEW_FUZZ_CORPUS_DIR);
}
#endif
