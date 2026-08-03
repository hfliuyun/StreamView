#include <streamview/rules/rule_execution_session.h>

#include <streamview/core/bit_reader.h>

#include <algorithm>
#include <functional>
#include <utility>

namespace streamview::rules {
namespace {

[[nodiscard]] RuleExecutionStatus ruleStatus(DslExecutionStatus status) noexcept {
    switch (status) {
    case DslExecutionStatus::Materialized:
        return RuleExecutionStatus::Materialized;
    case DslExecutionStatus::TruncatedSource:
        return RuleExecutionStatus::TruncatedSource;
    case DslExecutionStatus::InvalidSyntax:
        return RuleExecutionStatus::InvalidSyntax;
    case DslExecutionStatus::SourceError:
        return RuleExecutionStatus::SourceError;
    case DslExecutionStatus::Cancelled:
        return RuleExecutionStatus::Cancelled;
    case DslExecutionStatus::ResourceLimit:
        return RuleExecutionStatus::ResourceLimit;
    case DslExecutionStatus::InvalidDefinition:
        return RuleExecutionStatus::InvalidDefinition;
    }
    return RuleExecutionStatus::InvalidDefinition;
}

} // namespace

const RuleExecutionSession::ContextPayload*
RuleExecutionSession::contextPayload(core::ContextDefinitionId id) const noexcept {
    if (id.value() == 0 || id.value() > contextPayloads_.size()) {
        return nullptr;
    }
    const ContextPayload& payload =
        contextPayloads_.at(static_cast<std::size_t>(id.value() - 1));
    return payload.definitionId == id ? &payload : nullptr;
}

RuleExecutionResult RuleExecutionSession::run(const RuleExecutionRequest& request) {
    RuleExecutionResult result;
    if (request.source == nullptr || request.mapping == nullptr ||
        request.tree == nullptr ||
        request.structureIndex >= program_.structs.size()) {
        result.errorMessage = QStringLiteral("Rule execution request is invalid");
        return result;
    }
    if ((source_ != nullptr && source_ != request.source) ||
        (treeIdentity_ != 0 &&
         treeIdentity_ != request.tree->instanceIdentity())) {
        result.errorMessage =
            QStringLiteral("Rule execution session belongs to a different analysis");
        return result;
    }
    if (request.logicalStart > request.mapping->logicalBitLength()) {
        result.errorMessage = QStringLiteral("Rule execution logical start is out of range");
        return result;
    }
    const quint64 logicalLength =
        request.mapping->logicalBitLength() - request.logicalStart;

    const DslTypedStruct& structure =
        program_.structs.at(request.structureIndex);
    const bool usesContext =
        structure.contextDefinition || !structure.contextImports.empty();
    if (usesContext &&
        (!request.enclosingSourceSpan || request.enclosingSourceSpan->bitLength() == 0)) {
        result.errorMessage =
            QStringLiteral("Context execution requires a non-empty enclosing source span");
        return result;
    }
    if (usesContext) {
        const auto executionRange = core::LogicalRange::create(
            core::LogicalBitAddress(request.mapping->viewId(), request.logicalStart),
            logicalLength);
        const auto executionLocation =
            executionRange ? request.mapping->locate(*executionRange) : std::nullopt;
        if (!executionLocation ||
            std::any_of(executionLocation->sourceSpans().begin(),
                        executionLocation->sourceSpans().end(),
                        [&enclosing = *request.enclosingSourceSpan](
                            const core::SourceSpan& span) {
                            return span.start() < enclosing.start() ||
                                   enclosing.endExclusive() < span.endExclusive();
                        })) {
            result.errorMessage = QStringLiteral(
                "Context execution mapping is outside its enclosing source span");
            return result;
        }
    }

    auto reader = core::BitReader::fromMappingSlice(
        *request.source, *request.mapping, request.logicalStart, logicalLength);
    if (!reader) {
        result.errorMessage = QStringLiteral("Rule execution logical view is invalid");
        return result;
    }
    source_ = request.source;
    treeIdentity_ = request.tree->instanceIdentity();
    result.execution = DslVirtualMachine::execute(program_,
                                                  request.structureIndex,
                                                  *reader,
                                                  *request.mapping,
                                                  request.logicalStart,
                                                  *request.tree,
                                                  request.parentId,
                                                  request.options);
    result.status = ruleStatus(result.execution.status);
    result.errorMessage = result.execution.errorMessage;
    if (!result.execution.materialized()) {
        return result;
    }
    if (request.requireExactConsumption &&
        result.execution.bitsConsumed != logicalLength) {
        result.status = RuleExecutionStatus::InvalidSyntax;
        result.errorMessage =
            result.execution.bitsConsumed < logicalLength
                ? QStringLiteral("Rule structure retains %1 undeclared bits")
                      .arg(logicalLength - result.execution.bitsConsumed)
                : QStringLiteral("Rule structure consumed beyond its logical view");
        return result;
    }

    if (result.execution.contextImports.size() != structure.contextImports.size()) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Context import runtime values do not match typed IR");
        return result;
    }
    std::vector<RuleImportedContext> importedContexts;
    importedContexts.reserve(structure.contextImports.size());
    for (std::size_t importIndex = 0;
         importIndex < structure.contextImports.size();
         ++importIndex) {
        const DslTypedContextImport& import =
            structure.contextImports.at(importIndex);
        const DslExecutionContextImport& executionImport =
            result.execution.contextImports.at(importIndex);
        if (executionImport.kind != import.kind) {
            result.status = RuleExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Context import runtime kind is invalid");
            return result;
        }
        const core::ContextKey key{
            import.kind, contextScopeId_, executionImport.key.value};
        const core::ContextLookupResult lookup = contextDirectory_.resolveBefore(
            key, request.enclosingSourceSpan->start());
        if (!lookup.found()) {
            result.status = RuleExecutionStatus::DependencyUnavailable;
            result.errorMessage =
                lookup.status == core::ContextLookupStatus::DependencyUnavailable
                    ? QStringLiteral("Imported context generation is unavailable")
                    : QStringLiteral("Imported context was not defined before this structure");
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::DependencyUnavailable;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = result.errorMessage;
            diagnostic.fieldPath = structure.name + QLatin1Char('.') +
                                   structure.fields.at(import.keyFieldIndex).name;
            diagnostic.location = executionImport.key.location;
            if (result.execution.structureNode) {
                (void)request.tree->addDiagnostic(*result.execution.structureNode,
                                                  std::move(diagnostic));
            }
            return result;
        }

        RuleImportedContext imported;
        imported.key = executionImport.key;
        imported.definitionId = lookup.definition->id;
        std::vector<core::ContextDefinitionId> included;
        included.reserve(RuleImportedContext::maximumDefinitions());
        std::function<bool(core::ContextDefinitionId)> appendDefinition;
        appendDefinition = [&](core::ContextDefinitionId definitionId) {
            if (std::find(included.begin(), included.end(), definitionId) !=
                included.end()) {
                return true;
            }
            if (included.size() >= RuleImportedContext::maximumDefinitions()) {
                result.status = RuleExecutionStatus::ResourceLimit;
                result.errorMessage =
                    QStringLiteral("Imported context dependency closure exceeds 64 definitions");
                return false;
            }
            const auto definition = contextDirectory_.definition(definitionId);
            const ContextPayload* payload = contextPayload(definitionId);
            if (!definition || payload == nullptr) {
                result.status = RuleExecutionStatus::InvalidDefinition;
                result.errorMessage =
                    QStringLiteral("Imported context generation has no rules-owned payload");
                return false;
            }
            included.push_back(definitionId);
            imported.definitions.push_back({definitionId,
                                            definition->key.kind,
                                            payload->structureIndex,
                                            payload->values,
                                            definition->dependencies});
            for (const core::ContextDefinitionId dependencyId :
                 definition->dependencies) {
                if (!appendDefinition(dependencyId)) {
                    return false;
                }
            }
            return true;
        };
        if (!appendDefinition(imported.definitionId)) {
            core::ParseDiagnostic diagnostic;
            diagnostic.code = result.status == RuleExecutionStatus::ResourceLimit
                                  ? core::DiagnosticCode::ResourceLimit
                                  : core::DiagnosticCode::InvalidSyntax;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = result.errorMessage;
            diagnostic.fieldPath = structure.name + QLatin1Char('.') +
                                   structure.fields.at(import.keyFieldIndex).name;
            diagnostic.location = executionImport.key.location;
            if (result.execution.structureNode) {
                (void)request.tree->addDiagnostic(*result.execution.structureNode,
                                                  std::move(diagnostic));
            }
            return result;
        }
        importedContexts.push_back(std::move(imported));
    }

    if (!structure.contextDefinition) {
        result.importedContexts = std::move(importedContexts);
        result.status = RuleExecutionStatus::Materialized;
        return result;
    }
    if (!result.execution.structureNode || !result.execution.contextValues) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Materialized context definition has no runtime values");
        return result;
    }

    const DslTypedContextDefinition& definition = *structure.contextDefinition;
    const DslExecutionContextValues& values = *result.execution.contextValues;
    if (values.dependencies.size() != definition.dependencies.size() ||
        values.exports.size() != definition.exportFieldIndices.size()) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Context runtime values do not match typed IR");
        return result;
    }

    std::vector<core::ContextDefinitionId> dependencies;
    dependencies.reserve(definition.dependencies.size());
    for (std::size_t index = 0; index < definition.dependencies.size(); ++index) {
        const DslTypedContextDependency& dependency =
            definition.dependencies.at(index);
        const DslExecutionContextValue& dependencyValue = values.dependencies.at(index);
        const core::ContextKey key{
            dependency.kind, contextScopeId_, dependencyValue.value};
        const core::ContextLookupResult lookup =
            contextDirectory_.resolveBefore(key, request.enclosingSourceSpan->start());
        if (!lookup.found()) {
            result.status = RuleExecutionStatus::DependencyUnavailable;
            result.errorMessage =
                lookup.status == core::ContextLookupStatus::DependencyUnavailable
                    ? QStringLiteral("Context dependency generation is unavailable")
                    : QStringLiteral("Context dependency was not defined before this structure");
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::DependencyUnavailable;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = result.errorMessage;
            diagnostic.fieldPath = structure.name + QLatin1Char('.') +
                                   structure.fields.at(dependency.keyFieldIndex).name;
            diagnostic.location = dependencyValue.location;
            (void)request.tree->addDiagnostic(*result.execution.structureNode,
                                              std::move(diagnostic));
            return result;
        }
        dependencies.push_back(lookup.definition->id);
    }

    ContextPayload payload;
    payload.structureIndex = request.structureIndex;
    payload.values.reserve(values.exports.size());
    for (const DslExecutionContextValue& value : values.exports) {
        payload.values.push_back(value.value);
    }
    contextPayloads_.reserve(contextPayloads_.size() + 1);

    core::ContextDefinitionSpec specification{
        {definition.kind, contextScopeId_, values.key.value},
        *request.enclosingSourceSpan,
        *result.execution.structureNode,
        std::move(dependencies),
    };
    const core::ContextRegistrationResult registration =
        contextDirectory_.registerDefinition(std::move(specification));
    if (!registration.succeeded()) {
        result.status = registration.status ==
                                core::ContextRegistrationStatus::DependencyUnavailable
                            ? RuleExecutionStatus::DependencyUnavailable
                            : RuleExecutionStatus::InvalidDefinition;
        result.errorMessage = registration.errorMessage.isEmpty()
                                  ? QStringLiteral("Context definition registration failed")
                                  : registration.errorMessage;
        core::ParseDiagnostic diagnostic;
        diagnostic.code = result.status == RuleExecutionStatus::DependencyUnavailable
                              ? core::DiagnosticCode::DependencyUnavailable
                              : core::DiagnosticCode::InvalidSyntax;
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = result.errorMessage;
        diagnostic.fieldPath = structure.name + QLatin1Char('.') +
                               structure.fields.at(definition.keyFieldIndex).name;
        diagnostic.location = values.key.location;
        (void)request.tree->addDiagnostic(*result.execution.structureNode,
                                          std::move(diagnostic));
        return result;
    }

    payload.definitionId = *registration.definitionId;
    contextPayloads_.push_back(std::move(payload));
    result.publishedDefinition = registration.definitionId;
    result.importedContexts = std::move(importedContexts);
    result.status = RuleExecutionStatus::Materialized;
    return result;
}

} // namespace streamview::rules
