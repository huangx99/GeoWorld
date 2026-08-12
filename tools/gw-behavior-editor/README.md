# GeoWorld 行为工具链编辑器

中文桌面编辑器同时支持行为树与 HFSM。行为树可编辑稳定 ID、显示名称、BehaviorTree.CPP 节点类型与参数；HFSM 可编辑父状态、初始状态以及事件、目标和优先级转换。画布数据直接映射 `tooling` 文档，不保存独立 UI 私有格式。

构建与运行：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-m3-tooling
cmake --build --preset vcpkg-m3-tooling
./build/vcpkg-m3-tooling/gw-behavior-editor
```

无显示环境可执行文档往返冒烟测试：

```bash
QT_QPA_PLATFORM=minimal ./build/vcpkg-m3-tooling/gw-behavior-editor \
  --smoke assets/fixtures/m3_behavior_tree.json
```
