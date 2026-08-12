# tooling

工具链公共库负责行为编辑文档、稳定诊断、JSON Schema 校验、Graphviz 自动布局、FlatBuffers 制品编译和加载校验。编辑器与 CLI 都使用这里的同一份文档模型，运行时不依赖 Qt 或编辑器实现。

行为树节点类型由 `BehaviorNodeRegistry` 注册；内置项对应 BehaviorTree.CPP 4.x，领域插件可增加有明确子节点基数的节点描述，不能绕过注册表接受未知语义。相同文档重复序列化、导出 DOT 和编译会保持稳定顺序。

行为树制品保存已验证节点模型及 BehaviorTree.CPP XML；HFSM 制品保存状态、父级和转换。加载器会验证 FlatBuffers identifier、类型、格式/schema/compiler 版本与 source hash，HFSM 状态按父级依赖顺序构建。
