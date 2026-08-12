# gw-st

HFSM 编辑文档命令行工具：

```bash
gw-st validate machine.json
gw-st dot machine.json machine.dot
gw-st layout machine.json machine.layout.json
gw-st compile machine.json machine.gwst
gw-st inspect machine.gwst
```

校验覆盖父状态引用与环路、初始状态、转换引用/冲突和不可达状态。`layout` 由 Graphviz 计算布局，不在工程内维护自制布局算法。
