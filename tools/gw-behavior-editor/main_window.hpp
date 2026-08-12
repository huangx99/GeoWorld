#pragma once

#include "behavior_graph_model.hpp"
#include "hfsm_graph_model.hpp"

#include <QMainWindow>

#include <memory>

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    [[nodiscard]] bool loadDocument(const QString& path);
    [[nodiscard]] bool smokeRoundTrip(const QString& path, QString& message);

private:
    enum class DocumentKind { behavior_tree, hfsm };

    void newBehaviorTree();
    void newHfsm();
    void switchDocument(DocumentKind kind);
    void openDocument();
    void saveDocument();
    void validateDocument();
    void exportDot();
    void compileArtifact();
    void autoLayout();
    void addSelectedNode();
    void showNodeProperties(QtNodes::NodeId node_id);
    void applyProperties();
    void rebuildScene();
    void showDiagnostics(const geoworld::tooling::DocumentValidation& validation);
    [[nodiscard]] QString schemaSource(DocumentKind kind) const;

    DocumentKind kind_{DocumentKind::behavior_tree};
    BehaviorGraphModel behavior_model_;
    HfsmGraphModel hfsm_model_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;
    QtNodes::GraphicsView* view_{};
    QStackedWidget* palette_stack_{};
    QListWidget* behavior_palette_{};
    QListWidget* hfsm_palette_{};
    QPlainTextEdit* diagnostics_{};
    QLineEdit* document_id_{};
    QLineEdit* object_id_{};
    QLineEdit* object_name_{};
    QComboBox* node_type_{};
    QLineEdit* parent_id_{};
    QPlainTextEdit* parameters_{};
    QTableWidget* transitions_{};
    QCheckBox* entry_object_{};
    QFormLayout* property_form_{};
    QtNodes::NodeId selected_id_{QtNodes::InvalidNodeId};
    QString current_path_;
};
