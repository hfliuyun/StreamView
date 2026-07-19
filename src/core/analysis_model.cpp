#include <streamview/core/analysis_model.h>

#include <utility>

namespace streamview::core {

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
    if (terminalState != MaterializationState::Cancelled &&
        terminalState != MaterializationState::Unsupported &&
        terminalState != MaterializationState::Invalid) {
        return false;
    }
    if (!transition(id, terminalState)) {
        return false;
    }
    return addDiagnostic(id, std::move(diagnostic));
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
