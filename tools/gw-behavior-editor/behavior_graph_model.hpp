#pragma once

#include "geoworld/tooling/behavior_document.hpp"

#include <QtNodes/AbstractGraphModel>
#include <QPointF>

#include <unordered_map>
#include <unordered_set>

class BehaviorGraphModel final : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    BehaviorGraphModel();

    void setDocument(geoworld::tooling::BehaviorTreeDocument document);
    [[nodiscard]] geoworld::tooling::BehaviorTreeDocument document() const;
    [[nodiscard]] const geoworld::tooling::BehaviorNodeDocument* nodeDocument(
        QtNodes::NodeId node_id) const;
    [[nodiscard]] bool isRoot(QtNodes::NodeId node_id) const noexcept;
    [[nodiscard]] QString treeId() const;
    void setTreeId(QString tree_id);
    bool updateNode(QtNodes::NodeId node_id,
                    const geoworld::tooling::BehaviorNodeDocument& document,
                    bool is_root);

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
    bool loopsEnabled() const override { return false; }

private:
    struct Node {
        QString stable_id;
        QString type;
        QString name;
        QPointF position;
        std::map<std::string, std::string> parameters;
    };

    void resetModel();
    [[nodiscard]] unsigned int outputCount(QtNodes::NodeId node_id) const;

    QtNodes::NodeId next_node_id_{1};
    QString tree_id_{"未命名行为树"};
    QtNodes::NodeId root_id_{QtNodes::InvalidNodeId};
    std::unordered_map<QtNodes::NodeId, Node> nodes_;
    std::unordered_set<QtNodes::ConnectionId> connections_;
};
