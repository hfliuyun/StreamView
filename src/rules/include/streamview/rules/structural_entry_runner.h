#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/dsl_vm.h>

#include <memory>
#include <optional>

namespace streamview::rules {

struct StructuralExecutionOptions final {
    DslExecutionLimits limits;
    std::optional<core::CancellationToken> cancellation;
};

struct StructuralExecutionResult final {
    DslExecutionResult execution;
    std::shared_ptr<core::AnalysisTree> tree;

    [[nodiscard]] bool succeeded() const noexcept {
        return execution.materialized() && tree != nullptr;
    }
};

class StructuralEntryRunner final {
public:
    [[nodiscard]] static StructuralExecutionResult execute(
        const core::RandomAccessSource& baseSource,
        const core::SourceMapping& sourceMapping,
        const DslTypedProgram& program,
        const StructuralExecutionOptions& options = {});
};

} // namespace streamview::rules
