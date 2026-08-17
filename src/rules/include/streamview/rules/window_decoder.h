#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/dsl_vm.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace streamview::rules {

struct RunnerExecutionBudget final {
    quint64 remainingNodes = 100'000;
    quint64 remainingInstructions = 1'000'000;
    quint32 currentNestingDepth = 0;
    std::optional<core::CancellationToken> cancellation;
};

struct WindowDecodeRequest final {
    quint64 pageIndex = 0;
    quint64 pageSize = 256;
};

struct WindowDecodeResult final {
    DslExecutionStatus status = DslExecutionStatus::Materialized;
    std::vector<core::AnalysisNodeId> entryNodes;
    quint64 decodedEntryCount = 0;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

class WindowDecoder final {
public:
    explicit WindowDecoder(
        const DslTypedProgram& program,
        const core::RandomAccessSource& source,
        core::SourceMapping sourceMapping,
        std::shared_ptr<core::AnalysisTree> tree,
        core::AnalysisNodeId windowNodeId,
        std::shared_ptr<RunnerExecutionBudget> budget,
        std::optional<core::CancellationToken> cancellation = std::nullopt);

    [[nodiscard]] WindowDecodeResult decodeWindow(const WindowDecodeRequest& request);

    [[nodiscard]] const DslTypedProgram& program() const noexcept { return *program_; }
    [[nodiscard]] const core::RandomAccessSource& source() const noexcept { return *source_; }
    [[nodiscard]] const core::SourceMapping& sourceMapping() const noexcept { return sourceMapping_; }
    [[nodiscard]] core::AnalysisNodeId windowNodeId() const noexcept { return windowNodeId_; }

private:
    const DslTypedProgram* program_ = nullptr;
    const core::RandomAccessSource* source_ = nullptr;
    core::SourceMapping sourceMapping_;
    std::shared_ptr<core::AnalysisTree> tree_;
    core::AnalysisNodeId windowNodeId_{0};
    std::shared_ptr<RunnerExecutionBudget> budget_;
    std::optional<core::CancellationToken> cancellation_;
};

} // namespace streamview::rules
