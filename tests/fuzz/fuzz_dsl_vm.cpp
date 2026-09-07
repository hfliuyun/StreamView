#include "standalone_fuzz_driver.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/dsl_vm.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

namespace {

class FuzzMemorySource final : public streamview::core::RandomAccessSource {
public:
    explicit FuzzMemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }

    [[nodiscard]] QString identity() const override {
        return QStringLiteral("fuzz_memory");
    }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {streamview::core::SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {count == destination.size() ? streamview::core::SourceReadStatus::Complete
                                            : streamview::core::SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

const char* kFuzzVmSchema = R"(
enum FuzzMode {
    Alpha = 0;
    Beta = 1;
    Gamma = 2;
}

struct FuzzHeader {
    bits<8> marker;
    bits<8> mode @enum(FuzzMode);
    ue ue_field;
    se se_field;
    if (marker == 0) {
        bits<16> zero_ext;
    } else {
        bits<8> nonzero_ext;
    }
    switch (marker) {
        case 0: {
            bits<8> arm0;
        }
        case 1: {
            bits<16> arm1;
        }
        default: {
            bits<8> arm_def;
        }
    }
    repeat(marker, 16) {
        bits<8> item;
    }
}

entry FuzzHeader;
)";

const streamview::rules::DslTypedProgram& getFuzzProgram() {
    static const streamview::rules::DslTypedProgram program = [] {
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(kFuzzVmSchema));
        auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        if (!compiled.succeeded() || !compiled.program.has_value()) {
            std::cerr << "[FUZZ ERROR] Failed to compile builtin fuzz VM schema!" << std::endl;
            std::abort();
        }
        return std::move(*compiled.program);
    }();
    return program;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr) {
        return 0;
    }

    std::vector<std::byte> buffer(size);
    if (size > 0) {
        std::memcpy(buffer.data(), data, size);
    }

    FuzzMemorySource source(std::move(buffer));
    const quint64 totalBits = source.sizeBytes() * 8U;
    const auto span = streamview::core::SourceSpan::create(
        streamview::core::SourceBitAddress(0), totalBits);
    if (!span.has_value()) {
        return 0;
    }

    const auto mapping = streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), {*span});
    if (!mapping.has_value()) {
        return 0;
    }

    const auto& program = getFuzzProgram();
    streamview::core::BitReader reader(source, *span);
    auto tree = streamview::core::AnalysisTree::create(QStringLiteral("fuzz-root"));
    if (!tree.has_value()) {
        return 0;
    }

    streamview::rules::DslExecutionOptions options;
    // Bound execution budget for fuzzing throughput and responsiveness
    options.limits.maximumInstructions = 10'000;
    options.limits.maximumMaterializedNodes = 1'000;

    const auto result = streamview::rules::DslExecutor::decodeStruct(
        program,
        QStringLiteral("FuzzHeader"),
        reader,
        *mapping,
        0,
        *tree,
        tree->rootId(),
        options);
    static_cast<void>(result);

    return 0;
}

#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)
int main(int argc, char** argv) {
    return streamview::fuzz::runStandalone(argc, argv, STREAMVIEW_FUZZ_CORPUS_DIR);
}
#endif
