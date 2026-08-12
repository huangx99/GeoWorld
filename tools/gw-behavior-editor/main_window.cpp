#include "main_window.hpp"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include <sstream>

namespace {

const QStringList hfsm_items{"状态"};
constexpr int transition_columns = 3;
constexpr int transition_event_column = 0;
constexpr int transition_target_column = 1;
constexpr int transition_priority_column = 2;
constexpr int default_transition_priority = 100;
constexpr int initial_window_width = 1440;
constexpr int initial_window_height = 900;
constexpr int parameter_editor_height = 130;
constexpr int transition_editor_height = 180;
constexpr int behavior_palette_index = 0;
constexpr int hfsm_palette_index = 1;

QString read_text(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

bool write_text(const QString& path, const std::string& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(text.data(), static_cast<qint64>(text.size())) == static_cast<qint64>(text.size());
}

bool write_bytes(const QString& path, const std::vector<std::uint8_t>& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()))
            == static_cast<qint64>(bytes.size());
}

std::map<std::string, std::string> parse_parameters(const QString& source) {
    std::map<std::string, std::string> result;
    for (const auto& line : source.split('\n', Qt::SkipEmptyParts)) {
        const auto separator = line.indexOf('=');
        if (separator > 0) {
            result.emplace(line.left(separator).trimmed().toStdString(),
                           line.mid(separator + 1).trimmed().toStdString());
        }
    }
    return result;
}

QString format_parameters(const std::map<std::string, std::string>& parameters) {
    QStringList lines;
    for (const auto& [key, value] : parameters) {
        lines.push_back(QString::fromStdString(key + "=" + value));
    }
    return lines.join('\n');
}

} // namespace

MainWindow::MainWindow() {
    setWindowTitle("GeoWorld 行为工具链编辑器");
    resize(initial_window_width, initial_window_height);

    auto* palette_dock = new QDockWidget("节点库", this);
    palette_stack_ = new QStackedWidget(palette_dock);
    behavior_palette_ = new QListWidget(palette_stack_);
    const auto registry = geoworld::tooling::BehaviorNodeRegistry::defaults();
    for (const auto& descriptor : registry.descriptors()) {
        auto* item = new QListWidgetItem(
            QString::fromStdString(descriptor.display_name + "（" + descriptor.type + "）"),
            behavior_palette_);
        item->setData(Qt::UserRole, QString::fromStdString(descriptor.type));
    }
    hfsm_palette_ = new QListWidget(palette_stack_);
    hfsm_palette_->addItems(hfsm_items);
    palette_stack_->addWidget(behavior_palette_);
    palette_stack_->addWidget(hfsm_palette_);
    palette_dock->setWidget(palette_stack_);
    addDockWidget(Qt::LeftDockWidgetArea, palette_dock);

    auto* property_dock = new QDockWidget("中文属性面板", this);
    auto* property_widget = new QWidget(property_dock);
    auto* property_layout = new QVBoxLayout(property_widget);
    property_form_ = new QFormLayout;
    document_id_ = new QLineEdit(property_widget);
    object_id_ = new QLineEdit(property_widget);
    object_name_ = new QLineEdit(property_widget);
    node_type_ = new QComboBox(property_widget);
    for (const auto& descriptor : registry.descriptors()) {
        node_type_->addItem(QString::fromStdString(descriptor.display_name),
                            QString::fromStdString(descriptor.type));
    }
    parent_id_ = new QLineEdit(property_widget);
    entry_object_ = new QCheckBox("设为根节点", property_widget);
    parameters_ = new QPlainTextEdit(property_widget);
    parameters_->setPlaceholderText("每行一个参数：名称=值");
    parameters_->setMaximumHeight(parameter_editor_height);
    transitions_ = new QTableWidget(0, transition_columns, property_widget);
    transitions_->setHorizontalHeaderLabels({"事件", "目标状态 ID", "优先级"});
    transitions_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    transitions_->setMinimumHeight(transition_editor_height);
    property_form_->addRow("文档 ID", document_id_);
    property_form_->addRow("稳定 ID", object_id_);
    property_form_->addRow("显示名称", object_name_);
    property_form_->addRow("节点类型", node_type_);
    property_form_->addRow("父状态 ID", parent_id_);
    property_form_->addRow("入口对象", entry_object_);
    property_form_->addRow("运行参数", parameters_);
    property_form_->addRow("状态转换", transitions_);
    property_layout->addLayout(property_form_);
    auto* add_transition = new QPushButton("添加转换", property_widget);
    auto* delete_transition = new QPushButton("删除选中转换", property_widget);
    auto* apply = new QPushButton("应用属性", property_widget);
    property_layout->addWidget(add_transition);
    property_layout->addWidget(delete_transition);
    property_layout->addWidget(apply);
    property_layout->addStretch();
    property_dock->setWidget(property_widget);
    addDockWidget(Qt::RightDockWidgetArea, property_dock);

    auto* diagnostic_dock = new QDockWidget("校验与编译诊断", this);
    diagnostics_ = new QPlainTextEdit(diagnostic_dock);
    diagnostics_->setReadOnly(true);
    diagnostic_dock->setWidget(diagnostics_);
    addDockWidget(Qt::BottomDockWidgetArea, diagnostic_dock);

    auto* file_menu = menuBar()->addMenu("文件");
    auto* new_bt_action = file_menu->addAction("新建行为树");
    auto* new_hfsm_action = file_menu->addAction("新建 HFSM");
    auto* open_action = file_menu->addAction("打开编辑文档…");
    auto* save_action = file_menu->addAction("保存");
    auto* export_action = file_menu->addAction("导出 Graphviz DOT…");
    auto* compile_action = file_menu->addAction("编译运行时制品…");
    auto* edit_menu = menuBar()->addMenu("编辑");
    auto* add_action = edit_menu->addAction("添加选中节点");
    auto* layout_action = edit_menu->addAction("Graphviz 自动布局");
    auto* validate_action = edit_menu->addAction("校验文档");
    auto* toolbar = addToolBar("主要操作");
    toolbar->addAction(open_action);
    toolbar->addAction(save_action);
    toolbar->addAction(add_action);
    toolbar->addAction(layout_action);
    toolbar->addAction(validate_action);
    toolbar->addAction(compile_action);

    connect(new_bt_action, &QAction::triggered, this, [this] { newBehaviorTree(); });
    connect(new_hfsm_action, &QAction::triggered, this, [this] { newHfsm(); });
    connect(open_action, &QAction::triggered, this, [this] { openDocument(); });
    connect(save_action, &QAction::triggered, this, [this] { saveDocument(); });
    connect(export_action, &QAction::triggered, this, [this] { exportDot(); });
    connect(compile_action, &QAction::triggered, this, [this] { compileArtifact(); });
    connect(add_action, &QAction::triggered, this, [this] { addSelectedNode(); });
    connect(layout_action, &QAction::triggered, this, [this] { autoLayout(); });
    connect(validate_action, &QAction::triggered, this, [this] { validateDocument(); });
    connect(behavior_palette_, &QListWidget::itemDoubleClicked, this, [this] { addSelectedNode(); });
    connect(hfsm_palette_, &QListWidget::itemDoubleClicked, this, [this] { addSelectedNode(); });
    connect(apply, &QPushButton::clicked, this, [this] { applyProperties(); });
    connect(add_transition, &QPushButton::clicked, this, [this] {
        const auto row = transitions_->rowCount();
        transitions_->insertRow(row);
        transitions_->setItem(row, transition_event_column, new QTableWidgetItem("事件"));
        transitions_->setItem(row, transition_target_column, new QTableWidgetItem);
        transitions_->setItem(row, transition_priority_column,
                              new QTableWidgetItem(QString::number(default_transition_priority)));
    });
    connect(delete_transition, &QPushButton::clicked, this, [this] {
        if (transitions_->currentRow() >= 0) transitions_->removeRow(transitions_->currentRow());
    });
    switchDocument(DocumentKind::behavior_tree);
    statusBar()->showMessage("新建或打开 JSON 编辑文档，所有输出在校验通过后生成");
}

MainWindow::~MainWindow() = default;

void MainWindow::newBehaviorTree() {
    geoworld::tooling::BehaviorTreeDocument document;
    document.tree_id = "NewBehaviorTree";
    document.root = "root";
    document.nodes = {{"root", "AlwaysSuccess", "根节点", {120.0, 80.0}, {}, {}}};
    behavior_model_.setDocument(std::move(document));
    current_path_.clear();
    switchDocument(DocumentKind::behavior_tree);
}

void MainWindow::newHfsm() {
    geoworld::tooling::HfsmDocument document;
    document.machine_id = "NewStateMachine";
    document.initial_state = "initial";
    document.states = {{"initial", "初始状态", "", {120.0, 80.0}}};
    hfsm_model_.setDocument(std::move(document));
    current_path_.clear();
    switchDocument(DocumentKind::hfsm);
}

void MainWindow::switchDocument(DocumentKind kind) {
    kind_ = kind;
    selected_id_ = QtNodes::InvalidNodeId;
    palette_stack_->setCurrentIndex(kind == DocumentKind::behavior_tree
        ? behavior_palette_index : hfsm_palette_index);
    node_type_->setVisible(kind == DocumentKind::behavior_tree);
    parameters_->setVisible(kind == DocumentKind::behavior_tree);
    parent_id_->setVisible(kind == DocumentKind::hfsm);
    transitions_->setVisible(kind == DocumentKind::hfsm);
    entry_object_->setText(kind == DocumentKind::behavior_tree ? "设为根节点" : "设为初始状态");
    rebuildScene();
}

void MainWindow::rebuildScene() {
    scene_.reset();
    auto& model = kind_ == DocumentKind::behavior_tree
        ? static_cast<QtNodes::AbstractGraphModel&>(behavior_model_)
        : static_cast<QtNodes::AbstractGraphModel&>(hfsm_model_);
    scene_ = std::make_unique<QtNodes::BasicGraphicsScene>(model);
    if (view_ == nullptr) {
        view_ = new QtNodes::GraphicsView(scene_.get(), this);
        setCentralWidget(view_);
    } else {
        view_->setScene(scene_.get());
    }
    connect(scene_.get(), &QtNodes::BasicGraphicsScene::nodeClicked, this,
            [this](QtNodes::NodeId node_id) { showNodeProperties(node_id); });
}

QString MainWindow::schemaSource(DocumentKind kind) const {
    const auto filename = kind == DocumentKind::behavior_tree
        ? "behavior_tree.schema.json" : "hfsm.schema.json";
    return read_text(QString{":/schemas/"} + filename);
}

bool MainWindow::loadDocument(const QString& path) {
    const auto source = read_text(path);
    if (source.isEmpty()) {
        statusBar()->showMessage("无法读取编辑文档");
        return false;
    }
    const auto is_hfsm = source.contains("\"machine_id\"");
    const auto document_kind = is_hfsm ? DocumentKind::hfsm : DocumentKind::behavior_tree;
    const auto schema = schemaSource(document_kind);
    const auto parsed = is_hfsm
        ? geoworld::tooling::parse_hfsm_json(source.toStdString(), schema.toStdString())
        : geoworld::tooling::parse_behavior_tree_json(source.toStdString(), schema.toStdString());
    showDiagnostics(parsed.validation);
    if (!parsed.validation.valid()) return false;
    if (is_hfsm) hfsm_model_.setDocument(parsed.hfsm);
    else behavior_model_.setDocument(parsed.behavior_tree);
    current_path_ = path;
    switchDocument(document_kind);
    statusBar()->showMessage("已打开 " + path);
    return true;
}

void MainWindow::openDocument() {
    const auto path = QFileDialog::getOpenFileName(
        this, "打开行为编辑文档", {}, "GeoWorld 行为文档 (*.json)");
    if (path.isEmpty()) return;
    if (!loadDocument(path)) {
        QMessageBox::critical(this, "无法打开", "文档未通过 JSON Schema 或语义校验，请查看诊断面板。");
    }
}

void MainWindow::saveDocument() {
    if (current_path_.isEmpty()) {
        current_path_ = QFileDialog::getSaveFileName(
            this, "保存行为编辑文档", {}, "GeoWorld 行为文档 (*.json)");
    }
    if (current_path_.isEmpty()) return;
    const auto validation = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::validate(behavior_model_.document())
        : geoworld::tooling::validate(hfsm_model_.document());
    showDiagnostics(validation);
    if (!validation.valid()) {
        QMessageBox::warning(this, "无法保存", "文档存在错误，请先修复诊断。");
        return;
    }
    const auto source = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::canonical_json(behavior_model_.document())
        : geoworld::tooling::canonical_json(hfsm_model_.document());
    if (!write_text(current_path_, source)) {
        QMessageBox::critical(this, "保存失败", "无法写入目标文件。");
        return;
    }
    statusBar()->showMessage("已保存 " + current_path_);
}

void MainWindow::validateDocument() {
    const auto validation = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::validate(behavior_model_.document())
        : geoworld::tooling::validate(hfsm_model_.document());
    showDiagnostics(validation);
    statusBar()->showMessage(validation.valid() ? "文档校验通过" : "文档校验失败");
}

void MainWindow::exportDot() {
    const auto validation = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::validate(behavior_model_.document())
        : geoworld::tooling::validate(hfsm_model_.document());
    showDiagnostics(validation);
    if (!validation.valid()) return;
    const auto path = QFileDialog::getSaveFileName(this, "导出 Graphviz DOT", {}, "Graphviz DOT (*.dot)");
    const auto dot = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::export_dot(behavior_model_.document())
        : geoworld::tooling::export_dot(hfsm_model_.document());
    if (!path.isEmpty() && write_text(path, dot)) statusBar()->showMessage("DOT 已导出 " + path);
}

void MainWindow::compileArtifact() {
    const auto validation = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::validate(behavior_model_.document())
        : geoworld::tooling::validate(hfsm_model_.document());
    showDiagnostics(validation);
    if (!validation.valid()) return;
    const auto filter = kind_ == DocumentKind::behavior_tree
        ? "GeoWorld 行为树制品 (*.gwbt)" : "GeoWorld HFSM 制品 (*.gwst)";
    const auto path = QFileDialog::getSaveFileName(this, "编译运行时制品", {}, filter);
    const auto bytes = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::compile(behavior_model_.document())
        : geoworld::tooling::compile(hfsm_model_.document());
    if (!path.isEmpty() && !bytes.empty() && write_bytes(path, bytes)) {
        statusBar()->showMessage(QString("制品已生成 %1（%2 字节）").arg(path).arg(bytes.size()));
    }
}

void MainWindow::autoLayout() {
    geoworld::tooling::DocumentValidation validation;
    if (kind_ == DocumentKind::behavior_tree) {
        auto document = behavior_model_.document();
        validation = geoworld::tooling::apply_graphviz_layout(document);
        if (validation.valid()) behavior_model_.setDocument(std::move(document));
    } else {
        auto document = hfsm_model_.document();
        validation = geoworld::tooling::apply_graphviz_layout(document);
        if (validation.valid()) hfsm_model_.setDocument(std::move(document));
    }
    showDiagnostics(validation);
    if (validation.valid()) {
        rebuildScene();
        view_->fitInView(scene_->itemsBoundingRect(), Qt::KeepAspectRatio);
        statusBar()->showMessage("Graphviz 自动布局完成");
    }
}

void MainWindow::addSelectedNode() {
    const auto center = view_->mapToScene(view_->viewport()->rect().center());
    QtNodes::NodeId id{};
    if (kind_ == DocumentKind::behavior_tree) {
        const auto* item = behavior_palette_->currentItem();
        if (item == nullptr) return;
        id = behavior_model_.addNode(item->data(Qt::UserRole).toString());
        behavior_model_.setNodeData(id, QtNodes::NodeRole::Position, center);
    } else {
        id = hfsm_model_.addNode("State");
        hfsm_model_.setNodeData(id, QtNodes::NodeRole::Position, center);
    }
    showNodeProperties(id);
}

void MainWindow::showNodeProperties(QtNodes::NodeId node_id) {
    selected_id_ = node_id;
    transitions_->setRowCount(0);
    if (kind_ == DocumentKind::behavior_tree) {
        const auto* node = behavior_model_.nodeDocument(node_id);
        if (node == nullptr) return;
        document_id_->setText(behavior_model_.treeId());
        object_id_->setText(QString::fromStdString(node->id));
        object_name_->setText(QString::fromStdString(node->name));
        node_type_->setCurrentIndex(node_type_->findData(QString::fromStdString(node->type)));
        entry_object_->setChecked(behavior_model_.isRoot(node_id));
        parameters_->setPlainText(format_parameters(node->parameters));
        parent_id_->clear();
    } else {
        const auto* state = hfsm_model_.stateDocument(node_id);
        if (state == nullptr) return;
        document_id_->setText(hfsm_model_.machineId());
        object_id_->setText(QString::fromStdString(state->id));
        object_name_->setText(QString::fromStdString(state->name));
        parent_id_->setText(QString::fromStdString(state->parent));
        entry_object_->setChecked(hfsm_model_.isInitial(node_id));
        for (const auto& transition : hfsm_model_.transitionsFrom(node_id)) {
            const auto row = transitions_->rowCount();
            transitions_->insertRow(row);
            transitions_->setItem(row, transition_event_column,
                                  new QTableWidgetItem(QString::fromStdString(transition.event)));
            transitions_->setItem(row, transition_target_column,
                                  new QTableWidgetItem(QString::fromStdString(transition.target)));
            transitions_->setItem(row, transition_priority_column,
                                  new QTableWidgetItem(QString::number(transition.priority)));
        }
    }
}

void MainWindow::applyProperties() {
    if (selected_id_ == QtNodes::InvalidNodeId) return;
    bool updated{};
    if (kind_ == DocumentKind::behavior_tree) {
        const auto* current = behavior_model_.nodeDocument(selected_id_);
        if (current == nullptr) return;
        auto node = *current;
        node.id = object_id_->text().trimmed().toStdString();
        node.name = object_name_->text().trimmed().toStdString();
        node.type = node_type_->currentData().toString().toStdString();
        node.parameters = parse_parameters(parameters_->toPlainText());
        behavior_model_.setTreeId(document_id_->text().trimmed());
        updated = behavior_model_.updateNode(selected_id_, node, entry_object_->isChecked());
    } else {
        const auto* current = hfsm_model_.stateDocument(selected_id_);
        if (current == nullptr) return;
        auto state = *current;
        state.id = object_id_->text().trimmed().toStdString();
        state.name = object_name_->text().trimmed().toStdString();
        state.parent = parent_id_->text().trimmed().toStdString();
        std::vector<geoworld::tooling::HfsmTransitionDocument> transitions;
        for (int row = 0; row < transitions_->rowCount(); ++row) {
            if (transitions_->item(row, transition_event_column) == nullptr
                || transitions_->item(row, transition_target_column) == nullptr) continue;
            transitions.push_back({state.id,
                transitions_->item(row, transition_event_column)->text().trimmed().toStdString(),
                transitions_->item(row, transition_target_column)->text().trimmed().toStdString(),
                transitions_->item(row, transition_priority_column) == nullptr
                    ? default_transition_priority
                    : transitions_->item(row, transition_priority_column)->text().toInt()});
        }
        hfsm_model_.setMachineId(document_id_->text().trimmed());
        updated = hfsm_model_.updateState(selected_id_, state, entry_object_->isChecked(),
                                          std::move(transitions));
    }
    if (!updated) {
        QMessageBox::warning(this, "属性未应用", "稳定 ID 重复、字段为空或转换目标状态不存在。");
        return;
    }
    rebuildScene();
    statusBar()->showMessage("属性已应用，请运行文档校验确认运行语义");
}

bool MainWindow::smokeRoundTrip(const QString& path, QString& message) {
    if (!loadDocument(path)) {
        message = "加载或 Schema 校验失败";
        return false;
    }
    const auto validation = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::validate(behavior_model_.document())
        : geoworld::tooling::validate(hfsm_model_.document());
    if (!validation.valid()) {
        message = "模型往返后的语义校验失败";
        return false;
    }
    const auto source = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::canonical_json(behavior_model_.document())
        : geoworld::tooling::canonical_json(hfsm_model_.document());
    const auto reparsed = kind_ == DocumentKind::behavior_tree
        ? geoworld::tooling::parse_behavior_tree_json(source, schemaSource(kind_).toStdString())
        : geoworld::tooling::parse_hfsm_json(source, schemaSource(kind_).toStdString());
    if (!reparsed.validation.valid()) {
        message = "模型规范化输出未通过 Schema 校验";
        return false;
    }
    message = kind_ == DocumentKind::behavior_tree ? "行为树 GUI 冒烟测试通过" : "HFSM GUI 冒烟测试通过";
    return true;
}

void MainWindow::showDiagnostics(const geoworld::tooling::DocumentValidation& validation) {
    diagnostics_->clear();
    if (validation.diagnostics.empty()) {
        diagnostics_->appendPlainText("校验通过：没有发现问题。");
        return;
    }
    for (const auto& diagnostic : validation.diagnostics) {
        diagnostics_->appendPlainText(QString("%1 %2 [%3] %4")
            .arg(diagnostic.severity == geoworld::tooling::DiagnosticSeverity::error ? "错误" : "警告")
            .arg(QString::fromStdString(diagnostic.code))
            .arg(QString::fromStdString(diagnostic.object_id))
            .arg(QString::fromStdString(diagnostic.message)));
    }
}
