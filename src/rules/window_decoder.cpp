#include <streamview/rules/window_decoder.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl_executor.h>

#include <algorithm>
#include <limits>
#include <map>

namespace streamview::rules {

namespace {

[[nodiscard]] constexpr bool addWouldOverflow(quint64 left, quint64 right) noexcept {
    return std::numeric_limits<quint64>::max() - left < right;
}

[[nodiscard]] constexpr bool multiplyWouldOverflow(quint64 left, quint64 right) noexcept {
    if (left == 0 || right == 0) {
        return false;
    }
    return left > std::numeric_limits<quint64>::max() / right;
}

} // namespace

struct WindowDecoder::State final {
    std::map<quint64, core::AnalysisNodeId> entryNodes;
};

std::shared_ptr<WindowDecoder::State> WindowDecoder::createState() {
    return std::make_shared<State>();
}

WindowDecoder::WindowDecoder(
    const DslTypedProgram& program,
    const core::RandomAccessSource& source,
    core::SourceMapping sourceMapping,
    std::shared_ptr<core::AnalysisTree> tree,
    core::AnalysisNodeId windowNodeId,
    std::shared_ptr<RunnerExecutionBudget> budget,
    std::optional<core::CancellationToken> cancellation)
    : WindowDecoder(program,
                    source,
                    std::move(sourceMapping),
                    std::move(tree),
                    windowNodeId,
                    std::move(budget),
                    createState(),
                    std::move(cancellation)) {}

WindowDecoder::WindowDecoder(
    const DslTypedProgram& program,
    const core::RandomAccessSource& source,
    core::SourceMapping sourceMapping,
    std::shared_ptr<core::AnalysisTree> tree,
    core::AnalysisNodeId windowNodeId,
    std::shared_ptr<RunnerExecutionBudget> budget,
    std::shared_ptr<State> state,
    std::optional<core::CancellationToken> cancellation)
    : program_(&program)
    , source_(&source)
    , sourceMapping_(std::move(sourceMapping))
    , tree_(std::move(tree))
    , windowNodeId_(windowNodeId)
    , budget_(std::move(budget))
    , state_(std::move(state))
    , cancellation_(std::move(cancellation)) {
    if (!state_) {
        state_ = std::make_shared<State>();
    }
    if (tree_) {
        const auto node = tree_->node(windowNodeId_);
        if (node && node->location()) {
            const auto windowMapping = core::SourceMapping::create(
                sourceMapping_.viewId(), node->location()->sourceSpans());
            if (windowMapping) {
                sourceMapping_ = *windowMapping;
            }
        }
    }
}

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

    if (multiplyWouldOverflow(request.pageIndex, request.pageSize)) {
        result.status = DslExecutionStatus::TruncatedSource;
        result.decodedEntryCount = 0;
        return result;
    }
    const quint64 startIndex = request.pageIndex * request.pageSize;

    if (startIndex >= clampedCount) {
        result.status = DslExecutionStatus::TruncatedSource;
        result.decodedEntryCount = 0;
        return result;
    }

    const quint64 remainingEntries = clampedCount - startIndex;
    const quint64 pageCount = std::min(request.pageSize, remainingEntries);
    if (addWouldOverflow(startIndex, pageCount)) {
        result.status = DslExecutionStatus::TruncatedSource;
        return result;
    }
    const quint64 pageEndIndex = startIndex + pageCount;
    if (pageCount > std::numeric_limits<std::size_t>::max()) {
        result.status = DslExecutionStatus::ResourceLimit;
        result.errorMessage = QStringLiteral("Requested page exceeds host container limits");
        return result;
    }

    result.entryNodes.reserve(static_cast<std::size_t>(pageCount));

    bool needsMaterialization = false;
    for (quint64 entryIndex = startIndex; entryIndex < pageEndIndex; ++entryIndex) {
        if (!state_->entryNodes.contains(entryIndex)) {
            needsMaterialization = true;
            break;
        }
    }
    if (needsMaterialization) {
        const auto currentNode = tree_->node(windowNodeId_);
        if (!currentNode) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Window node disappeared from analysis tree");
            return result;
        }
        bool enteredIndexing = currentNode->state() == core::MaterializationState::Indexing;
        if (currentNode->state() == core::MaterializationState::Cancelled) {
            enteredIndexing = tree_->resumeCancelled(windowNodeId_);
        } else if (!enteredIndexing) {
            enteredIndexing = tree_->transition(
                windowNodeId_, core::MaterializationState::Indexing);
        }
        if (!enteredIndexing) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage =
                QStringLiteral("Window node cannot accept additional decoded entries");
            return result;
        }
    }

    for (quint64 i = 0; i < pageCount; ++i) {
        const quint64 entryIndex = startIndex + i;
        if (const auto existing = state_->entryNodes.find(entryIndex);
            existing != state_->entryNodes.end()) {
            result.entryNodes.push_back(existing->second);
            ++result.decodedEntryCount;
            continue;
        }

        if (cancellation_ && cancellation_->isCancellationRequested()) {
            result.status = DslExecutionStatus::Cancelled;
            return result;
        }
        if (budget_ && budget_->cancellation && budget_->cancellation->isCancellationRequested()) {
            result.status = DslExecutionStatus::Cancelled;
            return result;
        }
        if (budget_ && (budget_->remainingNodes == 0 || budget_->remainingInstructions == 0)) {
            result.status = DslExecutionStatus::ResourceLimit;
            return result;
        }

        if (multiplyWouldOverflow(entryIndex, windowMeta.entrySizeBits)) {
            result.status = DslExecutionStatus::SourceError;
            result.errorMessage = QStringLiteral("Entry bit offset calculation overflow");
            return result;
        }
        const quint64 entryBitOffset = entryIndex * windowMeta.entrySizeBits;

        auto entryLogicalRange = core::LogicalRange::create(
            core::LogicalBitAddress(sourceMapping_.viewId(), entryBitOffset),
            windowMeta.entrySizeBits);
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

        auto treeSnapshot = tree_->snapshot(windowNodeId_);
        auto execResult = DslExecutor::decodeStruct(*program_,
                                                    windowMeta.entryStructIndex,
                                                    entryReader,
                                                    *entryMapping,
                                                    0,
                                                    *tree_,
                                                    windowNodeId_,
                                                    options);

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
            if (!tree_->restore(std::move(treeSnapshot))) {
                result.status = DslExecutionStatus::InvalidDefinition;
                result.errorMessage =
                    QStringLiteral("Unable to restore failed window entry transaction");
                return result;
            }
            result.status = execResult.status;
            result.errorMessage = execResult.errorMessage;
            return result;
        }

        if (!execResult.structureNode.has_value()) {
            (void)tree_->restore(std::move(treeSnapshot));
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage =
                QStringLiteral("Window entry completed without a structure node");
            return result;
        }
        state_->entryNodes.emplace(entryIndex, *execResult.structureNode);
        result.entryNodes.push_back(*execResult.structureNode);
        ++result.decodedEntryCount;
    }

    if (state_->entryNodes.size() == clampedCount) {
        (void)tree_->transition(windowNodeId_, core::MaterializationState::Materialized);
    }

    if (clampedCount < windowMeta.entryCount && pageEndIndex == clampedCount) {
        result.status = DslExecutionStatus::TruncatedSource;
        return result;
    }

    result.status = DslExecutionStatus::Materialized;
    return result;
}

} // namespace streamview::rules
