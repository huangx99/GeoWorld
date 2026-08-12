#include "hfsm_graph_model.hpp"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/StyleCollection>

#include <algorithm>
#include <iterator>

namespace {

constexpr int state_width = 210;
constexpr int state_height = 90;
constexpr int default_transition_priority = 100;

} // namespace

HfsmGraphModel::HfsmGraphModel() = default;

void HfsmGraphModel::setDocument(geoworld::tooling::HfsmDocument document) {
    states_.clear();
    transitions_.clear();
    next_node_id_ = 1;
    initial_id_ = QtNodes::InvalidNodeId;
    machine_id_ = QString::fromStdString(document.machine_id);
    std::unordered_map<std::string, QtNodes::NodeId> ids;
    for (const auto& source : document.states) {
        const auto id = newNodeId();
        ids.emplace(source.id, id);
        states_.emplace(id, State{QString::fromStdString(source.id), QString::fromStdString(source.name),
            QString::fromStdString(source.parent), {source.position.x, source.position.y}});
        if (source.id == document.initial_state) initial_id_ = id;
    }
    std::unordered_map<QtNodes::NodeId, QtNodes::PortIndex> ports;
    for (const auto& source : document.transitions) {
        if (!ids.contains(source.source) || !ids.contains(source.target)) continue;
        const auto source_id = ids.at(source.source);
        transitions_.push_back({{source_id, ports[source_id]++, ids.at(source.target), 0},
                                QString::fromStdString(source.event), source.priority});
    }
    Q_EMIT modelReset();
}

geoworld::tooling::HfsmDocument HfsmGraphModel::document() const {
    geoworld::tooling::HfsmDocument result;
    result.machine_id = machine_id_.toStdString();
    std::vector<QtNodes::NodeId> ids;
    for (const auto& [id, state] : states_) {
        static_cast<void>(state);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (const auto id : ids) {
        const auto& state = states_.at(id);
        result.states.push_back({state.stable_id.toStdString(), state.name.toStdString(),
            state.parent.toStdString(), {state.position.x(), state.position.y()}});
    }
    auto ordered = transitions_;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return std::tie(left.connection.outNodeId, left.connection.outPortIndex)
            < std::tie(right.connection.outNodeId, right.connection.outPortIndex);
    });
    for (const auto& transition : ordered) {
        result.transitions.push_back({states_.at(transition.connection.outNodeId).stable_id.toStdString(),
            transition.event.toStdString(), states_.at(transition.connection.inNodeId).stable_id.toStdString(),
            transition.priority});
    }
    if (states_.contains(initial_id_)) result.initial_state = states_.at(initial_id_).stable_id.toStdString();
    return result;
}

const geoworld::tooling::HfsmStateDocument* HfsmGraphModel::stateDocument(QtNodes::NodeId node_id) const {
    static thread_local geoworld::tooling::HfsmStateDocument result;
    if (!states_.contains(node_id)) return nullptr;
    const auto& state = states_.at(node_id);
    result = {state.stable_id.toStdString(), state.name.toStdString(), state.parent.toStdString(),
              {state.position.x(), state.position.y()}};
    return &result;
}

std::vector<geoworld::tooling::HfsmTransitionDocument> HfsmGraphModel::transitionsFrom(
    QtNodes::NodeId node_id) const {
    std::vector<geoworld::tooling::HfsmTransitionDocument> result;
    if (!states_.contains(node_id)) return result;
    for (const auto& transition : transitions_) {
        if (transition.connection.outNodeId == node_id) {
            result.push_back({states_.at(node_id).stable_id.toStdString(), transition.event.toStdString(),
                states_.at(transition.connection.inNodeId).stable_id.toStdString(), transition.priority});
        }
    }
    return result;
}

bool HfsmGraphModel::isInitial(QtNodes::NodeId node_id) const noexcept { return initial_id_ == node_id; }

QString HfsmGraphModel::machineId() const { return machine_id_; }

void HfsmGraphModel::setMachineId(QString machine_id) {
    if (!machine_id.trimmed().isEmpty()) machine_id_ = std::move(machine_id);
}

bool HfsmGraphModel::updateState(QtNodes::NodeId node_id,
    const geoworld::tooling::HfsmStateDocument& state, bool is_initial,
    std::vector<geoworld::tooling::HfsmTransitionDocument> transitions) {
    if (!states_.contains(node_id) || state.id.empty() || state.name.empty()) return false;
    const auto duplicate = std::find_if(states_.begin(), states_.end(), [&](const auto& entry) {
        return entry.first != node_id && entry.second.stable_id.toStdString() == state.id;
    });
    if (duplicate != states_.end()) return false;
    for (const auto& transition : transitions) {
        const auto target = std::find_if(states_.begin(), states_.end(), [&](const auto& entry) {
            return entry.second.stable_id.toStdString() == transition.target
                || (entry.first == node_id && transition.target == state.id);
        });
        if (target == states_.end()) return false;
    }
    auto& target = states_.at(node_id);
    const auto old_id = target.stable_id.toStdString();
    target.stable_id = QString::fromStdString(state.id);
    target.name = QString::fromStdString(state.name);
    target.parent = QString::fromStdString(state.parent);
    for (auto& candidate : states_) {
        if (candidate.second.parent.toStdString() == old_id) candidate.second.parent = target.stable_id;
    }
    std::erase_if(transitions_, [node_id](const auto& item) {
        return item.connection.outNodeId == node_id;
    });
    for (const auto& source : transitions) {
        const auto target_id = std::find_if(states_.begin(), states_.end(), [&](const auto& entry) {
            return entry.second.stable_id.toStdString() == source.target
                || (entry.first == node_id && source.target == state.id);
        });
        transitions_.push_back({{node_id, nextOutputPort(node_id), target_id->first, 0},
            QString::fromStdString(source.event), source.priority});
    }
    if (is_initial) initial_id_ = node_id;
    Q_EMIT modelReset();
    return true;
}

QtNodes::NodeId HfsmGraphModel::newNodeId() { return next_node_id_++; }

std::unordered_set<QtNodes::NodeId> HfsmGraphModel::allNodeIds() const {
    std::unordered_set<QtNodes::NodeId> result;
    for (const auto& [id, state] : states_) {
        static_cast<void>(state);
        result.insert(id);
    }
    return result;
}

std::unordered_set<QtNodes::ConnectionId> HfsmGraphModel::allConnectionIds(QtNodes::NodeId node_id) const {
    std::unordered_set<QtNodes::ConnectionId> result;
    for (const auto& transition : transitions_) {
        if (transition.connection.outNodeId == node_id || transition.connection.inNodeId == node_id) {
            result.insert(transition.connection);
        }
    }
    return result;
}

std::unordered_set<QtNodes::ConnectionId> HfsmGraphModel::connections(QtNodes::NodeId node_id,
    QtNodes::PortType port_type, QtNodes::PortIndex port_index) const {
    std::unordered_set<QtNodes::ConnectionId> result;
    for (const auto& transition : transitions_) {
        if (QtNodes::getNodeId(port_type, transition.connection) == node_id
            && QtNodes::getPortIndex(port_type, transition.connection) == port_index) {
            result.insert(transition.connection);
        }
    }
    return result;
}

bool HfsmGraphModel::connectionExists(QtNodes::ConnectionId connection) const {
    return std::any_of(transitions_.begin(), transitions_.end(), [&](const auto& item) {
        return item.connection == connection;
    });
}

QtNodes::NodeId HfsmGraphModel::addNode(QString) {
    const auto id = newNodeId();
    const auto stable_id = QString("状态_%1").arg(id);
    states_.emplace(id, State{stable_id, stable_id, {}, {}});
    if (initial_id_ == QtNodes::InvalidNodeId) initial_id_ = id;
    Q_EMIT nodeCreated(id);
    return id;
}

bool HfsmGraphModel::connectionPossible(QtNodes::ConnectionId connection) const {
    return states_.contains(connection.outNodeId) && states_.contains(connection.inNodeId)
        && !connectionExists(connection);
}

void HfsmGraphModel::addConnection(QtNodes::ConnectionId connection) {
    connection.outPortIndex = nextOutputPort(connection.outNodeId);
    if (!connectionPossible(connection)) return;
    transitions_.push_back({connection, "事件", default_transition_priority});
    Q_EMIT connectionCreated(connection);
}

bool HfsmGraphModel::nodeExists(QtNodes::NodeId node_id) const { return states_.contains(node_id); }

QtNodes::PortIndex HfsmGraphModel::nextOutputPort(QtNodes::NodeId node_id) const {
    QtNodes::PortIndex result{};
    for (const auto& transition : transitions_) {
        if (transition.connection.outNodeId == node_id) {
            result = std::max(result, transition.connection.outPortIndex + 1);
        }
    }
    return result;
}

QVariant HfsmGraphModel::nodeData(QtNodes::NodeId node_id, QtNodes::NodeRole role) const {
    if (!states_.contains(node_id)) return {};
    const auto& state = states_.at(node_id);
    switch (role) {
    case QtNodes::NodeRole::Type: return QString{"HFSM 状态"};
    case QtNodes::NodeRole::Position: return state.position;
    case QtNodes::NodeRole::Size: return QSize{state_width, state_height};
    case QtNodes::NodeRole::CaptionVisible: return true;
    case QtNodes::NodeRole::Caption:
        return node_id == initial_id_ ? QString("★ %1\n%2").arg(state.name, state.stable_id)
                                      : QString("%1\n%2").arg(state.name, state.stable_id);
    case QtNodes::NodeRole::Style: return QtNodes::StyleCollection::nodeStyle().toJson().toVariantMap();
    case QtNodes::NodeRole::InPortCount: return 1U;
    case QtNodes::NodeRole::OutPortCount: return nextOutputPort(node_id) + 1U;
    case QtNodes::NodeRole::Widget: return QVariant{};
    default: return {};
    }
}

bool HfsmGraphModel::setNodeData(QtNodes::NodeId node_id, QtNodes::NodeRole role, QVariant value) {
    if (!states_.contains(node_id)) return false;
    if (role == QtNodes::NodeRole::Position) {
        states_.at(node_id).position = value.toPointF();
        Q_EMIT nodePositionUpdated(node_id);
        return true;
    }
    return false;
}

QVariant HfsmGraphModel::portData(QtNodes::NodeId node_id, QtNodes::PortType port_type,
    QtNodes::PortIndex port_index, QtNodes::PortRole role) const {
    switch (role) {
    case QtNodes::PortRole::DataType: return QString{"状态转换"};
    case QtNodes::PortRole::ConnectionPolicyRole:
        return QVariant::fromValue(port_type == QtNodes::PortType::In
            ? QtNodes::ConnectionPolicy::Many : QtNodes::ConnectionPolicy::One);
    case QtNodes::PortRole::CaptionVisible: return true;
    case QtNodes::PortRole::Caption:
        if (port_type == QtNodes::PortType::In) return QString{"进入"};
        for (const auto& transition : transitions_) {
            if (transition.connection.outNodeId == node_id
                && transition.connection.outPortIndex == port_index) {
                return QString("%1 / %2").arg(transition.event).arg(transition.priority);
            }
        }
        return QString{"新转换"};
    default: return {};
    }
}

bool HfsmGraphModel::setPortData(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex,
    const QVariant&, QtNodes::PortRole) { return false; }

bool HfsmGraphModel::deleteConnection(QtNodes::ConnectionId connection) {
    const auto before = transitions_.size();
    std::erase_if(transitions_, [&](const auto& item) { return item.connection == connection; });
    if (before == transitions_.size()) return false;
    Q_EMIT connectionDeleted(connection);
    Q_EMIT modelReset();
    return true;
}

bool HfsmGraphModel::deleteNode(QtNodes::NodeId node_id) {
    if (!states_.contains(node_id)) return false;
    for (const auto& connection : allConnectionIds(node_id)) deleteConnection(connection);
    const auto deleted_id = states_.at(node_id).stable_id;
    states_.erase(node_id);
    for (auto& [id, state] : states_) {
        static_cast<void>(id);
        if (state.parent == deleted_id) state.parent.clear();
    }
    if (initial_id_ == node_id) initial_id_ = states_.empty() ? QtNodes::InvalidNodeId : states_.begin()->first;
    Q_EMIT nodeDeleted(node_id);
    return true;
}
