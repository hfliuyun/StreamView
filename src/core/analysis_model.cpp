#include <streamview/core/analysis_model.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace streamview::core {
namespace {

std::atomic<quint64> nextAnalysisTreeIdentity{1};

} // namespace

quint64 AnalysisTree::nextInstanceIdentity() noexcept {
    for (;;) {
        const quint64 identity =
            nextAnalysisTreeIdentity.fetch_add(1, std::memory_order_relaxed);
        if (identity != 0) {
            return identity;
        }
    }
}

AnalysisTree::AnalysisTree(const AnalysisTree& other)
    : instanceIdentity_(nextInstanceIdentity()), nodes_(other.nodes_) {}

AnalysisTree::AnalysisTree(AnalysisTree&& other) noexcept
    : instanceIdentity_(other.instanceIdentity_), nodes_(std::move(other.nodes_)) {
    other.instanceIdentity_ = nextInstanceIdentity();
}

AnalysisTree& AnalysisTree::operator=(const AnalysisTree& other) {
    if (this != &other) {
        std::vector<AnalysisNode> nodes(other.nodes_);
        nodes_ = std::move(nodes);
        instanceIdentity_ = nextInstanceIdentity();
    }
    return *this;
}

AnalysisTree& AnalysisTree::operator=(AnalysisTree&& other) noexcept {
    if (this != &other) {
        nodes_ = std::move(other.nodes_);
        instanceIdentity_ = other.instanceIdentity_;
        other.instanceIdentity_ = nextInstanceIdentity();
    }
    return *this;
}

std::optional<AnalysisTree> AnalysisTree::create(const QString& rootName) {
    if (rootName.isEmpty()) {
        return std::nullopt;
    }

    AnalysisNodeSpec rootSpec;
    rootSpec.kind = AnalysisNodeKind::Root;
    rootSpec.name = rootName;
    rootSpec.state = MaterializationState::Indexing;
    return AnalysisTree(AnalysisNode(AnalysisNodeId(1), std::nullopt, std::move(rootSpec)));
}

std::optional<AnalysisNode> AnalysisTree::node(AnalysisNodeId id) const {
    const AnalysisNode* result = nodeForRead(id);
    if (result == nullptr) {
        return std::nullopt;
    }
    return *result;
}

std::optional<AnalysisNodeId>
AnalysisTree::mostSpecificMaterializedNodeAt(SourceBitAddress sourceBit) const noexcept {
    std::optional<AnalysisNodeId> bestId;
    std::size_t bestDepth = 0;
    quint64 bestCoverage = std::numeric_limits<quint64>::max();

    for (const AnalysisNode& candidate : nodes_) {
        if (candidate.state_ != MaterializationState::Materialized || !candidate.location_) {
            continue;
        }

        bool containsSourceBit = false;
        quint64 coverage = 0;
        for (const SourceSpan& span : candidate.location_->sourceSpans()) {
            if (span.start() <= sourceBit && sourceBit < span.endExclusive()) {
                containsSourceBit = true;
            }
            if (coverage > std::numeric_limits<quint64>::max() - span.bitLength()) {
                coverage = std::numeric_limits<quint64>::max();
            } else {
                coverage += span.bitLength();
            }
        }
        if (!containsSourceBit) {
            continue;
        }

        std::size_t depth = 0;
        auto parentId = candidate.parentId_;
        while (parentId) {
            ++depth;
            const AnalysisNode* parent = nodeForRead(*parentId);
            parentId = parent != nullptr ? parent->parentId_ : std::nullopt;
        }

        if (!bestId || depth > bestDepth ||
            (depth == bestDepth && coverage < bestCoverage) ||
            (depth == bestDepth && coverage == bestCoverage &&
             candidate.id_.value() < bestId->value())) {
            bestId = candidate.id_;
            bestDepth = depth;
            bestCoverage = coverage;
        }
    }
    return bestId;
}

std::optional<AnalysisNodeId>
AnalysisTree::appendChild(AnalysisNodeId parentId, AnalysisNodeSpec spec) {
    AnalysisNode* parent = nodeForMutation(parentId);
    if (parent == nullptr || parent->state_ != MaterializationState::Indexing ||
        !isValidSpec(spec) || spec.kind == AnalysisNodeKind::Root) {
        return std::nullopt;
    }

    const auto nextId = static_cast<quint64>(nodes_.size()) + 1;
    if (nextId == 0) {
        return std::nullopt;
    }

    const AnalysisNodeId childId(nextId);
    AnalysisNode child(childId, parentId, std::move(spec));
    nodes_.push_back(std::move(child));
    nodeForMutation(parentId)->children_.push_back(childId);
    return childId;
}

bool AnalysisTree::transition(AnalysisNodeId id, MaterializationState nextState) noexcept {
    AnalysisNode* node = nodeForMutation(id);
    if (node == nullptr || !canTransition(node->state_, nextState)) {
        return false;
    }
    node->state_ = nextState;
    return true;
}

bool AnalysisTree::addDiagnostic(AnalysisNodeId id, ParseDiagnostic diagnostic) {
    AnalysisNode* node = nodeForMutation(id);
    if (node == nullptr) {
        return false;
    }
    node->diagnostics_.push_back(std::move(diagnostic));
    return true;
}

bool AnalysisTree::markPartial(AnalysisNodeId id,
                               MaterializationState terminalState,
                               ParseDiagnostic diagnostic) {
    if (terminalState != MaterializationState::WaitingDependency &&
        terminalState != MaterializationState::Cancelled &&
        terminalState != MaterializationState::Unsupported &&
        terminalState != MaterializationState::Invalid) {
        return false;
    }
    if (!transition(id, terminalState)) {
        return false;
    }
    return addDiagnostic(id, std::move(diagnostic));
}

bool AnalysisTree::resumeCancelled(AnalysisNodeId id) noexcept {
    AnalysisNode* node = nodeForMutation(id);
    if (node == nullptr || node->state_ != MaterializationState::Cancelled) {
        return false;
    }

    std::erase_if(node->diagnostics_, [](const ParseDiagnostic& diagnostic) {
        return diagnostic.code == DiagnosticCode::Cancelled;
    });
    node->state_ = MaterializationState::Indexing;
    return true;
}

AnalysisTreeSnapshot AnalysisTree::snapshot(std::optional<AnalysisNodeId> preservedNode) const {
    const AnalysisNodeId nodeToPreserve = preservedNode.value_or(rootId());
    std::vector<AnalysisNode> preservedNodes;
    if (const AnalysisNode* node = nodeForRead(nodeToPreserve); node != nullptr) {
        preservedNodes.push_back(*node);
    }
    return AnalysisTreeSnapshot(instanceIdentity_, nodes_.size(), std::move(preservedNodes));
}

bool AnalysisTree::restore(AnalysisTreeSnapshot snapshot) noexcept {
    if (snapshot.treeIdentity_ != instanceIdentity_ || snapshot.nodeCount_ == 0 ||
        snapshot.nodeCount_ > nodes_.size() || snapshot.preservedNodes_.empty()) {
        return false;
    }
    for (const auto& preservedNode : snapshot.preservedNodes_) {
        if (preservedNode.id_.value() == 0 ||
            preservedNode.id_.value() > snapshot.nodeCount_) {
            return false;
        }
    }
    nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(snapshot.nodeCount_),
                 nodes_.end());
    for (auto& preservedNode : snapshot.preservedNodes_) {
        nodes_.at(static_cast<std::size_t>(preservedNode.id_.value() - 1)) =
            std::move(preservedNode);
    }
    return true;
}

bool AnalysisTree::hasPartialResults() const noexcept {
    for (const AnalysisNode& node : nodes_) {
        if (node.state_ == MaterializationState::Cancelled ||
            node.state_ == MaterializationState::Unsupported ||
            node.state_ == MaterializationState::Invalid) {
            return true;
        }
    }
    return false;
}

bool AnalysisTree::isFullyMaterialized() const noexcept {
    for (const AnalysisNode& node : nodes_) {
        if (node.state_ != MaterializationState::Materialized) {
            return false;
        }
    }
    return true;
}

AnalysisNode* AnalysisTree::nodeForMutation(AnalysisNodeId id) noexcept {
    if (id.value() == 0 || id.value() > nodes_.size()) {
        return nullptr;
    }
    return &nodes_.at(static_cast<std::size_t>(id.value() - 1));
}

const AnalysisNode* AnalysisTree::nodeForRead(AnalysisNodeId id) const noexcept {
    if (id.value() == 0 || id.value() > nodes_.size()) {
        return nullptr;
    }
    return &nodes_.at(static_cast<std::size_t>(id.value() - 1));
}

bool AnalysisTree::canTransition(MaterializationState from,
                                 MaterializationState to) noexcept {
    if (from == to) {
        return true;
    }

    switch (from) {
    case MaterializationState::Lazy:
        return to == MaterializationState::Indexing ||
               to == MaterializationState::WaitingDependency ||
               to == MaterializationState::Cancelled ||
               to == MaterializationState::Unsupported ||
               to == MaterializationState::Invalid ||
               to == MaterializationState::Materialized;
    case MaterializationState::Indexing:
        return to == MaterializationState::WaitingDependency ||
               to == MaterializationState::Cancelled ||
               to == MaterializationState::Unsupported ||
               to == MaterializationState::Invalid ||
               to == MaterializationState::Materialized;
    case MaterializationState::WaitingDependency:
        return to == MaterializationState::Indexing ||
               to == MaterializationState::Cancelled ||
               to == MaterializationState::Unsupported ||
               to == MaterializationState::Invalid ||
               to == MaterializationState::Materialized;
    case MaterializationState::Cancelled:
        return to == MaterializationState::Indexing;
    case MaterializationState::Unsupported:
    case MaterializationState::Invalid:
    case MaterializationState::Materialized:
        return false;
    }
    return false;
}

bool AnalysisTree::isValidSpec(const AnalysisNodeSpec& spec) noexcept {
    if (spec.name.isEmpty()) {
        return false;
    }
    if (spec.kind == AnalysisNodeKind::SyntaxField && !spec.location.has_value()) {
        return false;
    }
    if (spec.kind == AnalysisNodeKind::ComputedField && spec.location.has_value()) {
        return false;
    }
    return true;
}

} // namespace streamview::core
