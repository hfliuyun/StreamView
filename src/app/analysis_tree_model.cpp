#include "analysis_tree_model.h"

#include <QColor>

#include <limits>

namespace {

bool sameLocation(const std::optional<streamview::core::FieldLocation>& left,
                  const std::optional<streamview::core::FieldLocation>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left) {
        return true;
    }
    const auto& leftRange = left->logicalRange();
    const auto& rightRange = right->logicalRange();
    if (leftRange.start().viewId() != rightRange.start().viewId() ||
        leftRange.start().bitOffset() != rightRange.start().bitOffset() ||
        leftRange.bitLength() != rightRange.bitLength() ||
        left->sourceSpans().size() != right->sourceSpans().size()) {
        return false;
    }
    for (std::size_t index = 0; index < left->sourceSpans().size(); ++index) {
        const auto& leftSpan = left->sourceSpans().at(index);
        const auto& rightSpan = right->sourceSpans().at(index);
        if (leftSpan.start() != rightSpan.start() ||
            leftSpan.bitLength() != rightSpan.bitLength()) {
            return false;
        }
    }
    return true;
}

bool sameMetadata(const streamview::core::AnalysisNodeMetadata& left,
                  const streamview::core::AnalysisNodeMetadata& right) {
    if (left.typeName != right.typeName || left.description != right.description ||
        left.specification.has_value() != right.specification.has_value()) {
        return false;
    }
    return !left.specification ||
           (left.specification->standard == right.specification->standard &&
            left.specification->clause == right.specification->clause);
}

bool sameDiagnostic(const streamview::core::ParseDiagnostic& left,
                    const streamview::core::ParseDiagnostic& right) {
    return left.code == right.code && left.severity == right.severity &&
           left.message == right.message && left.fieldPath == right.fieldPath &&
           sameLocation(left.location, right.location);
}

bool sameDiagnostics(const std::vector<streamview::core::ParseDiagnostic>& left,
                     const std::vector<streamview::core::ParseDiagnostic>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!sameDiagnostic(left.at(index), right.at(index))) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace streamview::app {

AnalysisTreeModel::AnalysisTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void AnalysisTreeModel::resetFromTree(const core::AnalysisTree& tree) {
    beginResetModel();
    nodes_.clear();
    flatIndexByNodeId_.clear();
    const auto root = tree.node(tree.rootId());
    if (root) {
        flatten(tree, tree.rootId(), -1, 0);
    }
    endResetModel();
}

void AnalysisTreeModel::clear() {
    beginResetModel();
    nodes_.clear();
    flatIndexByNodeId_.clear();
    endResetModel();
}

bool AnalysisTreeModel::canAppendSubtree(
    const core::AnalysisTree& tree, core::AnalysisNodeId id,
    std::unordered_map<quint64, bool>& pending) const {
    if (id.value() == 0 || flatIndexByNodeId_.contains(id.value())) {
        return false;
    }
    const auto node = tree.node(id);
    if (!node || pending.contains(id.value())) {
        return false;
    }
    pending.emplace(id.value(), true);
    for (const auto childId : node->children()) {
        if (!canAppendSubtree(tree, childId, pending)) {
            return false;
        }
    }
    return true;
}

bool AnalysisTreeModel::appendTopLevelNodes(
    const core::AnalysisTree& tree, const std::vector<core::AnalysisNodeId>& nodeIds) {
    if (nodeIds.empty()) {
        return true;
    }
    if (nodes_.empty() || nodes_.front().id != tree.rootId()) {
        return false;
    }

    const auto root = tree.node(tree.rootId());
    if (!root || nodeIds.size() >
                     static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        root->children().size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) - nodeIds.size()) {
        return false;
    }

    std::unordered_map<quint64, bool> pending;
    pending.reserve(nodeIds.size());
    for (const auto id : nodeIds) {
        const auto node = tree.node(id);
        if (!node || node->parentId() != tree.rootId() || !canAppendSubtree(tree, id, pending)) {
            return false;
        }
    }

    if (nodes_.front().childFlatIndices.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int firstRow = static_cast<int>(nodes_.front().childFlatIndices.size());
    const int lastRow = firstRow + static_cast<int>(nodeIds.size()) - 1;
    beginInsertRows({}, firstRow, lastRow);
    for (std::size_t offset = 0; offset < nodeIds.size(); ++offset) {
        const int flatIndex = flatten(tree, nodeIds[offset], 0,
                                      firstRow + static_cast<int>(offset));
        if (flatIndex >= 0) {
            nodes_.front().childFlatIndices.push_back(flatIndex);
        }
    }
    endInsertRows();
    return true;
}

void AnalysisTreeModel::updateFromTree(const core::AnalysisTree& tree) {
    for (std::size_t flatIndex = 0; flatIndex < nodes_.size(); ++flatIndex) {
        FlatNode& flat = nodes_[flatIndex];
        const auto node = tree.node(flat.id);
        if (!node) {
            continue;
        }
        const bool changed = flat.kind != node->kind() || flat.name != node->name() ||
                             flat.state != node->state() || flat.value != node->value() ||
                             !sameLocation(flat.location, node->location()) ||
                             !sameMetadata(flat.metadata, node->metadata()) ||
                             !sameDiagnostics(flat.diagnostics, node->diagnostics());
        flat.kind = node->kind();
        flat.name = node->name();
        flat.state = node->state();
        flat.value = node->value();
        flat.location = node->location();
        flat.metadata = node->metadata();
        flat.diagnostics = node->diagnostics();
        if (changed && flatIndex != 0) {
            const QModelIndex changedIndex = indexForNodeId(flat.id);
            if (changedIndex.isValid()) {
                emit dataChanged(changedIndex,
                                 changedIndex.siblingAtColumn(ColumnCount - 1));
            }
        }
    }
}

int AnalysisTreeModel::flatten(const core::AnalysisTree& tree, core::AnalysisNodeId id,
                               int parentFlatIndex, int rowInParent) {
    const auto node = tree.node(id);
    if (!node) {
        return -1;
    }

    const int myIndex = static_cast<int>(nodes_.size());

    FlatNode flat;
    flat.id = node->id();
    flat.kind = node->kind();
    flat.name = node->name();
    flat.state = node->state();
    flat.value = node->value();
    flat.location = node->location();
    flat.metadata = node->metadata();
    flat.diagnostics = node->diagnostics();
    flat.parentFlatIndex = parentFlatIndex;
    flat.rowInParent = rowInParent;
    nodes_.push_back(std::move(flat));
    flatIndexByNodeId_[node->id().value()] = myIndex;

    int row = 0;
    for (const auto childId : node->children()) {
        const int childFlatIndex = flatten(tree, childId, myIndex, row);
        if (childFlatIndex >= 0) {
            nodes_[static_cast<std::size_t>(myIndex)].childFlatIndices.push_back(childFlatIndex);
        }
        ++row;
    }

    return myIndex;
}

QModelIndex AnalysisTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (column < 0 || column >= ColumnCount || row < 0 || nodes_.empty()) {
        return {};
    }

    // The invisible root is nodes_[0]; its children are the top-level items.
    int parentFlatIndex = 0;
    if (parent.isValid()) {
        parentFlatIndex = flatIndexAt(parent);
        if (parentFlatIndex < 0) {
            return {};
        }
    }

    const FlatNode& parentNode = nodes_[static_cast<std::size_t>(parentFlatIndex)];
    if (row >= static_cast<int>(parentNode.childFlatIndices.size())) {
        return {};
    }

    const int childFlatIndex = parentNode.childFlatIndices[static_cast<std::size_t>(row)];
    return createIndex(row, column, static_cast<quintptr>(childFlatIndex));
}

QModelIndex AnalysisTreeModel::parent(const QModelIndex& child) const {
    const int myFlatIndex = flatIndexAt(child);
    if (myFlatIndex <= 0) {
        return {};
    }

    const FlatNode& myNode = nodes_[static_cast<std::size_t>(myFlatIndex)];
    if (myNode.parentFlatIndex <= 0) {
        // Parent is the invisible root — return invalid (top-level item).
        return {};
    }

    const FlatNode& parentNode = nodes_[static_cast<std::size_t>(myNode.parentFlatIndex)];
    return createIndex(parentNode.rowInParent, 0,
                       static_cast<quintptr>(myNode.parentFlatIndex));
}

int AnalysisTreeModel::rowCount(const QModelIndex& parent) const {
    if (nodes_.empty()) {
        return 0;
    }

    int flatIndex = 0; // invisible root
    if (parent.isValid()) {
        if (parent.column() != 0) {
            return 0;
        }
        flatIndex = flatIndexAt(parent);
        if (flatIndex < 0) {
            return 0;
        }
    }

    return static_cast<int>(nodes_[static_cast<std::size_t>(flatIndex)].childFlatIndices.size());
}

int AnalysisTreeModel::columnCount(const QModelIndex& /*parent*/) const {
    return ColumnCount;
}

QVariant AnalysisTreeModel::data(const QModelIndex& index, int role) const {
    const FlatNode* node = flatNodeAt(index);
    if (!node) {
        return {};
    }

    if (role == Qt::DisplayRole) {
        switch (static_cast<Column>(index.column())) {
        case Name:
            return node->name;
        case Value:
            return node->value.isValid() ? node->value.toString() : QString();
        case Bits:
            return formatBits(node->location);
        case State:
            return stateName(node->state);
        default:
            return {};
        }
    }

    if (role == Qt::ForegroundRole) {
        if (node->state == core::MaterializationState::Invalid) {
            return QColor(Qt::red);
        }
        if (node->state == core::MaterializationState::Cancelled) {
            return QColor(Qt::darkYellow);
        }
    }

    if (role == Qt::ToolTipRole && !node->diagnostics.empty()) {
        QStringList tips;
        for (const auto& diagnostic : node->diagnostics) {
            tips.append(diagnostic.message);
        }
        return tips.join(QStringLiteral("\n"));
    }

    return {};
}

QVariant AnalysisTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (static_cast<Column>(section)) {
    case Name:
        return tr("Name");
    case Value:
        return tr("Value");
    case Bits:
        return tr("Source Bits");
    case State:
        return tr("State");
    default:
        return {};
    }
}

std::optional<core::AnalysisNodeId> AnalysisTreeModel::nodeIdAt(const QModelIndex& index) const {
    const FlatNode* node = flatNodeAt(index);
    if (!node) {
        return std::nullopt;
    }
    return node->id;
}

QModelIndex AnalysisTreeModel::indexForNodeId(core::AnalysisNodeId id, int column) const {
    if (column < 0 || column >= ColumnCount) {
        return {};
    }
    const auto found = flatIndexByNodeId_.find(id.value());
    if (found != flatIndexByNodeId_.end() && found->second > 0) {
        const FlatNode& node = nodes_[static_cast<std::size_t>(found->second)];
        return createIndex(node.rowInParent, column, static_cast<quintptr>(found->second));
    }
    return {};
}

int AnalysisTreeModel::flatIndexAt(const QModelIndex& index) const {
    if (!index.isValid() || index.model() != this) {
        return -1;
    }
    const int flatIndex = static_cast<int>(index.internalId());
    if (flatIndex < 0 || flatIndex >= static_cast<int>(nodes_.size())) {
        return -1;
    }
    return flatIndex;
}

const AnalysisTreeModel::FlatNode* AnalysisTreeModel::flatNodeAt(const QModelIndex& index) const {
    const int flatIndex = flatIndexAt(index);
    if (flatIndex < 0) {
        return nullptr;
    }
    return &nodes_[static_cast<std::size_t>(flatIndex)];
}

QString AnalysisTreeModel::formatBits(const std::optional<core::FieldLocation>& location) {
    if (!location || location->sourceSpans().empty()) {
        return {};
    }

    QStringList parts;
    for (const auto& span : location->sourceSpans()) {
        parts.append(
            QStringLiteral("[%1, %2)")
                .arg(span.start().absoluteBitOffset())
                .arg(span.endExclusive().absoluteBitOffset()));
    }
    return parts.join(QStringLiteral(", "));
}

QString AnalysisTreeModel::stateName(core::MaterializationState state) {
    switch (state) {
    case core::MaterializationState::Lazy:
        return QStringLiteral("lazy");
    case core::MaterializationState::Indexing:
        return QStringLiteral("indexing");
    case core::MaterializationState::WaitingDependency:
        return QStringLiteral("waiting");
    case core::MaterializationState::Cancelled:
        return QStringLiteral("cancelled");
    case core::MaterializationState::Unsupported:
        return QStringLiteral("unsupported");
    case core::MaterializationState::Invalid:
        return QStringLiteral("invalid");
    case core::MaterializationState::Materialized:
        return {};
    }
    return {};
}

} // namespace streamview::app
