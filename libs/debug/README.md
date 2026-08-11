# debug

运行时调试与检查接口，不承载业务逻辑，也不改变仿真权威状态。

职责包括：

- 只读 Inspector：语义对象、ECS 组件、关系、Cell、AOI、AI 状态；
- tick 控制：暂停、单步、运行到指定 tick；
- 条件断点：WID、Cell、事件、System、规则和 AI 状态转换；
- 状态 hash：按 tick 记录并支持回放二分定位；
- 命令缓冲、FEL、Cell 生命周期和插件调用的诊断快照。

调试控制必须通过显式 debug API 注入，不能让调试代码直接写世界内存。
