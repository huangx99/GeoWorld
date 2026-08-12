#include "geoworld/tooling/behavior_document.hpp"

#if GW_HAS_GRAPHVIZ
#include <graphviz/cgraph.h>
#include <graphviz/gvc.h>

extern "C" {
extern gvplugin_library_t gvplugin_dot_layout_LTX_library;
}
#endif

#include <memory>
#include <string>

namespace geoworld::tooling {
namespace {

constexpr std::string_view diagnostic_graphviz = "GWTD301";
constexpr double points_per_inch = 72.0;
constexpr double editor_pixels_per_inch = 96.0;

void add_layout_error(DocumentValidation& validation, std::string object_id, std::string message) {
    validation.diagnostics.push_back({std::string{diagnostic_graphviz}, DiagnosticSeverity::error,
                                      std::move(object_id), std::move(message)});
}

#if GW_HAS_GRAPHVIZ

struct GraphContextDeleter {
    void operator()(GVC_t* context) const { static_cast<void>(gvFreeContext(context)); }
};

struct GraphDeleter {
    void operator()(Agraph_t* graph) const { static_cast<void>(agclose(graph)); }
};

using GraphContext = std::unique_ptr<GVC_t, GraphContextDeleter>;
using Graph = std::unique_ptr<Agraph_t, GraphDeleter>;

lt_symlist_t graphviz_plugins[] = {
    {"gvplugin_dot_layout_LTX_library", &gvplugin_dot_layout_LTX_library},
    {nullptr, nullptr}
};

template <typename NodeRange, typename IdFunction, typename EdgeFunction, typename PositionFunction>
DocumentValidation layout(std::string_view document_id, NodeRange& nodes,
                          IdFunction id, EdgeFunction edges, PositionFunction set_position,
                          std::string_view engine, std::string_view rank_direction) {
    DocumentValidation validation;
    GraphContext context{gvContextPlugins(graphviz_plugins, 0)};
    Graph graph{agopen(const_cast<char*>("geoworld_layout"), Agdirected, nullptr)};
    if (!context || !graph) {
        add_layout_error(validation, std::string{document_id}, "Graphviz 上下文创建失败");
        return validation;
    }
    static_cast<void>(agattr(graph.get(), AGRAPH, const_cast<char*>("rankdir"),
                             const_cast<char*>(std::string{rank_direction}.c_str())));
    for (const auto& node : nodes) {
        static_cast<void>(agnode(graph.get(), const_cast<char*>(id(node).c_str()), 1));
    }
    for (const auto& node : nodes) {
        auto* source = agnode(graph.get(), const_cast<char*>(id(node).c_str()), 0);
        for (const auto& target_id : edges(node)) {
            auto* target = agnode(graph.get(), const_cast<char*>(target_id.c_str()), 0);
            if (source != nullptr && target != nullptr) {
                static_cast<void>(agedge(graph.get(), source, target, nullptr, 1));
            }
        }
    }
    const std::string layout_engine{engine};
    if (gvLayout(context.get(), graph.get(), layout_engine.c_str()) != 0) {
        add_layout_error(validation, std::string{document_id},
                         "Graphviz 布局引擎不可用: " + layout_engine);
        return validation;
    }
    const auto canvas_height = GD_bb(graph.get()).UR.y;
    for (auto& node : nodes) {
        auto* graph_node = agnode(graph.get(), const_cast<char*>(id(node).c_str()), 0);
        if (graph_node == nullptr) continue;
        const auto coordinate = ND_coord(graph_node);
        set_position(node, coordinate.x * editor_pixels_per_inch / points_per_inch,
                     (canvas_height - coordinate.y) * editor_pixels_per_inch / points_per_inch);
    }
    static_cast<void>(gvFreeLayout(context.get(), graph.get()));
    return validation;
}

#endif

} // namespace

DocumentValidation apply_graphviz_layout(BehaviorTreeDocument& document, std::string_view engine) {
#if GW_HAS_GRAPHVIZ
    return layout(document.tree_id, document.nodes,
        [](const auto& node) { return node.id; },
        [](const auto& node) { return node.children; },
        [](BehaviorNodeDocument& source, double x, double y) {
            source.position = {x, y};
        }, engine, "TB");
#else
    static_cast<void>(engine);
    DocumentValidation validation;
    add_layout_error(validation, document.tree_id, "当前构建未启用 Graphviz 布局依赖");
    return validation;
#endif
}

DocumentValidation apply_graphviz_layout(HfsmDocument& document, std::string_view engine) {
#if GW_HAS_GRAPHVIZ
    return layout(document.machine_id, document.states,
        [](const auto& state) { return state.id; },
        [&document](const auto& state) {
            std::vector<std::string> targets;
            for (const auto& transition : document.transitions) {
                if (transition.source == state.id) targets.push_back(transition.target);
            }
            for (const auto& candidate : document.states) {
                if (candidate.parent == state.id) targets.push_back(candidate.id);
            }
            return targets;
        },
        [](HfsmStateDocument& source, double x, double y) {
            source.position = {x, y};
        }, engine, "LR");
#else
    static_cast<void>(engine);
    DocumentValidation validation;
    add_layout_error(validation, document.machine_id, "当前构建未启用 Graphviz 布局依赖");
    return validation;
#endif
}

} // namespace geoworld::tooling
