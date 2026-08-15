#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/aac_adts_scanner.h>
#include <streamview/rules/rule_catalog.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::rules {

enum class AacAdtsAnalysisStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
    ResourceLimit,
    InvalidRule,
};

struct AacAdtsAnalysisBatch final {
    AacAdtsAnalysisStatus status = AacAdtsAnalysisStatus::InProgress;
    std::vector<core::AnalysisNodeId> frameNodes;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == AacAdtsAnalysisStatus::Complete;
    }
};

[[nodiscard]] QString aacAdtsRuleSource(QString* errorMessage = nullptr);
[[nodiscard]] RulePackageLoadResult loadAacAdtsRulePackage();

class AacAdtsAnalyzer final {
public:
    [[nodiscard]] static std::optional<AacAdtsAnalyzer>
    create(const core::RandomAccessSource& source,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    [[nodiscard]] static std::optional<AacAdtsAnalyzer>
    create(const core::RandomAccessSource& source,
           const RuleCatalogLookupResult& resolvedRule,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    AacAdtsAnalyzer(const AacAdtsAnalyzer&) = delete;
    AacAdtsAnalyzer(AacAdtsAnalyzer&&) noexcept = default;
    AacAdtsAnalyzer& operator=(const AacAdtsAnalyzer&) = delete;
    AacAdtsAnalyzer& operator=(AacAdtsAnalyzer&&) noexcept = default;

    /**
     * @brief Performs incremental batch analysis on the ADTS stream.
     *
     * Work budget accounting semantics:
     * - Fast path (in-sync): advances by frame header bytes per inspected frame.
     * - Resynchronization path: advances byte-by-byte for each byte searched during sync recovery.
     */
    [[nodiscard]] AacAdtsAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = 256U * 1024U);

    [[nodiscard]] bool resumeAfterCancellation(
        std::optional<core::CancellationToken> cancellation = std::nullopt,
        QString* errorMessage = nullptr);

    [[nodiscard]] const core::AnalysisTree& tree() const noexcept { return tree_; }
    [[nodiscard]] bool finished() const noexcept { return terminal_; }
    [[nodiscard]] quint64 scanCursor() const noexcept { return scanner_.cursor(); }
    [[nodiscard]] const RuleEntryPointIdentity& ruleIdentity() const noexcept {
        return ruleIdentity_;
    }

private:
    explicit AacAdtsAnalyzer(const core::RandomAccessSource& source,
                             RuleEntryPointIdentity ruleIdentity,
                             std::optional<core::CancellationToken> cancellation);

    AacAdtsScanner scanner_;
    core::AnalysisTree tree_;
    RuleEntryPointIdentity ruleIdentity_;
    quint64 nextViewId_ = 1;
    bool terminal_ = false;
};

} // namespace streamview::rules
