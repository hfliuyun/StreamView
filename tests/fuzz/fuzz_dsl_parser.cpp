#include "standalone_fuzz_driver.h"

#include <streamview/rules/dsl.h>
#include <QString>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr) {
        return 0;
    }

    const QString source = QString::fromUtf8(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));

    // Exercise lexer directly
    const auto lexResult = streamview::rules::DslLexer::lex(source);
    static_cast<void>(lexResult);

    // Exercise parser directly
    const auto parseResult = streamview::rules::DslParser::parse(source);
    static_cast<void>(parseResult);

    return 0;
}

#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)
int main(int argc, char** argv) {
    return streamview::fuzz::runStandalone(argc, argv, STREAMVIEW_FUZZ_CORPUS_DIR);
}
#endif
