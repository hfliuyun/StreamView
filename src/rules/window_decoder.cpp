#include <streamview/rules/window_decoder.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl_executor.h>

#include <algorithm>
#include <limits>

namespace streamview::rules {

WindowDecoder::WindowDecoder(
    const DslTypedProgram& program,
    const core::RandomAccessSource& source,
    core::SourceMapping sourceMapping,
    std::shared_ptr<core::AnalysisTree> tree,
    core::AnalysisNodeId windowNodeId,
    std::shared_ptr<RunnerExecutionBudget> budget,
    std::optional<core::CancellationToken> cancellation)
    : program_(&program)
    , source_(&source)
    , sourceMapping_(std::move(sourceMapping))
    , tree_(std::move(tree))
    , windowNodeId_(windowNodeId)
    , budget_(std::move(budget))
    , cancellation_(std::move(cancellation)) {}

WindowDecodeResult WindowDecoder::decodeWindow(const WindowDecodeRequest& request) {
    WindowDecodeResult result;

    if (request.pageSize == 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Page size must be greater than zero");
        return result;
    }

    if (!tree_) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("AnalysisTree is not available");
        return result;
    }

    auto nodeOpt = tree_->node(windowNodeId_);
    if (!nodeOpt.has_value()) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Window node does not exist in analysis tree");
        return result;
    }

    const auto& meta = nodeOpt->metadata();
    if (!meta.window.has_value()) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Node does not contain window metadata");
        return result;
    }

    const auto& windowMeta = *meta.window;
    if (!program_ || windowMeta.entryStructIndex >= program_->structs.size()) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Window entry struct index is out of range");
        return result;
    }

    if (windowMeta.entrySizeBits == 0 || (windowMeta.entrySizeBits % 8U) != 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Window entry size must be non-zero and byte-aligned");
        return result;
    }

    const auto& locationOpt = nodeOpt->location();
    if (!locationOpt.has_value()) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Window node does not have a valid location");
        return result;
    }

    const quint64 regionBitLength = locationOpt->logicalRange().bitLength();
    const quint64 availableRegionBytes = regionBitLength / 8U;
    const quint64 entrySizeBytes = windowMeta.entrySizeBits / 8U;
    const quint64 clampedCount = std::min(windowMeta.entryCount, availableRegionBytes / entrySizeBytes);

    quint64 startIndex = 0;
    if (__builtin_mul_overflow(request.pageIndex, request.pageSize, &startIndex)) {
        result.status = DslExecutionStatus::TruncatedSource;
        result.decodedEntryCount = 0;
        return result;
    }

    if (startIndex >= clampedCount) {
        result.status = DslExecutionStatus::TruncatedSource;
        result.decodedEntryCount = 0;
        return result;
    }

    const quint64 remainingEntries = clampedCount - startIndex;
    const quint64 pageCount = std::min(request.pageSize, remainingEntries);

    result.entryNodes.reserve(static_cast<std::size_t>(pageCount));

    (void)tree_->transition(windowNodeId_, core::MaterializationState::Indexing);

    for (quint64 i = 0; i < pageCount; ++i) {
        if (cancellation_ && cancellation_->isCancellationRequested()) {
            result.status = DslExecutionStatus::Cancelled;
            (void)tree_->transition(windowNodeId_, result.decodedEntryCount > 0 ? core::MaterializationState::Materialized : core::MaterializationState::Cancelled);
            return result;
        }
        if (budget_ && budget_->cancellation && budget_->cancellation->isCancellationRequested()) {
            result.status = DslExecutionStatus::Cancelled;
            (void)tree_->transition(windowNodeId_, result.decodedEntryCount > 0 ? core::MaterializationState::Materialized : core::MaterializationState::Cancelled);
            return result;
        }
        if (budget_ && (budget_->remainingNodes == 0 || budget_->remainingInstructions == 0)) {
            result.status = DslExecutionStatus::ResourceLimit;
            (void)tree_->transition(windowNodeId_, result.decodedEntryCount > 0 ? core::MaterializationState::Materialized : core::MaterializationState::Invalid);
            return result;
        }

        const quint64 entryIndex = startIndex + i;
        quint64 entryBitOffset = 0;
        if (__builtin_mul_overflow(entryIndex, windowMeta.entrySizeBits, &entryBitOffset)) {
            result.status = DslExecutionStatus::SourceError;
            result.errorMessage = QStringLiteral("Entry bit offset calculation overflow");
            return result;
        }

        quint64 entryLogicalStart = 0;
        if (__builtin_add_overflow(locationOpt->logicalRange().start().bitOffset(), entryBitOffset, &entryLogicalStart)) {
            result.status = DslExecutionStatus::SourceError;
            result.errorMessage = QStringLiteral("Entry logical address calculation overflow");
            return result;
        }

        auto entryLogicalRange = core::LogicalRange::create(
            core::LogicalBitAddress(sourceMapping_.viewId(), entryLogicalStart), windowMeta.entrySizeBits);
        if (!entryLogicalRange.has_value()) {
            result.status = DslExecutionStatus::SourceError;
            result.errorMessage = QStringLiteral("Failed to create entry logical bit range");
            return result;
        }

        auto locateRes = sourceMapping_.locate(*entryLogicalRange);
        if (!locateRes.has_value() || locateRes->sourceSpans().empty()) {
            result.status = DslExecutionStatus::SourceError;
            result.errorMessage = QStringLiteral("Failed to locate entry source spans");
            return result;
        }

        auto entryMapping = core::SourceMapping::create(sourceMapping_.viewId(), locateRes->sourceSpans());
        if (!entryMapping.has_value()) {
            result.status = DslExecutionStatus::SourceError;
            result.errorMessage = QStringLiteral("Failed to create entry source mapping");
            return result;
        }

        core::BitReader entryReader(*source_, *entryMapping);

        DslExecutionOptions options;
        if (budget_) {
            options.limits.maximumMaterializedNodes = budget_->remainingNodes;
            options.limits.maximumInstructions = budget_->remainingInstructions;
            if (budget_->cancellation) {
                options.cancellation = budget_->cancellation;
            }
        }
        if (cancellation_) {
            options.cancellation = cancellation_;
        }

        auto execResult = DslExecutor::decodeStruct(
            *program_, windowMeta.entryStructIndex, entryReader, *entryMapping, 0, *tree_, windowNodeId_, options);

        if (budget_) {
            budget_->remainingNodes = (execResult.nodesCreated >= budget_->remainingNodes)
                                          ? 0
                                          : (budget_->remainingNodes - execResult.nodesCreated);
            budget_->remainingInstructions =
                (execResult.instructionsExecuted >= budget_->remainingInstructions)
                    ? 0
                    : (budget_->remainingInstructions - execResult.instructionsExecuted);
        }

        if (execResult.status != DslExecutionStatus::Materialized) {
            result.status = execResult.status;
            result.errorMessage = execResult.errorMessage;
            (void)tree_->transition(windowNodeId_, result.decodedEntryCount > 0 ? core::MaterializationState::Materialized : core::MaterializationState::Invalid);
            return result;
        }

        if (execResult.structureNode.has_value()) {
            result.entryNodes.push_back(*execResult.structureNode);
        }
        result.decodedEntryCount++;
    }

    (void)tree_->transition(windowNodeId_, core::MaterializationState::Materialized);

    if (startIndex + pageCount < (request.pageIndex + 1) * request.pageSize) {
        if (startIndex + pageCount < windowMeta.entryCount) {
            result.status = DslExecutionStatus::TruncatedSource;
            return result;
        }
    }

    result.status = DslExecutionStatus::Materialized;
    return result;
}

} // namespace streamview::rules
