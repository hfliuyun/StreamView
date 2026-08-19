#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>

#include <QString>
#include <QtGlobal>

#include <compare>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace streamview::core {

enum class ContextDefinitionKind : quint8 {
    H264SequenceParameterSet,
    H264PictureParameterSet,
    AacAudioSpecificConfig,
    IsoBmffSampleDescription,
};

class ContextDefinitionId final {
public:
    constexpr explicit ContextDefinitionId(quint64 value = 0) noexcept : value_(value) {}

    [[nodiscard]] constexpr quint64 value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const ContextDefinitionId&,
                                      const ContextDefinitionId&) = default;

private:
    quint64 value_ = 0;
};

struct ContextKey final {
    ContextDefinitionKind kind = ContextDefinitionKind::H264SequenceParameterSet;
    quint64 scopeId = 0;
    quint64 value = 0;

    friend constexpr auto operator<=>(const ContextKey&, const ContextKey&) = default;
};

struct ContextDefinitionSpec final {
    ContextKey key;
    std::vector<SourceSpan> sourceSpans;
    AnalysisNodeId analysisNodeId;
    std::vector<ContextDefinitionId> dependencies;
};

struct ContextDefinition final {
    ContextDefinitionId id;
    ContextKey key;
    std::vector<SourceSpan> sourceSpans;
    AnalysisNodeId analysisNodeId;
    std::vector<ContextDefinitionId> dependencies;
};

enum class ContextRegistrationStatus : quint8 {
    Registered,
    InvalidDefinition,
    DuplicateDefinition,
    DependencyUnavailable,
};

struct ContextRegistrationResult final {
    ContextRegistrationStatus status = ContextRegistrationStatus::InvalidDefinition;
    std::optional<ContextDefinitionId> definitionId;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == ContextRegistrationStatus::Registered;
    }
};

enum class ContextLookupStatus : quint8 {
    Found,
    NotFound,
    DependencyUnavailable,
};

struct ContextLookupResult final {
    ContextLookupStatus status = ContextLookupStatus::NotFound;
    std::optional<ContextDefinition> definition;
    std::optional<ContextDefinitionId> unavailableDependency;

    [[nodiscard]] bool found() const noexcept { return status == ContextLookupStatus::Found; }
};

class ContextDirectory final {
public:
    [[nodiscard]] static constexpr std::size_t maximumDependencyDepth() noexcept { return 64; }

    [[nodiscard]] std::size_t definitionCount() const noexcept { return definitions_.size(); }
    [[nodiscard]] std::optional<ContextDefinition>
    definition(ContextDefinitionId id) const;

    [[nodiscard]] ContextRegistrationResult registerDefinition(ContextDefinitionSpec spec);
    [[nodiscard]] ContextLookupResult resolveBefore(const ContextKey& key,
                                                    SourceBitAddress sourcePosition) const;
    [[nodiscard]] ContextLookupResult resolveLatestBefore(ContextDefinitionKind kind,
                                                          quint64 scopeId,
                                                          SourceBitAddress sourcePosition) const;

private:
    [[nodiscard]] ContextLookupResult
    resolveBefore(const ContextKey& key,
                  SourceBitAddress sourcePosition,
                  std::vector<ContextDefinitionId>& resolutionStack) const;
    [[nodiscard]] ContextLookupResult
    resolveLatestBefore(ContextDefinitionKind kind,
                        quint64 scopeId,
                        SourceBitAddress sourcePosition,
                        std::vector<ContextDefinitionId>& resolutionStack) const;

    std::vector<ContextDefinition> definitions_;
    std::map<ContextKey, std::vector<ContextDefinitionId>> definitionsByKey_;
};

} // namespace streamview::core
