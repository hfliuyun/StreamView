#include <streamview/core/context_directory.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace streamview::core {
namespace {

bool isKnownKind(ContextDefinitionKind kind) noexcept {
    switch (kind) {
    case ContextDefinitionKind::H264SequenceParameterSet:
    case ContextDefinitionKind::H264PictureParameterSet:
    case ContextDefinitionKind::AacAudioSpecificConfig:
    case ContextDefinitionKind::IsoBmffSampleDescription:
        return true;
    }
    return false;
}

SourceBitAddress sourceStart(const std::vector<SourceSpan>& spans) noexcept {
    return spans.front().start();
}

SourceBitAddress sourceEnd(const std::vector<SourceSpan>& spans) noexcept {
    return spans.back().endExclusive();
}

bool spansOverlap(const std::vector<SourceSpan>& left,
                  const std::vector<SourceSpan>& right) noexcept {
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;
    while (leftIndex < left.size() && rightIndex < right.size()) {
        const SourceSpan& leftSpan = left.at(leftIndex);
        const SourceSpan& rightSpan = right.at(rightIndex);
        if (leftSpan.start() < rightSpan.endExclusive() &&
            rightSpan.start() < leftSpan.endExclusive()) {
            return true;
        }
        if (leftSpan.endExclusive() <= rightSpan.start()) {
            ++leftIndex;
        } else {
            ++rightIndex;
        }
    }
    return false;
}

} // namespace

std::optional<ContextDefinition>
ContextDirectory::definition(ContextDefinitionId id) const {
    if (id.value() == 0 || id.value() > definitions_.size()) {
        return std::nullopt;
    }
    return definitions_.at(static_cast<std::size_t>(id.value() - 1));
}

ContextRegistrationResult
ContextDirectory::registerDefinition(ContextDefinitionSpec spec) {
    auto normalizedLocation = SourceMapping::create(LogicalViewId(1), spec.sourceSpans);
    if (!isKnownKind(spec.key.kind) || !normalizedLocation ||
        normalizedLocation->logicalBitLength() == 0 || spec.analysisNodeId.value() == 0) {
        return {ContextRegistrationStatus::InvalidDefinition,
                std::nullopt,
                QStringLiteral("Context definition metadata is invalid")};
    }
    spec.sourceSpans = normalizedLocation->sourceSpans();
    std::vector<ContextDefinitionId> checkedDependencies;
    checkedDependencies.reserve(spec.dependencies.size());
    for (const auto dependencyId : spec.dependencies) {
        const auto dependency = definition(dependencyId);
        if (!dependency || dependency->key == spec.key ||
            std::find(checkedDependencies.begin(), checkedDependencies.end(), dependencyId) !=
                checkedDependencies.end()) {
            return {ContextRegistrationStatus::InvalidDefinition,
                    std::nullopt,
                    QStringLiteral("Context definition dependency metadata is invalid")};
        }
        const auto currentDependency = resolveBefore(dependency->key,
                                                     sourceStart(spec.sourceSpans));
        if (!currentDependency.found() ||
            currentDependency.definition->id != dependencyId) {
            return {ContextRegistrationStatus::DependencyUnavailable,
                    std::nullopt,
                    QStringLiteral("Context definition dependency is unavailable")};
        }
        checkedDependencies.push_back(dependencyId);
    }

    if (definitions_.size() >= std::numeric_limits<quint64>::max()) {
        return {ContextRegistrationStatus::InvalidDefinition,
                std::nullopt,
                QStringLiteral("Context definition identifier space is exhausted")};
    }

    const auto [keyEntry, insertedKey] = definitionsByKey_.try_emplace(spec.key);
    auto& keyDefinitions = keyEntry->second;
    try {
        keyDefinitions.reserve(keyDefinitions.size() + 1);
    } catch (...) {
        if (insertedKey) {
            definitionsByKey_.erase(keyEntry);
        }
        throw;
    }
    const bool overlapsExisting = std::any_of(
        keyDefinitions.begin(), keyDefinitions.end(), [this, &spec](ContextDefinitionId id) {
            return spansOverlap(
                definitions_.at(static_cast<std::size_t>(id.value() - 1)).sourceSpans,
                spec.sourceSpans);
        });
    if (overlapsExisting) {
        return {ContextRegistrationStatus::DuplicateDefinition,
                std::nullopt,
                QStringLiteral("Context definitions for one key cannot overlap")};
    }
    const auto insertion = std::lower_bound(
        keyDefinitions.begin(),
        keyDefinitions.end(),
        sourceEnd(spec.sourceSpans),
        [this](ContextDefinitionId existingId, SourceBitAddress position) {
            return definitions_.at(static_cast<std::size_t>(existingId.value() - 1))
                       .sourceSpans.back().endExclusive() < position;
        });

    const ContextDefinitionId id(static_cast<quint64>(definitions_.size()) + 1);
    try {
        definitions_.push_back(ContextDefinition{id,
                                                 spec.key,
                                                 std::move(spec.sourceSpans),
                                                 spec.analysisNodeId,
                                                 std::move(spec.dependencies)});
    } catch (...) {
        if (insertedKey) {
            definitionsByKey_.erase(keyEntry);
        }
        throw;
    }
    keyDefinitions.insert(insertion, id);
    return {ContextRegistrationStatus::Registered, id, {}};
}

ContextLookupResult
ContextDirectory::resolveBefore(const ContextKey& key, SourceBitAddress sourcePosition) const {
    std::vector<ContextDefinitionId> resolutionStack;
    return resolveBefore(key, sourcePosition, resolutionStack);
}

ContextLookupResult ContextDirectory::resolveBefore(
    const ContextKey& key,
    SourceBitAddress sourcePosition,
    std::vector<ContextDefinitionId>& resolutionStack) const {
    const auto found = definitionsByKey_.find(key);
    if (found == definitionsByKey_.end()) {
        return {};
    }

    const auto firstAfterPosition = std::upper_bound(
        found->second.begin(),
        found->second.end(),
        sourcePosition,
        [this](SourceBitAddress position, ContextDefinitionId definitionId) {
            return position < definitions_.at(static_cast<std::size_t>(definitionId.value() - 1))
                                  .sourceSpans.back().endExclusive();
        });
    if (firstAfterPosition == found->second.begin()) {
        return {};
    }

    const auto candidateId = *std::prev(firstAfterPosition);
    const auto& candidate =
        definitions_.at(static_cast<std::size_t>(candidateId.value() - 1));
    if (std::find(resolutionStack.begin(), resolutionStack.end(), candidate.id) !=
        resolutionStack.end()) {
        return {ContextLookupStatus::DependencyUnavailable, std::nullopt, candidate.id};
    }
    if (resolutionStack.size() >= maximumDependencyDepth()) {
        return {ContextLookupStatus::DependencyUnavailable, std::nullopt, candidate.id};
    }

    resolutionStack.push_back(candidate.id);
    for (const auto dependencyId : candidate.dependencies) {
        const auto dependency = definition(dependencyId);
        if (!dependency) {
            resolutionStack.pop_back();
            return {ContextLookupStatus::DependencyUnavailable,
                    std::nullopt,
                    dependencyId};
        }
        const auto currentDependency =
            resolveBefore(dependency->key, sourcePosition, resolutionStack);
        if (!currentDependency.found()) {
            resolutionStack.pop_back();
            return {ContextLookupStatus::DependencyUnavailable,
                    std::nullopt,
                    currentDependency.unavailableDependency.value_or(dependencyId)};
        }
        if (currentDependency.definition->id != dependencyId) {
            resolutionStack.pop_back();
            return {ContextLookupStatus::DependencyUnavailable,
                    std::nullopt,
                    dependencyId};
        }
    }
    resolutionStack.pop_back();
    return {ContextLookupStatus::Found, candidate, std::nullopt};
}

ContextLookupResult
ContextDirectory::resolveLatestBefore(ContextDefinitionKind kind,
                                     quint64 scopeId,
                                     SourceBitAddress sourcePosition) const {
    std::vector<ContextDefinitionId> resolutionStack;
    return resolveLatestBefore(kind, scopeId, sourcePosition, resolutionStack);
}

ContextLookupResult ContextDirectory::resolveLatestBefore(
    ContextDefinitionKind kind,
    quint64 scopeId,
    SourceBitAddress sourcePosition,
    std::vector<ContextDefinitionId>& resolutionStack) const {
    const ContextKey startKey{kind, scopeId, 0};
    const ContextKey endKey{kind, scopeId, std::numeric_limits<quint64>::max()};
    const auto lower = definitionsByKey_.lower_bound(startKey);
    const auto upper = definitionsByKey_.upper_bound(endKey);
    if (lower == upper) {
        return {};
    }

    std::optional<ContextDefinitionId> latestCandidateId;
    SourceBitAddress latestEndPosition(0);

    for (auto it = lower; it != upper; ++it) {
        const auto firstAfterPosition = std::upper_bound(
            it->second.begin(),
            it->second.end(),
            sourcePosition,
            [this](SourceBitAddress position, ContextDefinitionId definitionId) {
                return position < definitions_.at(static_cast<std::size_t>(definitionId.value() - 1))
                                      .sourceSpans.back().endExclusive();
            });
        if (firstAfterPosition == it->second.begin()) {
            continue;
        }
        const auto candidateId = *std::prev(firstAfterPosition);
        const auto& candidate =
            definitions_.at(static_cast<std::size_t>(candidateId.value() - 1));
        if (!latestCandidateId.has_value() ||
            candidate.sourceSpans.back().endExclusive() > latestEndPosition) {
            latestCandidateId = candidateId;
            latestEndPosition = candidate.sourceSpans.back().endExclusive();
        }
    }

    if (!latestCandidateId.has_value()) {
        return {};
    }

    const auto& candidate =
        definitions_.at(static_cast<std::size_t>(latestCandidateId->value() - 1));
    return resolveBefore(candidate.key, sourcePosition, resolutionStack);
}

} // namespace streamview::core
