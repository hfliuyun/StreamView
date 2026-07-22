#include "analysis_tree_model.h"

#include <QColor>

namespace streamview::app {

AnalysisTreeModel::AnalysisTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void AnalysisTreeModel::resetFromTree(const core::AnalysisTree& tree) {
    beginResetModel();
    nodes_.clear();
    const auto root = tree.node(tree.rootId());
    if (root) {
        flatten(tree, tree.rootId(), -1, 0);
    }
    endResetModel();
}

void AnalysisTreeModel::clear() {
    beginResetModel();
    nodes_.clear();
    endResetModel();
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
    flat.diagnostics = node->diagnostics();
    flat.parentFlatIndex = parentFlatIndex;
    flat.rowInParent = rowInParent;
    nodes_.push_back(std::move(flat));

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
    if (!child.isValid()) {
        return {};
    }

    const int myFlatIndex = static_cast<int>(child.internalId());
    if (myFlatIndex <= 0 || myFlatIndex >= static_cast<int>(nodes_.size())) {
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

int AnalysisTreeModel::flatIndexAt(const QModelIndex& index) const {
    if (!index.isValid()) {
        return 0;
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
