#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/context_directory.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/dsl_vm.h>

#include <QString>
#include <QtGlobal>

#include <optional>
#include <utility>
#include <vector>

namespace streamview::rules {

enum class RuleExecutionStatus : quint8 {
    Materialized,
    TruncatedSource,
    InvalidSyntax,
    DependencyUnavailable,
    SourceError,
    Cancelled,
    ResourceLimit,
    InvalidDefinition,
};

struct RuleExecutionRequest final {
    const core::RandomAccessSource* source = nullptr;
    quint32 structureIndex = 0;
    const core::SourceMapping* mapping = nullptr;
    quint64 logicalStart = 0;
    core::AnalysisTree* tree = nullptr;
    core::AnalysisNodeId parentId;
    std::optional<core::SourceSpan> enclosingSourceSpan;
    bool requireExactConsumption = true;
    DslExecutionOptions options;
};

struct RuleExecutionResult final {
    RuleExecutionStatus status = RuleExecutionStatus::InvalidDefinition;
    DslExecutionResult execution;
    std::optional<core::ContextDefinitionId> publishedDefinition;
    QString errorMessage;

    [[nodiscard]] bool materialized() const noexcept {
        return status == RuleExecutionStatus::Materialized;
    }
};

class RuleExecutionSession final {
public:
    explicit RuleExecutionSession(DslTypedProgram program,
                                  quint64 contextScopeId = 0) noexcept
        : program_(std::move(program)), contextScopeId_(contextScopeId) {}
    RuleExecutionSession(const RuleExecutionSession&) = delete;
    RuleExecutionSession(RuleExecutionSession&&) noexcept = default;
    RuleExecutionSession& operator=(const RuleExecutionSession&) = delete;
    RuleExecutionSession& operator=(RuleExecutionSession&&) noexcept = default;

    [[nodiscard]] RuleExecutionResult run(const RuleExecutionRequest& request);
    [[nodiscard]] const DslTypedProgram& program() const noexcept { return program_; }
    [[nodiscard]] const core::ContextDirectory& contextDirectory() const noexcept {
        return contextDirectory_;
    }

private:
    struct ContextPayload final {
        core::ContextDefinitionId definitionId;
        quint32 structureIndex = 0;
        std::vector<quint64> values;
    };

    DslTypedProgram program_;
    quint64 contextScopeId_ = 0;
    const core::RandomAccessSource* source_ = nullptr;
    quint64 treeIdentity_ = 0;
    core::ContextDirectory contextDirectory_;
    std::vector<ContextPayload> contextPayloads_;
};

} // namespace streamview::rules
