#include <streamview/rules/rule_execution_session.h>

#include <streamview/core/bit_reader.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
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

[[nodiscard]] std::optional<std::vector<core::SourceSpan>> enclosingSourceSpans(
    const std::optional<core::SourceSpan>& legacySpan,
    const std::vector<core::SourceSpan>& spans) {
    std::vector<core::SourceSpan> requested = spans;
    if (requested.empty() && legacySpan.has_value()) {
        requested.push_back(*legacySpan);
    }
    const auto normalized =
        core::SourceMapping::create(core::LogicalViewId(1), std::move(requested));
    if (!normalized || normalized->logicalBitLength() == 0) {
        return std::nullopt;
    }
    return normalized->sourceSpans();
}

[[nodiscard]] bool mappingIsWithin(
    const core::SourceMapping& mapping,
    quint64 logicalStart,
    const std::vector<core::SourceSpan>& enclosingSpans) {
    if (logicalStart > mapping.logicalBitLength()) {
        return false;
    }
    const quint64 logicalLength = mapping.logicalBitLength() - logicalStart;
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(mapping.viewId(), logicalStart), logicalLength);
    const auto location = range ? mapping.locate(*range) : std::nullopt;
    return location && std::all_of(
                           location->sourceSpans().begin(), location->sourceSpans().end(),
                           [&enclosingSpans](const core::SourceSpan& span) {
                               return std::any_of(
                                   enclosingSpans.begin(), enclosingSpans.end(),
                                   [&span](const core::SourceSpan& enclosing) {
                                       return enclosing.start() <= span.start() &&
                                              span.endExclusive() <= enclosing.endExclusive();
                                   });
                           });
}

} // namespace

const RuleExecutionSession::ContextPayload*
RuleExecutionSession::contextPayload(core::ContextDefinitionId id) const noexcept {
    if (id.value() == 0 || id.value() > contextPayloads_.size()) {
        return nullptr;
    }
    const ContextPayload& payload = contextPayloads_.at(static_cast<std::size_t>(id.value() - 1));
    return payload.definitionId == id ? &payload : nullptr;
}

RuleExecutionResult RuleExecutionSession::run(const RuleExecutionRequest& request) {
    RuleExecutionResult result;
    if (request.source == nullptr || request.mapping == nullptr || request.tree == nullptr ||
        request.structureIndex >= program_.structs.size()) {
        result.errorMessage = QStringLiteral("Rule execution request is invalid");
        return result;
    }
    if ((source_ != nullptr && source_ != request.source) ||
        (treeIdentity_ != 0 && treeIdentity_ != request.tree->instanceIdentity())) {
        result.errorMessage =
            QStringLiteral("Rule execution session belongs to a different analysis");
        return result;
    }
    if (request.logicalStart > request.mapping->logicalBitLength()) {
        result.errorMessage = QStringLiteral("Rule execution logical start is out of range");
        return result;
    }
    const quint64 logicalLength = request.mapping->logicalBitLength() - request.logicalStart;

    const DslTypedStruct& structure = program_.structs.at(request.structureIndex);
    const bool usesContext = structure.contextDefinition || !structure.contextImports.empty();
    const auto enclosingSpans = enclosingSourceSpans(
        request.enclosingSourceSpan, request.enclosingSourceSpans);
    if (usesContext && !enclosingSpans) {
        result.errorMessage =
            QStringLiteral("Context execution requires a non-empty enclosing source span");
        return result;
    }
    if (usesContext) {
        if (!mappingIsWithin(*request.mapping, request.logicalStart, *enclosingSpans)) {
            result.errorMessage =
                QStringLiteral("Context execution mapping is outside its enclosing source span");
            return result;
        }
    }

    auto reader = core::BitReader::fromMappingSlice(*request.source, *request.mapping,
                                                    request.logicalStart, logicalLength);
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
    std::vector<std::optional<RuleImportedContext>> importCache(structure.contextImports.size());
    const auto materializeImport = [&](quint32 importIndex,
                                       std::optional<quint64> importKey) -> ImportMaterialization {
        if (importIndex >= structure.contextImports.size()) {
            return {DslExecutionStatus::InvalidDefinition, nullptr,
                    QStringLiteral("Context import index is out of range")};
        }
        if (importCache.at(importIndex)) {
            RuleImportedContext& cached = *importCache.at(importIndex);
            if (importKey.has_value()) {
                return cached.key.value == *importKey
                           ? ImportMaterialization{DslExecutionStatus::Materialized, &cached, {}}
                           : ImportMaterialization{
                                 DslExecutionStatus::InvalidDefinition, nullptr,
                                 QStringLiteral("Context import key changed during one execution")};
            }
            return ImportMaterialization{DslExecutionStatus::Materialized, &cached, {}};
        }

        const DslTypedContextImport& import = structure.contextImports.at(importIndex);
        core::ContextLookupResult lookup;
        if (importKey.has_value()) {
            const core::ContextKey key{import.kind, contextScopeId_, *importKey};
            lookup = contextDirectory_.resolveBefore(key, enclosingSpans->front().start());
        } else {
            lookup = contextDirectory_.resolveLatestBefore(import.kind, contextScopeId_,
                                                           enclosingSpans->front().start());
        }
        if (!lookup.found()) {
            return {
                DslExecutionStatus::DependencyUnavailable,
                nullptr,
                lookup.status == core::ContextLookupStatus::DependencyUnavailable
                    ? QStringLiteral("Imported context generation is unavailable")
                    : QStringLiteral("Imported context was not defined before this structure"),
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
            if (std::find(included.begin(), included.end(), definitionId) != included.end()) {
                return true;
            }
            if (included.size() >= RuleImportedContext::maximumDefinitions()) {
                closureStatus = DslExecutionStatus::ResourceLimit;
                closureError =
                    QStringLiteral("Imported context dependency closure exceeds 64 definitions");
                return false;
            }
            const auto definition = contextDirectory_.definition(definitionId);
            const ContextPayload* payload = contextPayload(definitionId);
            if (!definition || payload == nullptr) {
                closureStatus = DslExecutionStatus::InvalidDefinition;
                closureError =
                    QStringLiteral("Imported context generation has no rules-owned payload");
                return false;
            }
            included.push_back(definitionId);
            imported.definitions.push_back({definitionId, definition->key.kind,
                                            payload->structureIndex, payload->values,
                                            definition->dependencies});
            for (const core::ContextDefinitionId dependencyId : definition->dependencies) {
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
        return {DslExecutionStatus::Materialized, &*importCache.at(importIndex), {}};
    };
    const DslContextValueResolver contextValueResolver =
        [&](const DslContextValueRequest& valueRequest) {
            if (valueRequest.contextImportIndex >= structure.contextImports.size() ||
                structure.contextImports.at(valueRequest.contextImportIndex).kind !=
                    valueRequest.importKind) {
                return DslContextValueResolution{
                    DslExecutionStatus::InvalidDefinition, 0,
                    QStringLiteral("Imported context value request is invalid")};
            }
            const ImportMaterialization materialized =
                materializeImport(valueRequest.contextImportIndex, valueRequest.importKey);
            if (!materialized.materialized()) {
                return DslContextValueResolution{materialized.status, 0, materialized.errorMessage};
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
                        DslExecutionStatus::InvalidDefinition, 0,
                        QStringLiteral("Imported context value publisher is ambiguous")};
                }
                matchedDefinition = &definition;
            }
            if (matchedDefinition == nullptr ||
                valueRequest.exportIndex >= matchedDefinition->values.size()) {
                return DslContextValueResolution{
                    DslExecutionStatus::InvalidDefinition, 0,
                    QStringLiteral("Imported context value descriptor does not match its payload")};
            }
            return DslContextValueResolution{DslExecutionStatus::Materialized,
                                             matchedDefinition->values.at(valueRequest.exportIndex),
                                             {}};
        };
    result.execution = DslVirtualMachine::execute(
        program_, request.structureIndex, *reader, *request.mapping, request.logicalStart,
        *request.tree, request.parentId, request.options, contextValueResolver);
    result.status = ruleStatus(result.execution.status);
    result.errorMessage = result.execution.errorMessage;
    if (!result.execution.materialized()) {
        return result;
    }
    if (request.requireExactConsumption && result.execution.bitsConsumed != logicalLength) {
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
        result.errorMessage = QStringLiteral("Context import runtime values do not match typed IR");
        return result;
    }
    std::vector<RuleImportedContext> importedContexts;
    importedContexts.reserve(structure.contextImports.size());
    for (std::size_t importIndex = 0; importIndex < structure.contextImports.size();
         ++importIndex) {
        const DslTypedContextImport& import = structure.contextImports.at(importIndex);
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
                materializeImport(static_cast<quint32>(importIndex), executionImport.key->value);
            if (!materialized.materialized()) {
                result.status = ruleStatus(materialized.status);
                result.errorMessage = materialized.errorMessage;
                core::ParseDiagnostic diagnostic;
                diagnostic.code = result.status == RuleExecutionStatus::ResourceLimit
                                      ? core::DiagnosticCode::ResourceLimit
                                  : result.status == RuleExecutionStatus::DependencyUnavailable
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
        const DslTypedContextDependency& dependency = definition.dependencies.at(index);
        const DslExecutionContextValue& dependencyValue = values.dependencies.at(index);
        const core::ContextKey key{dependency.kind, contextScopeId_, dependencyValue.value};
        const core::ContextLookupResult lookup =
            contextDirectory_.resolveBefore(key, enclosingSpans->front().start());
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
        *enclosingSpans,
        *result.execution.structureNode,
        std::move(dependencies),
    };
    const core::ContextRegistrationResult registration =
        contextDirectory_.registerDefinition(std::move(specification));
    if (!registration.succeeded()) {
        result.status =
            registration.status == core::ContextRegistrationStatus::DependencyUnavailable
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
        diagnostic.fieldPath =
            structure.name + QLatin1Char('.') + structure.fields.at(definition.keyFieldIndex).name;
        diagnostic.location = values.key.location;
        (void)request.tree->addDiagnostic(*result.execution.structureNode, std::move(diagnostic));
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
    compoundInstructionsExecuted_ = 0;
    compoundNodesCreated_ = 0;
    compoundBytesInspected_ = 0;
}

CompoundRuleExecutionResult
RuleExecutionSession::runCompound(const CompoundRuleExecutionRequest& request) {
    CompoundRuleExecutionResult result;
    if (request.source == nullptr || request.headerMapping == nullptr || request.tree == nullptr ||
        request.headerStructureIndex >= program_.structs.size()) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Compound rule execution request is invalid");
        return result;
    }
    if (request.payloadStructureIndex.has_value() && request.autoDispatchPayload) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Cannot specify both explicit payload structure index and auto-dispatch");
        return result;
    }
    if (request.autoDispatchPayload) {
        if (request.payloadMapping == nullptr) {
            result.status = RuleExecutionStatus::InvalidDefinition;
            result.errorMessage =
                QStringLiteral("Auto payload dispatch requires a payload mapping");
            return result;
        }
    } else if (request.payloadStructureIndex.has_value()) {
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

    const DslTypedStruct& headerStructure = program_.structs.at(request.headerStructureIndex);
    const DslTypedStruct* payloadStructure =
        request.payloadStructureIndex.has_value()
            ? &program_.structs.at(*request.payloadStructureIndex)
            : nullptr;

    const bool usesStaticContext =
        headerStructure.contextDefinition || !headerStructure.contextImports.empty() ||
        (payloadStructure != nullptr &&
         (payloadStructure->contextDefinition || !payloadStructure->contextImports.empty()));

    const auto enclosingSpans = enclosingSourceSpans(
        request.enclosingSourceSpan, request.enclosingSourceSpans);
    if (usesStaticContext && !enclosingSpans) {
        result.status = RuleExecutionStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Context execution requires a non-empty enclosing source span");
        return result;
    }
    const auto mappingIsWithinEnclosing = [&enclosingSpans](
                                              const core::SourceMapping& mapping,
                                              quint64 logicalStart) {
        return enclosingSpans && mappingIsWithin(mapping, logicalStart, *enclosingSpans);
    };

    if (usesStaticContext) {
        if (!mappingIsWithinEnclosing(*request.headerMapping, 0)) {
            result.status = RuleExecutionStatus::InvalidDefinition;
            result.errorMessage =
                QStringLiteral("Context execution mapping is outside its enclosing source span");
            return result;
        }

        if (request.payloadMapping != nullptr) {
            if (!mappingIsWithinEnclosing(*request.payloadMapping,
                                          request.payloadLogicalStart)) {
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

    constexpr quint8 headerPhase = 0;
    constexpr quint8 payloadPhase = 1;
    std::map<std::tuple<quint8, quint32, quint32>, RuleImportedContext> importCache;

    const auto materializeImport = [&](quint8 phase, quint32 structureIndex, quint32 importIndex,
                                       std::optional<quint64> importKey) -> ImportMaterialization {
        if (structureIndex >= program_.structs.size()) {
            return {DslExecutionStatus::InvalidDefinition, nullptr,
                    QStringLiteral("Structure index is out of range")};
        }
        const DslTypedStruct& st = program_.structs.at(structureIndex);
        if (importIndex >= st.contextImports.size()) {
            return {DslExecutionStatus::InvalidDefinition, nullptr,
                    QStringLiteral("Context import index is out of range")};
        }
        const auto cacheKey = std::make_tuple(phase, structureIndex, importIndex);
        auto it = importCache.find(cacheKey);
        if (it != importCache.end()) {
            RuleImportedContext& cached = it->second;
            if (importKey.has_value()) {
                return cached.key.value == *importKey
                           ? ImportMaterialization{DslExecutionStatus::Materialized, &cached, {}}
                           : ImportMaterialization{
                                 DslExecutionStatus::InvalidDefinition, nullptr,
                                 QStringLiteral("Context import key changed during one execution")};
            }
            return ImportMaterialization{DslExecutionStatus::Materialized, &cached, {}};
        }

        const DslTypedContextImport& import = st.contextImports.at(importIndex);
        core::ContextLookupResult lookup;
        if (importKey.has_value()) {
            const core::ContextKey key{import.kind, contextScopeId_, *importKey};
            lookup = contextDirectory_.resolveBefore(key, enclosingSpans->front().start());
        } else {
            lookup = contextDirectory_.resolveLatestBefore(import.kind, contextScopeId_,
                                                           enclosingSpans->front().start());
        }
        if (!lookup.found()) {
            return {
                DslExecutionStatus::DependencyUnavailable,
                nullptr,
                lookup.status == core::ContextLookupStatus::DependencyUnavailable
                    ? QStringLiteral("Imported context generation is unavailable")
                    : QStringLiteral("Imported context was not defined before this structure"),
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
            if (std::find(included.begin(), included.end(), definitionId) != included.end()) {
                return true;
            }
            if (included.size() >= RuleImportedContext::maximumDefinitions()) {
                closureStatus = DslExecutionStatus::ResourceLimit;
                closureError =
                    QStringLiteral("Imported context dependency closure exceeds 64 definitions");
                return false;
            }
            const auto definition = contextDirectory_.definition(definitionId);
            const ContextPayload* payload = contextPayload(definitionId);
            if (!definition || payload == nullptr) {
                closureStatus = DslExecutionStatus::InvalidDefinition;
                closureError =
                    QStringLiteral("Imported context generation has no rules-owned payload");
                return false;
            }
            included.push_back(definitionId);
            imported.definitions.push_back({definitionId, definition->key.kind,
                                            payload->structureIndex, payload->values,
                                            definition->dependencies});
            for (const core::ContextDefinitionId dependencyId : definition->dependencies) {
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

    const auto makeContextValueResolver = [&](quint8 phase, quint32 consumerStructureIndex) {
        return DslContextValueResolver{[&, phase, consumerStructureIndex](
                                           const DslContextValueRequest& valueRequest) {
            const DslTypedStruct& consumerStructure = program_.structs.at(consumerStructureIndex);
            if (valueRequest.contextImportIndex >= consumerStructure.contextImports.size() ||
                consumerStructure.contextImports.at(valueRequest.contextImportIndex).kind !=
                    valueRequest.importKind) {
                return DslContextValueResolution{
                    DslExecutionStatus::InvalidDefinition, 0,
                    QStringLiteral("Imported context value request is invalid")};
            }
            const ImportMaterialization materialized =
                materializeImport(phase, consumerStructureIndex, valueRequest.contextImportIndex,
                                  valueRequest.importKey);
            if (!materialized.materialized()) {
                return DslContextValueResolution{materialized.status, 0, materialized.errorMessage};
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
                        DslExecutionStatus::InvalidDefinition, 0,
                        QStringLiteral("Imported context value publisher is ambiguous")};
                }
                matchedDefinition = &definition;
            }
            if (matchedDefinition == nullptr ||
                valueRequest.exportIndex >= matchedDefinition->values.size()) {
                return DslContextValueResolution{
                    DslExecutionStatus::InvalidDefinition, 0,
                    QStringLiteral("Imported context value descriptor does not match its payload")};
            }
            return DslContextValueResolution{DslExecutionStatus::Materialized,
                                             matchedDefinition->values.at(valueRequest.exportIndex),
                                             {}};
        }};
    };
    const DslContextValueResolver headerContextValueResolver =
        makeContextValueResolver(headerPhase, request.headerStructureIndex);
    const DslContextValueResolver payloadContextValueResolver =
        payloadStructure != nullptr
            ? makeContextValueResolver(payloadPhase, *request.payloadStructureIndex)
            : DslContextValueResolver{};

    std::optional<core::ContextDirectory> stagedContextDirectory;
    std::optional<std::vector<ContextPayload>> stagedContextPayloads;
    std::optional<core::ContextDefinitionId> stagedPublishedDefinitionId;
    struct StagingFailure final {
        DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
        QString errorMessage;
    };

    CompoundTransactionHooks compoundHooks;
    compoundHooks.onPrepareCommit = [&](const CompoundStructuralExecutionResult& execRes)
        -> std::optional<CompoundTransactionFailure> {
        try {
            const auto failTransaction = [&](DslExecutionStatus status,
                                             const QString& message) -> void {
                throw StagingFailure{status, message};
            };
            const auto verifyImports = [&](quint8 executionPhase, quint32 structureIndex,
                                           const DslTypedStruct& structure,
                                           const std::vector<DslExecutionContextImport>& imports,
                                           const QString& phaseName) {
                if (imports.size() != structure.contextImports.size()) {
                    failTransaction(
                        DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("%1 context import runtime values do not match typed IR")
                            .arg(phaseName));
                }
                for (std::size_t i = 0; i < structure.contextImports.size(); ++i) {
                    const DslTypedContextImport& import = structure.contextImports.at(i);
                    const DslExecutionContextImport& executionImport = imports.at(i);
                    if (executionImport.kind != import.kind) {
                        failTransaction(DslExecutionStatus::InvalidDefinition,
                                        QStringLiteral("%1 context import runtime kind is invalid")
                                            .arg(phaseName));
                    }
                    if (!import.keyFieldIndex.has_value() || !executionImport.key.has_value() ||
                        !executionImport.key->location) {
                        continue;
                    }
                    const ImportMaterialization materialized =
                        materializeImport(executionPhase, structureIndex, static_cast<quint32>(i),
                                          executionImport.key->value);
                    if (!materialized.materialized()) {
                        failTransaction(materialized.status, materialized.errorMessage);
                    }
                    materialized.imported->key = *executionImport.key;
                }
            };

            const std::optional<quint32> actualPayloadStructureIndex =
                request.payloadStructureIndex.has_value()
                    ? request.payloadStructureIndex
                    : execRes.selectedPayloadStructureIndex;

            verifyImports(headerPhase, request.headerStructureIndex, headerStructure,
                          execRes.headerContextImports, QStringLiteral("Header"));
            if (actualPayloadStructureIndex.has_value() &&
                *actualPayloadStructureIndex < program_.structs.size()) {
                const DslTypedStruct& actualPayloadStructure =
                    program_.structs.at(*actualPayloadStructureIndex);
                verifyImports(payloadPhase, *actualPayloadStructureIndex, actualPayloadStructure,
                              execRes.payloadContextImports, QStringLiteral("Payload"));
            }

            core::ContextDirectory nextDirectory = contextDirectory_;
            std::vector<ContextPayload> nextPayloads = contextPayloads_;
            std::optional<core::ContextDefinitionId> nextPublishedDefinitionId;
            const auto stageDefinition = [&](quint32 structureIndex,
                                             const DslTypedStruct& structure,
                                             std::optional<core::AnalysisNodeId> nodeId,
                                             const std::optional<DslExecutionContextValues>& values,
                                             const QString& phase) {
                if (!structure.contextDefinition.has_value()) {
                    return;
                }
                if (!nodeId.has_value() || !values.has_value()) {
                    failTransaction(
                        DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("%1 context execution did not produce publication values")
                            .arg(phase));
                }

                const DslTypedContextDefinition& definition = *structure.contextDefinition;
                if (values->dependencies.size() != definition.dependencies.size() ||
                    values->exports.size() != definition.exportFieldIndices.size()) {
                    failTransaction(
                        DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("%1 context runtime values do not match typed IR")
                            .arg(phase));
                }

                std::vector<core::ContextDefinitionId> dependencies;
                dependencies.reserve(definition.dependencies.size());
                for (std::size_t i = 0; i < definition.dependencies.size(); ++i) {
                    const DslTypedContextDependency& dependency = definition.dependencies.at(i);
                    const DslExecutionContextValue& dependencyValue = values->dependencies.at(i);
                    const core::ContextKey dependencyKey{dependency.kind, contextScopeId_,
                                                         dependencyValue.value};
                    const auto lookup = nextDirectory.resolveBefore(
                        dependencyKey, enclosingSpans->front().start());
                    if (!lookup.found()) {
                        failTransaction(
                            DslExecutionStatus::DependencyUnavailable,
                            QStringLiteral(
                                "Context dependency was not defined before this structure"));
                    }
                    dependencies.push_back(lookup.definition->id);
                }

                ContextPayload payload;
                payload.structureIndex = structureIndex;
                payload.values.reserve(values->exports.size());
                for (const DslExecutionContextValue& value : values->exports) {
                    payload.values.push_back(value.value);
                }
                core::ContextDefinitionSpec spec{
                    {definition.kind, contextScopeId_, values->key.value},
                    *enclosingSpans,
                    *nodeId,
                    std::move(dependencies),
                };
                const auto registration = nextDirectory.registerDefinition(std::move(spec));
                if (!registration.succeeded()) {
                    failTransaction(registration.status ==
                                            core::ContextRegistrationStatus::DependencyUnavailable
                                        ? DslExecutionStatus::DependencyUnavailable
                                        : DslExecutionStatus::InvalidDefinition,
                                     registration.errorMessage.isEmpty()
                                         ? QStringLiteral("Context definition registration failed")
                                         : registration.errorMessage);
                }
                payload.definitionId = *registration.definitionId;
                nextPayloads.push_back(std::move(payload));
                nextPublishedDefinitionId = registration.definitionId;
            };

            stageDefinition(request.headerStructureIndex, headerStructure, execRes.headerNodeId,
                            execRes.headerContextValues, QStringLiteral("Header"));
            if (actualPayloadStructureIndex.has_value() &&
                *actualPayloadStructureIndex < program_.structs.size()) {
                const DslTypedStruct& actualPayloadStructure =
                    program_.structs.at(*actualPayloadStructureIndex);
                stageDefinition(*actualPayloadStructureIndex, actualPayloadStructure,
                                execRes.payloadNodeId, execRes.payloadContextValues,
                                QStringLiteral("Payload"));
            }

            stagedContextDirectory.emplace(std::move(nextDirectory));
            stagedContextPayloads.emplace(std::move(nextPayloads));
            stagedPublishedDefinitionId = nextPublishedDefinitionId;
            if (request.transactionHooks.onPrepareCommit) {
                return request.transactionHooks.onPrepareCommit(execRes);
            }
            return std::nullopt;
        } catch (const StagingFailure& failure) {
            return CompoundTransactionFailure{failure.status, failure.errorMessage};
        }
    };
    compoundHooks.onCommitWithResult = request.transactionHooks.onCommitWithResult;
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
    const auto applyRemainingBudget = [&](quint64 sessionMaximum, quint64 consumed,
                                          quint64 requestMaximum, quint64* effectiveMaximum,
                                          const QString& budgetName, bool zeroMeansUnlimited) {
        if (sessionMaximum == 0 && zeroMeansUnlimited) {
            return true;
        }
        if (consumed >= sessionMaximum) {
            result.status = RuleExecutionStatus::ResourceLimit;
            result.errorMessage =
                QStringLiteral("Compound session %1 budget is exhausted").arg(budgetName);
            return false;
        }
        const quint64 sessionRemaining = sessionMaximum - consumed;
        *effectiveMaximum = requestMaximum == 0 && zeroMeansUnlimited ? sessionRemaining
                            : requestMaximum == 0                     ? 0
                                                  : std::min(requestMaximum, sessionRemaining);
        return true;
    };
    if (!applyRemainingBudget(compoundLimits_.maximumInstructions, compoundInstructionsExecuted_,
                              request.options.limits.maximumInstructions,
                              &compoundReq.options.limits.maximumInstructions,
                              QStringLiteral("instruction"), false) ||
        !applyRemainingBudget(compoundLimits_.maximumMaterializedNodes, compoundNodesCreated_,
                              request.options.limits.maximumMaterializedNodes,
                              &compoundReq.options.limits.maximumMaterializedNodes,
                              QStringLiteral("materialized-node"), false)) {
        return result;
    }
    if (request.payloadStructureIndex.has_value() || request.autoDispatchPayload) {
        const bool autoInspectionExhausted =
            request.autoDispatchPayload && compoundLimits_.maximumInspectedBytes != 0 &&
            compoundBytesInspected_ >= compoundLimits_.maximumInspectedBytes;
        if (autoInspectionExhausted) {
            compoundReq.options.limits.maximumInspectedBytes = 0;
            compoundReq.inspectionBudgetExhausted = true;
        } else if (!applyRemainingBudget(compoundLimits_.maximumInspectedBytes,
                                         compoundBytesInspected_,
                                         request.options.limits.maximumInspectedBytes,
                                         &compoundReq.options.limits.maximumInspectedBytes,
                                         QStringLiteral("inspection"), true)) {
            return result;
        }
    }
    compoundReq.requireExactConsumption = request.requireExactConsumption;
    compoundReq.headerContextValueResolver = headerContextValueResolver;
    compoundReq.payloadContextValueResolver = payloadContextValueResolver;
    compoundReq.autoDispatchPayload = request.autoDispatchPayload;
    compoundReq.payloadContextResolverFactory = [&](quint32 payloadStructureIndex) {
        if (payloadStructureIndex >= program_.structs.size()) {
            throw std::runtime_error("Selected payload structure index is out of range");
        }
        const DslTypedStruct& selectedPayload = program_.structs.at(payloadStructureIndex);
        const bool selectedPayloadUsesContext =
            selectedPayload.contextDefinition || !selectedPayload.contextImports.empty();
        if (selectedPayloadUsesContext) {
            if (!enclosingSpans) {
                throw std::runtime_error(
                    "Selected payload context requires a non-empty enclosing source span");
            }
            if (!mappingIsWithinEnclosing(*request.headerMapping, 0) ||
                request.payloadMapping == nullptr ||
                !mappingIsWithinEnclosing(*request.payloadMapping,
                                          request.payloadLogicalStart)) {
                throw std::runtime_error(
                    "Selected payload context mapping is outside its enclosing source span");
            }
        }
        return makeContextValueResolver(payloadPhase, payloadStructureIndex);
    };
    compoundReq.transactionHooks = compoundHooks;

    source_ = request.source;
    treeIdentity_ = request.tree->instanceIdentity();
    result.execution = CompoundStructuralRunner::execute(program_, compoundReq);
    const auto accumulate = [](quint64* total, quint64 amount) {
        *total = amount > std::numeric_limits<quint64>::max() - *total
                     ? std::numeric_limits<quint64>::max()
                     : *total + amount;
    };
    accumulate(&compoundInstructionsExecuted_, result.execution.instructionsExecuted);
    accumulate(&compoundNodesCreated_, result.execution.nodesCreated);
    accumulate(&compoundBytesInspected_, result.execution.inspectedByteCount);

    result.status = ruleStatus(result.execution.status);
    result.errorMessage = result.execution.errorMessage;

    if (result.materialized()) {
        if (!stagedContextDirectory.has_value() || !stagedContextPayloads.has_value()) {
            result.status = RuleExecutionStatus::InvalidDefinition;
            result.errorMessage =
                QStringLiteral("Compound context transaction did not reach commit");
            return result;
        }
        contextDirectory_ = std::move(*stagedContextDirectory);
        contextPayloads_ = std::move(*stagedContextPayloads);
        result.publishedDefinition = stagedPublishedDefinitionId;
        result.importedContexts.reserve(importCache.size());
        for (auto& [_, imported] : importCache) {
            result.importedContexts.push_back(std::move(imported));
        }
    }
    return result;
}

} // namespace streamview::rules
