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
    case DslExecutionStatus::Unsupported:
        return RuleExecutionStatus::Unsupported;
    case DslExecutionStatus::TruncatedSource:
        return RuleExecutionStatus::TruncatedSource;
    case DslExecutionStatus::InvalidSyntax:
        return RuleExecutionStatus::InvalidSyntax;
    case DslExecutionStatus::DependencyUnavailable:
        return RuleExecutionStatus::DependencyUnavailable;
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

    struct ImportMaterialization final {
        DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
        RuleImportedContext* imported = nullptr;
        QString errorMessage;

        [[nodiscard]] bool materialized() const noexcept {
            return status == DslExecutionStatus::Materialized && imported != nullptr;
        }
    };
    std::vector<std::optional<RuleImportedContext>> importCache(
        structure.contextImports.size());
    const auto materializeImport =
        [&](quint32 importIndex, std::optional<quint64> importKey) -> ImportMaterialization {
        if (importIndex >= structure.contextImports.size()) {
            return {DslExecutionStatus::InvalidDefinition,
                    nullptr,
                    QStringLiteral("Context import index is out of range")};
        }
        if (importCache.at(importIndex)) {
            RuleImportedContext& cached = *importCache.at(importIndex);
            if (importKey.has_value()) {
                return cached.key.value == *importKey
                           ? ImportMaterialization{
                                 DslExecutionStatus::Materialized, &cached, {}}
                           : ImportMaterialization{
                                 DslExecutionStatus::InvalidDefinition,
                                 nullptr,
                                 QStringLiteral(
                                     "Context import key changed during one execution")};
            }
            return ImportMaterialization{
                DslExecutionStatus::Materialized, &cached, {}};
        }

        const DslTypedContextImport& import =
            structure.contextImports.at(importIndex);
        core::ContextLookupResult lookup;
        if (importKey.has_value()) {
            const core::ContextKey key{import.kind, contextScopeId_, *importKey};
            lookup = contextDirectory_.resolveBefore(
                key, request.enclosingSourceSpan->start());
        } else {
            lookup = contextDirectory_.resolveLatestBefore(
                import.kind, contextScopeId_, request.enclosingSourceSpan->start());
        }
        if (!lookup.found()) {
            return {
                DslExecutionStatus::DependencyUnavailable,
                nullptr,
                lookup.status == core::ContextLookupStatus::DependencyUnavailable
                    ? QStringLiteral("Imported context generation is unavailable")
                    : QStringLiteral(
                          "Imported context was not defined before this structure"),
            };
        }

        RuleImportedContext imported;
        if (importKey.has_value()) {
            imported.key.value = *importKey;
        } else if (lookup.definition) {
            imported.key.value = lookup.definition->key.value;
        }
        imported.definitionId = lookup.definition->id;
        std::vector<core::ContextDefinitionId> included;
        included.reserve(RuleImportedContext::maximumDefinitions());
        DslExecutionStatus closureStatus = DslExecutionStatus::Materialized;
        QString closureError;
        std::function<bool(core::ContextDefinitionId)> appendDefinition;
        appendDefinition = [&](core::ContextDefinitionId definitionId) {
            if (std::find(included.begin(), included.end(), definitionId) !=
                included.end()) {
                return true;
            }
            if (included.size() >= RuleImportedContext::maximumDefinitions()) {
                closureStatus = DslExecutionStatus::ResourceLimit;
                closureError = QStringLiteral(
                    "Imported context dependency closure exceeds 64 definitions");
                return false;
            }
            const auto definition = contextDirectory_.definition(definitionId);
            const ContextPayload* payload = contextPayload(definitionId);
            if (!definition || payload == nullptr) {
                closureStatus = DslExecutionStatus::InvalidDefinition;
                closureError = QStringLiteral(
                    "Imported context generation has no rules-owned payload");
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
            return {closureStatus, nullptr, std::move(closureError)};
        }

        importCache.at(importIndex) = std::move(imported);
        return {DslExecutionStatus::Materialized,
                &*importCache.at(importIndex),
                {}};
    };
    const DslContextValueResolver contextValueResolver =
        [&](const DslContextValueRequest& valueRequest) {
        if (valueRequest.contextImportIndex >= structure.contextImports.size() ||
            structure.contextImports.at(valueRequest.contextImportIndex).kind !=
                valueRequest.importKind) {
            return DslContextValueResolution{
                DslExecutionStatus::InvalidDefinition,
                0,
                QStringLiteral("Imported context value request is invalid")};
        }
        const ImportMaterialization materialized = materializeImport(
            valueRequest.contextImportIndex, valueRequest.importKey);
        if (!materialized.materialized()) {
            return DslContextValueResolution{
                materialized.status, 0, materialized.errorMessage};
        }

        const RuleImportedContextDefinition* matchedDefinition = nullptr;
        for (const RuleImportedContextDefinition& definition :
             materialized.imported->definitions) {
            if (definition.kind != valueRequest.definitionKind ||
                definition.structureIndex != valueRequest.structureIndex) {
                continue;
            }
            if (matchedDefinition != nullptr) {
                return DslContextValueResolution{
                    DslExecutionStatus::InvalidDefinition,
                    0,
                    QStringLiteral(
                        "Imported context value publisher is ambiguous")};
            }
            matchedDefinition = &definition;
        }
        if (matchedDefinition == nullptr ||
            valueRequest.exportIndex >= matchedDefinition->values.size()) {
            return DslContextValueResolution{
                DslExecutionStatus::InvalidDefinition,
                0,
                QStringLiteral(
                    "Imported context value descriptor does not match its payload")};
        }
        return DslContextValueResolution{
            DslExecutionStatus::Materialized,
            matchedDefinition->values.at(valueRequest.exportIndex),
            {}};
    };
    result.execution = DslVirtualMachine::execute(program_,
                                                  request.structureIndex,
                                                  *reader,
                                                  *request.mapping,
                                                  request.logicalStart,
                                                  *request.tree,
                                                  request.parentId,
                                                  request.options,
                                                  contextValueResolver);
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
        if (import.keyFieldIndex.has_value()) {
            if (!executionImport.key.has_value() || !executionImport.key->location) {
                continue;
            }
            const ImportMaterialization materialized =
                materializeImport(static_cast<quint32>(importIndex),
                                  executionImport.key->value);
            if (!materialized.materialized()) {
                result.status = ruleStatus(materialized.status);
                result.errorMessage = materialized.errorMessage;
                core::ParseDiagnostic diagnostic;
                diagnostic.code = result.status == RuleExecutionStatus::ResourceLimit
                                      ? core::DiagnosticCode::ResourceLimit
                                  : result.status ==
                                            RuleExecutionStatus::DependencyUnavailable
                                      ? core::DiagnosticCode::DependencyUnavailable
                                      : core::DiagnosticCode::InvalidSyntax;
                diagnostic.severity = core::DiagnosticSeverity::Error;
                diagnostic.message = result.errorMessage;
                diagnostic.fieldPath = structure.name + QLatin1Char('.') +
                                       structure.fields.at(*import.keyFieldIndex).name;
                diagnostic.location = executionImport.key->location;
                if (result.execution.structureNode) {
                    (void)request.tree->addDiagnostic(*result.execution.structureNode,
                                                      std::move(diagnostic));
                }
                return result;
            }
            materialized.imported->key = *executionImport.key;
            importedContexts.push_back(std::move(*importCache.at(importIndex)));
        } else {
            if (!importCache.at(importIndex).has_value()) {
                continue;
            }
            importedContexts.push_back(std::move(*importCache.at(importIndex)));
        }
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

void RuleExecutionSession::reset(quint64 contextScopeId) noexcept {
    contextDirectory_ = core::ContextDirectory{};
    contextPayloads_.clear();
    source_ = nullptr;
    treeIdentity_ = 0;
    contextScopeId_ = contextScopeId;
}

CompoundRuleExecutionResult
RuleExecutionSession::runCompound(const CompoundRuleExecutionRequest& request) {
    CompoundRuleExecutionResult result;
    if (request.source == nullptr || request.headerMapping == nullptr ||
        request.tree == nullptr ||
        request.headerStructureIndex >= program_.structs.size()) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Compound rule execution request is invalid");
        return result;
    }
    if (request.payloadStructureIndex.has_value()) {
        if (*request.payloadStructureIndex >= program_.structs.size() ||
            request.payloadMapping == nullptr) {
            result.status = RuleExecutionStatus::InvalidDefinition;
            result.errorMessage =
                QStringLiteral("Compound rule execution payload request is invalid");
            return result;
        }
    }

    if ((source_ != nullptr && source_ != request.source) ||
        (treeIdentity_ != 0 && treeIdentity_ != request.tree->instanceIdentity())) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Rule execution session belongs to a different analysis");
        return result;
    }

    const DslTypedStruct& headerStructure =
        program_.structs.at(request.headerStructureIndex);
    const DslTypedStruct* payloadStructure =
        request.payloadStructureIndex.has_value()
            ? &program_.structs.at(*request.payloadStructureIndex)
            : nullptr;

    const bool usesContext =
        headerStructure.contextDefinition ||
        !headerStructure.contextImports.empty() ||
        (payloadStructure != nullptr &&
         (payloadStructure->contextDefinition ||
          !payloadStructure->contextImports.empty()));

    if (usesContext &&
        (!request.enclosingSourceSpan ||
         request.enclosingSourceSpan->bitLength() == 0)) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Context execution requires a non-empty enclosing source span");
        return result;
    }
    if (usesContext) {
        const auto headerRange = core::LogicalRange::create(
            core::LogicalBitAddress(request.headerMapping->viewId(), 0),
            request.headerMapping->logicalBitLength());
        const auto headerLocation =
            headerRange ? request.headerMapping->locate(*headerRange) : std::nullopt;
        if (!headerLocation ||
            std::any_of(headerLocation->sourceSpans().begin(),
                        headerLocation->sourceSpans().end(),
                        [&enclosing = *request.enclosingSourceSpan](
                            const core::SourceSpan& span) {
                            return span.start() < enclosing.start() ||
                                   enclosing.endExclusive() < span.endExclusive();
                        })) {
            result.status = RuleExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral(
                "Context execution mapping is outside its enclosing source span");
            return result;
        }

        if (request.payloadMapping != nullptr) {
            const quint64 payloadLogicalLength =
                request.payloadLogicalStart <=
                        request.payloadMapping->logicalBitLength()
                    ? (request.payloadMapping->logicalBitLength() -
                       request.payloadLogicalStart)
                    : 0;
            const auto payloadRange = core::LogicalRange::create(
                core::LogicalBitAddress(request.payloadMapping->viewId(),
                                        request.payloadLogicalStart),
                payloadLogicalLength);
            const auto payloadLocation =
                payloadRange ? request.payloadMapping->locate(*payloadRange)
                             : std::nullopt;
            if (!payloadLocation ||
                std::any_of(payloadLocation->sourceSpans().begin(),
                            payloadLocation->sourceSpans().end(),
                            [&enclosing = *request.enclosingSourceSpan](
                                const core::SourceSpan& span) {
                                return span.start() < enclosing.start() ||
                                       enclosing.endExclusive() < span.endExclusive();
                            })) {
                result.status = RuleExecutionStatus::InvalidDefinition;
                result.errorMessage = QStringLiteral(
                    "Context execution mapping is outside its enclosing source span");
                return result;
            }
        }
    }

    struct ImportMaterialization final {
        DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
        RuleImportedContext* imported = nullptr;
        QString errorMessage;

        [[nodiscard]] bool materialized() const noexcept {
            return status == DslExecutionStatus::Materialized && imported != nullptr;
        }
    };

    std::map<std::pair<quint32, quint32>, RuleImportedContext> importCache;

    const auto materializeImport =
        [&](quint32 structureIndex,
            quint32 importIndex,
            std::optional<quint64> importKey) -> ImportMaterialization {
        if (structureIndex >= program_.structs.size()) {
            return {DslExecutionStatus::InvalidDefinition,
                    nullptr,
                    QStringLiteral("Structure index is out of range")};
        }
        const DslTypedStruct& st = program_.structs.at(structureIndex);
        if (importIndex >= st.contextImports.size()) {
            return {DslExecutionStatus::InvalidDefinition,
                    nullptr,
                    QStringLiteral("Context import index is out of range")};
        }
        const auto cacheKey = std::make_pair(structureIndex, importIndex);
        auto it = importCache.find(cacheKey);
        if (it != importCache.end()) {
            RuleImportedContext& cached = it->second;
            if (importKey.has_value()) {
                return cached.key.value == *importKey
                           ? ImportMaterialization{
                                 DslExecutionStatus::Materialized, &cached, {}}
                           : ImportMaterialization{
                                 DslExecutionStatus::InvalidDefinition,
                                 nullptr,
                                 QStringLiteral(
                                     "Context import key changed during one execution")};
            }
            return ImportMaterialization{
                DslExecutionStatus::Materialized, &cached, {}};
        }

        const DslTypedContextImport& import = st.contextImports.at(importIndex);
        core::ContextLookupResult lookup;
        if (importKey.has_value()) {
            const core::ContextKey key{import.kind, contextScopeId_, *importKey};
            lookup = contextDirectory_.resolveBefore(
                key, request.enclosingSourceSpan->start());
        } else {
            lookup = contextDirectory_.resolveLatestBefore(
                import.kind, contextScopeId_, request.enclosingSourceSpan->start());
        }
        if (!lookup.found()) {
            return {
                DslExecutionStatus::DependencyUnavailable,
                nullptr,
                lookup.status == core::ContextLookupStatus::DependencyUnavailable
                    ? QStringLiteral("Imported context generation is unavailable")
                    : QStringLiteral(
                          "Imported context was not defined before this structure"),
            };
        }

        RuleImportedContext imported;
        if (importKey.has_value()) {
            imported.key.value = *importKey;
        } else if (lookup.definition) {
            imported.key.value = lookup.definition->key.value;
        }
        imported.definitionId = lookup.definition->id;
        std::vector<core::ContextDefinitionId> included;
        included.reserve(RuleImportedContext::maximumDefinitions());
        DslExecutionStatus closureStatus = DslExecutionStatus::Materialized;
        QString closureError;
        std::function<bool(core::ContextDefinitionId)> appendDefinition;
        appendDefinition = [&](core::ContextDefinitionId definitionId) {
            if (std::find(included.begin(), included.end(), definitionId) !=
                included.end()) {
                return true;
            }
            if (included.size() >= RuleImportedContext::maximumDefinitions()) {
                closureStatus = DslExecutionStatus::ResourceLimit;
                closureError = QStringLiteral(
                    "Imported context dependency closure exceeds 64 definitions");
                return false;
            }
            const auto definition = contextDirectory_.definition(definitionId);
            const ContextPayload* payload = contextPayload(definitionId);
            if (!definition || payload == nullptr) {
                closureStatus = DslExecutionStatus::InvalidDefinition;
                closureError = QStringLiteral(
                    "Imported context generation has no rules-owned payload");
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
            return {closureStatus, nullptr, std::move(closureError)};
        }

        auto [insertedIt, _] = importCache.emplace(cacheKey, std::move(imported));
        return {DslExecutionStatus::Materialized, &insertedIt->second, {}};
    };

    const auto findConsumerStructureIndex =
        [&](quint32 importIndex, core::ContextDefinitionKind kind) -> std::optional<quint32> {
        if (importIndex < headerStructure.contextImports.size() &&
            headerStructure.contextImports.at(importIndex).kind == kind) {
            return request.headerStructureIndex;
        }
        if (payloadStructure != nullptr &&
            importIndex < payloadStructure->contextImports.size() &&
            payloadStructure->contextImports.at(importIndex).kind == kind) {
            return *request.payloadStructureIndex;
        }
        return std::nullopt;
    };

    const DslContextValueResolver contextValueResolver =
        [&](const DslContextValueRequest& valueRequest) {
        const auto consumerStructIndex = findConsumerStructureIndex(
            valueRequest.contextImportIndex, valueRequest.importKind);
        if (!consumerStructIndex.has_value()) {
            return DslContextValueResolution{
                DslExecutionStatus::InvalidDefinition,
                0,
                QStringLiteral("Imported context value request is invalid")};
        }
        const ImportMaterialization materialized = materializeImport(
            *consumerStructIndex,
            valueRequest.contextImportIndex,
            valueRequest.importKey);
        if (!materialized.materialized()) {
            return DslContextValueResolution{
                materialized.status, 0, materialized.errorMessage};
        }

        const RuleImportedContextDefinition* matchedDefinition = nullptr;
        for (const RuleImportedContextDefinition& definition :
             materialized.imported->definitions) {
            if (definition.kind != valueRequest.definitionKind ||
                definition.structureIndex != valueRequest.structureIndex) {
                continue;
            }
            if (matchedDefinition != nullptr) {
                return DslContextValueResolution{
                    DslExecutionStatus::InvalidDefinition,
                    0,
                    QStringLiteral("Imported context value publisher is ambiguous")};
            }
            matchedDefinition = &definition;
        }
        if (matchedDefinition == nullptr ||
            valueRequest.exportIndex >= matchedDefinition->values.size()) {
            return DslContextValueResolution{
                DslExecutionStatus::InvalidDefinition,
                0,
                QStringLiteral(
                    "Imported context value descriptor does not match its payload")};
        }
        return DslContextValueResolution{
            DslExecutionStatus::Materialized,
            matchedDefinition->values.at(valueRequest.exportIndex),
            {}};
    };

    std::optional<core::ContextDefinitionId> publishedDefinitionId;
    std::optional<DslExecutionStatus> importFailureStatus;
    QString importFailureMessage;

    CompoundTransactionHooks compoundHooks;
    compoundHooks.onCommitWithResult = [&](const CompoundStructuralExecutionResult& execRes) {
        // 1. Verify and materialize all declared header context imports
        if (execRes.headerContextImports.size() != headerStructure.contextImports.size()) {
            throw std::runtime_error("Header context import runtime values do not match typed IR");
        }
        for (std::size_t i = 0; i < headerStructure.contextImports.size(); ++i) {
            const DslTypedContextImport& import = headerStructure.contextImports.at(i);
            const DslExecutionContextImport& executionImport = execRes.headerContextImports.at(i);
            if (executionImport.kind != import.kind) {
                throw std::runtime_error("Header context import runtime kind is invalid");
            }
            if (import.keyFieldIndex.has_value()) {
                if (!executionImport.key.has_value() || !executionImport.key->location) {
                    continue;
                }
                const ImportMaterialization materialized = materializeImport(
                    request.headerStructureIndex,
                    static_cast<quint32>(i),
                    executionImport.key->value);
                if (!materialized.materialized()) {
                    importFailureStatus = materialized.status;
                    importFailureMessage = materialized.errorMessage;
                    throw std::runtime_error(materialized.errorMessage.toStdString());
                }
                materialized.imported->key = *executionImport.key;
            }
        }

        // 2. Verify and materialize all declared payload context imports
        if (payloadStructure != nullptr) {
            if (execRes.payloadContextImports.size() != payloadStructure->contextImports.size()) {
                throw std::runtime_error("Payload context import runtime values do not match typed IR");
            }
            for (std::size_t i = 0; i < payloadStructure->contextImports.size(); ++i) {
                const DslTypedContextImport& import = payloadStructure->contextImports.at(i);
                const DslExecutionContextImport& executionImport = execRes.payloadContextImports.at(i);
                if (executionImport.kind != import.kind) {
                    throw std::runtime_error("Payload context import runtime kind is invalid");
                }
                if (import.keyFieldIndex.has_value()) {
                    if (!executionImport.key.has_value() || !executionImport.key->location) {
                        continue;
                    }
                    const ImportMaterialization materialized = materializeImport(
                        *request.payloadStructureIndex,
                        static_cast<quint32>(i),
                        executionImport.key->value);
                    if (!materialized.materialized()) {
                        importFailureStatus = materialized.status;
                        importFailureMessage = materialized.errorMessage;
                        throw std::runtime_error(materialized.errorMessage.toStdString());
                    }
                    materialized.imported->key = *executionImport.key;
                }
            }
        }

        // 3. User commit hooks
        if (request.transactionHooks.onCommitWithResult) {
            request.transactionHooks.onCommitWithResult(execRes);
        }
        if (request.transactionHooks.onCommit) {
            request.transactionHooks.onCommit();
        }

        // 4. Staged publication for Header (after user commit hooks succeed)
        if (headerStructure.contextDefinition.has_value() &&
            execRes.headerNodeId.has_value() &&
            execRes.headerContextValues.has_value()) {
            const DslTypedContextDefinition& def = *headerStructure.contextDefinition;
            const DslExecutionContextValues& vals = *execRes.headerContextValues;
            if (vals.dependencies.size() != def.dependencies.size() ||
                vals.exports.size() != def.exportFieldIndices.size()) {
                throw std::runtime_error("Header context runtime values do not match typed IR");
            }
            std::vector<core::ContextDefinitionId> dependencies;
            dependencies.reserve(def.dependencies.size());
            for (std::size_t i = 0; i < def.dependencies.size(); ++i) {
                const DslTypedContextDependency& dep = def.dependencies.at(i);
                const DslExecutionContextValue& depVal = vals.dependencies.at(i);
                const core::ContextKey depKey{dep.kind, contextScopeId_, depVal.value};
                const auto depLookup = contextDirectory_.resolveBefore(
                    depKey, request.enclosingSourceSpan->start());
                if (!depLookup.found()) {
                    throw std::runtime_error("Context dependency was not defined before this structure");
                }
                dependencies.push_back(depLookup.definition->id);
            }
            ContextPayload payload;
            payload.structureIndex = request.headerStructureIndex;
            payload.values.reserve(vals.exports.size());
            for (const auto& v : vals.exports) {
                payload.values.push_back(v.value);
            }
            core::ContextDefinitionSpec spec{
                {def.kind, contextScopeId_, vals.key.value},
                *request.enclosingSourceSpan,
                *execRes.headerNodeId,
                std::move(dependencies),
            };
            const auto reg = contextDirectory_.registerDefinition(std::move(spec));
            if (!reg.succeeded()) {
                throw std::runtime_error(reg.errorMessage.isEmpty()
                                             ? "Context definition registration failed"
                                             : reg.errorMessage.toStdString());
            }
            payload.definitionId = *reg.definitionId;
            contextPayloads_.push_back(std::move(payload));
            publishedDefinitionId = reg.definitionId;
        }

        // 5. Staged publication for Payload (after user commit hooks succeed)
        if (payloadStructure != nullptr && payloadStructure->contextDefinition.has_value() &&
            execRes.payloadNodeId.has_value() &&
            execRes.payloadContextValues.has_value()) {
            const DslTypedContextDefinition& def = *payloadStructure->contextDefinition;
            const DslExecutionContextValues& vals = *execRes.payloadContextValues;
            if (vals.dependencies.size() != def.dependencies.size() ||
                vals.exports.size() != def.exportFieldIndices.size()) {
                throw std::runtime_error("Payload context runtime values do not match typed IR");
            }
            std::vector<core::ContextDefinitionId> dependencies;
            dependencies.reserve(def.dependencies.size());
            for (std::size_t i = 0; i < def.dependencies.size(); ++i) {
                const DslTypedContextDependency& dep = def.dependencies.at(i);
                const DslExecutionContextValue& depVal = vals.dependencies.at(i);
                const core::ContextKey depKey{dep.kind, contextScopeId_, depVal.value};
                const auto depLookup = contextDirectory_.resolveBefore(
                    depKey, request.enclosingSourceSpan->start());
                if (!depLookup.found()) {
                    throw std::runtime_error("Context dependency was not defined before this structure");
                }
                dependencies.push_back(depLookup.definition->id);
            }
            ContextPayload payload;
            payload.structureIndex = *request.payloadStructureIndex;
            payload.values.reserve(vals.exports.size());
            for (const auto& v : vals.exports) {
                payload.values.push_back(v.value);
            }
            core::ContextDefinitionSpec spec{
                {def.kind, contextScopeId_, vals.key.value},
                *request.enclosingSourceSpan,
                *execRes.payloadNodeId,
                std::move(dependencies),
            };
            const auto reg = contextDirectory_.registerDefinition(std::move(spec));
            if (!reg.succeeded()) {
                throw std::runtime_error(reg.errorMessage.isEmpty()
                                             ? "Context definition registration failed"
                                             : reg.errorMessage.toStdString());
            }
            payload.definitionId = *reg.definitionId;
            contextPayloads_.push_back(std::move(payload));
            publishedDefinitionId = reg.definitionId;
        }
    };
    compoundHooks.onCommit = request.transactionHooks.onCommit;
    compoundHooks.onRollback = request.transactionHooks.onRollback;

    CompoundStructuralExecutionRequest compoundReq;
    compoundReq.source = request.source;
    compoundReq.headerMapping = request.headerMapping;
    compoundReq.headerStructureIndex = request.headerStructureIndex;
    compoundReq.payloadStructureIndex = request.payloadStructureIndex;
    compoundReq.payloadMapping = request.payloadMapping;
    compoundReq.payloadLogicalStart = request.payloadLogicalStart;
    compoundReq.transformProviderId = request.transformProviderId;
    compoundReq.transformRegistry = request.transformRegistry;
    compoundReq.tree = request.tree;
    compoundReq.parentId = request.parentId;
    compoundReq.options = request.options;
    compoundReq.requireExactConsumption = request.requireExactConsumption;
    compoundReq.contextValueResolver = contextValueResolver;
    compoundReq.transactionHooks = compoundHooks;

    result.execution = CompoundStructuralRunner::execute(program_, compoundReq);
    if (importFailureStatus.has_value()) {
        result.status = ruleStatus(*importFailureStatus);
        result.errorMessage = importFailureMessage;
    } else {
        result.status = ruleStatus(result.execution.status);
        result.errorMessage = result.execution.errorMessage;
    }

    if (result.materialized()) {
        source_ = request.source;
        treeIdentity_ = request.tree->instanceIdentity();
        result.publishedDefinition = publishedDefinitionId;
        result.importedContexts.reserve(importCache.size());
        for (auto& [_, imported] : importCache) {
            result.importedContexts.push_back(std::move(imported));
        }
    }
    return result;
}

} // namespace streamview::rules
