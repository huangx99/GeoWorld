#pragma once

#include "geoworld/tooling/behavior_document.hpp"

#include <QtNodes/AbstractGraphModel>
#include <QPointF>

#include <unordered_map>
#include <unordered_set>

class HfsmGraphModel final : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    HfsmGraphModel();

    void setDocument(geoworld::tooling::HfsmDocument document);
    [[nodiscard]] geoworld::tooling::HfsmDocument document() const;
    [[nodiscard]] const geoworld::tooling::HfsmStateDocument* stateDocument(
        QtNodes::NodeId node_id) const;
    [[nodiscard]] std::vector<geoworld::tooling::HfsmTransitionDocument> transitionsFrom(
        QtNodes::NodeId node_id) const;
    [[nodiscard]] bool isInitial(QtNodes::NodeId node_id) const noexcept;
    [[nodiscard]] QString machineId() const;
    void setMachineId(QString machine_id);
    bool updateState(QtNodes::NodeId node_id, const geoworld::tooling::HfsmStateDocument& state,
                     bool is_initial,
                     std::vector<geoworld::tooling::HfsmTransitionDocument> transitions);

    QtNodes::NodeId newNodeId() override;
    std::unordered_set<QtNodes::NodeId> allNodeIds() const override;
    std::unordered_set<QtNodes::ConnectionId> allConnectionIds(QtNodes::NodeId node_id) const override;
    std::unordered_set<QtNodes::ConnectionId> connections(QtNodes::NodeId node_id,
        QtNodes::PortType port_type, QtNodes::PortIndex port_index) const override;
    bool connectionExists(QtNodes::ConnectionId connection) const override;
    QtNodes::NodeId addNode(QString node_type) override;
    bool connectionPossible(QtNodes::ConnectionId connection) const override;
    void addConnection(QtNodes::ConnectionId connection) override;
    bool nodeExists(QtNodes::NodeId node_id) const override;
    QVariant nodeData(QtNodes::NodeId node_id, QtNodes::NodeRole role) const override;
    bool setNodeData(QtNodes::NodeId node_id, QtNodes::NodeRole role, QVariant value) override;
    QVariant portData(QtNodes::NodeId node_id, QtNodes::PortType port_type,
                      QtNodes::PortIndex port_index, QtNodes::PortRole role) const override;
    bool setPortData(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex,
                     const QVariant&, QtNodes::PortRole) override;
    bool deleteConnection(QtNodes::ConnectionId connection) override;
    bool deleteNode(QtNodes::NodeId node_id) override;
    bool loopsEnabled() const override { return true; }

private:
    struct State {
        QString stable_id;
        QString name;
        QString parent;
        QPointF position;
    };

    struct Transition {
        QtNodes::ConnectionId connection;
        QString event;
        int priority{};
    };

    [[nodiscard]] QtNodes::PortIndex nextOutputPort(QtNodes::NodeId node_id) const;

    QtNodes::NodeId next_node_id_{1};
    QString machine_id_{"未命名状态机"};
    QtNodes::NodeId initial_id_{QtNodes::InvalidNodeId};
    std::unordered_map<QtNodes::NodeId, State> states_;
    std::vector<Transition> transitions_;
};
