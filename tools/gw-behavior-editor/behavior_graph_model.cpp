#include "behavior_graph_model.hpp"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/StyleCollection>

#include <algorithm>
#include <iterator>

namespace {

geoworld::tooling::BehaviorNodeArity arity(const QString& type) {
    const auto registry = geoworld::tooling::BehaviorNodeRegistry::defaults();
    const auto* descriptor = registry.find(type.toStdString());
    return descriptor == nullptr ? geoworld::tooling::BehaviorNodeArity::leaf : descriptor->arity;
}

constexpr unsigned int default_control_ports = 4;
constexpr int node_width = 190;
constexpr int node_height = 80;

} // namespace

BehaviorGraphModel::BehaviorGraphModel() = default;

void BehaviorGraphModel::resetModel() {
    nodes_.clear();
    connections_.clear();
    next_node_id_ = 1;
    root_id_ = QtNodes::InvalidNodeId;
    Q_EMIT modelReset();
}

void BehaviorGraphModel::setDocument(geoworld::tooling::BehaviorTreeDocument document) {
    resetModel();
    tree_id_ = QString::fromStdString(document.tree_id);
    std::unordered_map<std::string, QtNodes::NodeId> ids;
    for (const auto& source : document.nodes) {
        const auto id = newNodeId();
        ids.emplace(source.id, id);
        nodes_.emplace(id, Node{QString::fromStdString(source.id), QString::fromStdString(source.type),
            QString::fromStdString(source.name), {source.position.x, source.position.y}, source.parameters});
        if (source.id == document.root) root_id_ = id;
    }
    for (const auto& source : document.nodes) {
        for (std::size_t index = 0; index < source.children.size(); ++index) {
            if (ids.contains(source.children[index])) {
                connections_.insert({ids.at(source.id), static_cast<QtNodes::PortIndex>(index),
                                     ids.at(source.children[index]), 0});
            }
        }
    }
    Q_EMIT modelReset();
}

geoworld::tooling::BehaviorTreeDocument BehaviorGraphModel::document() const {
    geoworld::tooling::BehaviorTreeDocument result;
    result.tree_id = tree_id_.toStdString();
    std::vector<QtNodes::NodeId> ids;
    ids.reserve(nodes_.size());
    for (const auto& [id, node] : nodes_) {
        static_cast<void>(node);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (const auto id : ids) {
        const auto& source = nodes_.at(id);
        geoworld::tooling::BehaviorNodeDocument node;
        node.id = source.stable_id.toStdString();
        node.type = source.type.toStdString();
        node.name = source.name.toStdString();
        node.position = {source.position.x(), source.position.y()};
        node.parameters = source.parameters;
        std::vector<QtNodes::ConnectionId> outgoing;
        std::copy_if(connections_.begin(), connections_.end(), std::back_inserter(outgoing),
                     [id](const auto& connection) { return connection.outNodeId == id; });
        std::sort(outgoing.begin(), outgoing.end(), [](const auto& left, const auto& right) {
            return left.outPortIndex < right.outPortIndex;
        });
        for (const auto& connection : outgoing) {
            node.children.push_back(nodes_.at(connection.inNodeId).stable_id.toStdString());
        }
        result.nodes.push_back(std::move(node));
    }
    if (nodes_.contains(root_id_)) result.root = nodes_.at(root_id_).stable_id.toStdString();
    return result;
}

const geoworld::tooling::BehaviorNodeDocument* BehaviorGraphModel::nodeDocument(
    QtNodes::NodeId node_id) const {
    static thread_local geoworld::tooling::BehaviorNodeDocument result;
    if (!nodes_.contains(node_id)) return nullptr;
    const auto& source = nodes_.at(node_id);
    result = {source.stable_id.toStdString(), source.type.toStdString(), source.name.toStdString(),
              {source.position.x(), source.position.y()}, {}, source.parameters};
    return &result;
}

bool BehaviorGraphModel::isRoot(QtNodes::NodeId node_id) const noexcept { return root_id_ == node_id; }

QString BehaviorGraphModel::treeId() const { return tree_id_; }

void BehaviorGraphModel::setTreeId(QString tree_id) {
    if (!tree_id.trimmed().isEmpty()) tree_id_ = std::move(tree_id);
}

bool BehaviorGraphModel::updateNode(QtNodes::NodeId node_id,
    const geoworld::tooling::BehaviorNodeDocument& document, bool is_root) {
    if (!nodes_.contains(node_id) || document.id.empty() || document.name.empty()) return false;
    const auto duplicate = std::find_if(nodes_.begin(), nodes_.end(), [&](const auto& entry) {
        return entry.first != node_id && entry.second.stable_id.toStdString() == document.id;
    });
    if (duplicate != nodes_.end()) return false;
    auto& target = nodes_.at(node_id);
    target.stable_id = QString::fromStdString(document.id);
    target.type = QString::fromStdString(document.type);
    target.name = QString::fromStdString(document.name);
    target.parameters = document.parameters;
    if (is_root) root_id_ = node_id;
    Q_EMIT nodeUpdated(node_id);
    Q_EMIT modelReset();
    return true;
}

QtNodes::NodeId BehaviorGraphModel::newNodeId() { return next_node_id_++; }

std::unordered_set<QtNodes::NodeId> BehaviorGraphModel::allNodeIds() const {
    std::unordered_set<QtNodes::NodeId> ids;
    for (const auto& [id, node] : nodes_) {
        static_cast<void>(node);
        ids.insert(id);
    }
    return ids;
}

std::unordered_set<QtNodes::ConnectionId> BehaviorGraphModel::allConnectionIds(
    QtNodes::NodeId node_id) const {
    std::unordered_set<QtNodes::ConnectionId> result;
    std::copy_if(connections_.begin(), connections_.end(), std::inserter(result, result.end()),
                 [node_id](const auto& connection) {
                     return connection.inNodeId == node_id || connection.outNodeId == node_id;
                 });
    return result;
}

std::unordered_set<QtNodes::ConnectionId> BehaviorGraphModel::connections(
    QtNodes::NodeId node_id, QtNodes::PortType port_type, QtNodes::PortIndex port_index) const {
    std::unordered_set<QtNodes::ConnectionId> result;
    std::copy_if(connections_.begin(), connections_.end(), std::inserter(result, result.end()),
                 [=](const auto& connection) {
                     return QtNodes::getNodeId(port_type, connection) == node_id
                         && QtNodes::getPortIndex(port_type, connection) == port_index;
                 });
    return result;
}

bool BehaviorGraphModel::connectionExists(QtNodes::ConnectionId connection) const {
    return connections_.contains(connection);
}

QtNodes::NodeId BehaviorGraphModel::addNode(QString node_type) {
    if (node_type.isEmpty()) node_type = "AlwaysSuccess";
    const auto id = newNodeId();
    const auto stable_id = QString("节点_%1").arg(id);
    nodes_.emplace(id, Node{stable_id, node_type, stable_id, {}, {}});
    if (root_id_ == QtNodes::InvalidNodeId) root_id_ = id;
    Q_EMIT nodeCreated(id);
    return id;
}

bool BehaviorGraphModel::connectionPossible(QtNodes::ConnectionId connection) const {
    if (!nodeExists(connection.outNodeId) || !nodeExists(connection.inNodeId)
        || connection.outNodeId == connection.inNodeId || connectionExists(connection)) return false;
    return connections(connection.inNodeId, QtNodes::PortType::In, 0).empty();
}

void BehaviorGraphModel::addConnection(QtNodes::ConnectionId connection) {
    if (!connectionPossible(connection)) return;
    connections_.insert(connection);
    Q_EMIT connectionCreated(connection);
}

bool BehaviorGraphModel::nodeExists(QtNodes::NodeId node_id) const { return nodes_.contains(node_id); }

unsigned int BehaviorGraphModel::outputCount(QtNodes::NodeId node_id) const {
    const auto node_arity = arity(nodes_.at(node_id).type);
    if (node_arity == geoworld::tooling::BehaviorNodeArity::leaf) return 0;
    if (node_arity == geoworld::tooling::BehaviorNodeArity::decorator) return 1;
    unsigned int maximum = 0;
    for (const auto& connection : connections_) {
        if (connection.outNodeId == node_id) maximum = std::max(maximum, connection.outPortIndex + 1);
    }
    return std::max(default_control_ports, maximum + 1U);
}

QVariant BehaviorGraphModel::nodeData(QtNodes::NodeId node_id, QtNodes::NodeRole role) const {
    if (!nodes_.contains(node_id)) return {};
    const auto& node = nodes_.at(node_id);
    switch (role) {
    case QtNodes::NodeRole::Type: return node.type;
    case QtNodes::NodeRole::Position: return node.position;
    case QtNodes::NodeRole::Size: return QSize{node_width, node_height};
    case QtNodes::NodeRole::CaptionVisible: return true;
    case QtNodes::NodeRole::Caption:
        return node_id == root_id_ ? QString("★ %1\n%2").arg(node.name, node.type)
                                   : QString("%1\n%2").arg(node.name, node.type);
    case QtNodes::NodeRole::Style: return QtNodes::StyleCollection::nodeStyle().toJson().toVariantMap();
    case QtNodes::NodeRole::InPortCount: return node_id == root_id_ ? 0U : 1U;
    case QtNodes::NodeRole::OutPortCount: return outputCount(node_id);
    case QtNodes::NodeRole::Widget: return QVariant{};
    default: return {};
    }
}

bool BehaviorGraphModel::setNodeData(QtNodes::NodeId node_id, QtNodes::NodeRole role, QVariant value) {
    if (!nodes_.contains(node_id)) return false;
    if (role == QtNodes::NodeRole::Position) {
        nodes_.at(node_id).position = value.toPointF();
        Q_EMIT nodePositionUpdated(node_id);
        return true;
    }
    if (role == QtNodes::NodeRole::Caption) {
        nodes_.at(node_id).name = value.toString();
        Q_EMIT nodeUpdated(node_id);
        return true;
    }
    return false;
}

QVariant BehaviorGraphModel::portData(QtNodes::NodeId, QtNodes::PortType port_type,
    QtNodes::PortIndex port_index, QtNodes::PortRole role) const {
    switch (role) {
    case QtNodes::PortRole::DataType: return QString{"行为流"};
    case QtNodes::PortRole::ConnectionPolicyRole:
        return QVariant::fromValue(port_type == QtNodes::PortType::In
            ? QtNodes::ConnectionPolicy::One : QtNodes::ConnectionPolicy::Many);
    case QtNodes::PortRole::CaptionVisible: return true;
    case QtNodes::PortRole::Caption:
        return port_type == QtNodes::PortType::In ? QString{"父节点"} : QString("子节点 %1").arg(port_index + 1);
    default: return {};
    }
}

bool BehaviorGraphModel::setPortData(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex,
                                     const QVariant&, QtNodes::PortRole) { return false; }

bool BehaviorGraphModel::deleteConnection(QtNodes::ConnectionId connection) {
    if (!connections_.erase(connection)) return false;
    Q_EMIT connectionDeleted(connection);
    return true;
}

bool BehaviorGraphModel::deleteNode(QtNodes::NodeId node_id) {
    if (!nodes_.contains(node_id)) return false;
    for (const auto& connection : allConnectionIds(node_id)) deleteConnection(connection);
    nodes_.erase(node_id);
    if (root_id_ == node_id) root_id_ = nodes_.empty() ? QtNodes::InvalidNodeId : nodes_.begin()->first;
    Q_EMIT nodeDeleted(node_id);
    return true;
}
