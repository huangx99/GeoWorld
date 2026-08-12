# gw-bt

行为树编辑文档命令行工具：

```bash
gw-bt validate tree.json
gw-bt dot tree.json tree.dot
gw-bt layout tree.json tree.layout.json
gw-bt compile tree.json tree.gwbt
gw-bt inspect tree.gwbt
```

`layout` 使用 Graphviz dot 引擎写回编辑坐标；`compile` 输出版本化 FlatBuffers 制品。所有入口都会先执行 JSON Schema 与语义校验。
