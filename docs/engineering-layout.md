# GeoWorld 工程文件组织

## 组织原则

- 语义对象层与 ECS 运行时层分离；`world_model` 不依赖 flecs。
- `simulation` 是 tick、FEL 和 command buffer 的唯一编排边界。
- `projection` 负责世界投影，`gateway` 负责连接和协议。
- 领域扩展只能新增 schema、System/规则和 AI 节点。
- 分布式能力先在 `libs/distribution` 冻结接口，M6 前不进入热路径。
- `tooling` 定义资产生产与校验接口，编辑器实现不进入运行时依赖图。

## 依赖方向

```text
foundation -> schema -> world_model -> ecs_runtime -> simulation
spatial ------------------------------------------^     |
                                                   +--> ai / rules
simulation -> projection -> protocol -> gateway
world_model + ecs_runtime + simulation -> persistence
world_model + ecs_runtime + spatial + simulation -> debug
schema + world_model + debug -> tooling -> tools
```

## 可观测性与调试

`observability` 和 `debug` 是两个不同边界：

- `observability` 面向生产环境，提供结构化日志、metrics、trace、tick 预算、AOI/网关扇出和 Cell 生命周期指标；
- `debug` 面向开发、测试和回放，提供只读 Inspector、暂停/单步、条件断点、状态 hash 和命令/FEL 诊断。

日志和调试输出不能改变仿真结果。确定性模式下禁止把墙钟、线程顺序或随机日志副作用带入世界状态；高频日志必须采样或进入有界 ring buffer。

M1 必须先完成以下基础能力：

1. 统一日志接口和等级/分类；
2. tick、Track A/B/C、命令缓冲、FEL、Cell 和 AOI 的核心指标；
3. 可配置的 tick 状态 hash；
4. 只读世界 Inspector；
5. pause/step/run-to-tick 控制；
6. 回放差异的 hash 二分定位。

## 三方库

依赖由 `vcpkg.json` 管理，安装结果位于 `build/vcpkg_installed/`。只有需要固定源码、维护补丁或等待上游包的依赖才放入 `third_party/vendor/`。任何三方类型都应封装在对应库的 adapter 中，不能泄漏到 GeoWorld 公共 API。

## 里程碑映射

| 里程碑 | 目录 |
|---|---|
| M1 世界内核 | `foundation`、`schema`、`world_model`、`ecs_runtime`、`simulation`、`observability`、`debug`、`tooling` |
| M2 空间与流式 | `spatial`、`data` |
| M3 规则与 AI | `rules`、`ai`、`domains`、`plugins` |
| M4 接入层 | `projection`、`protocol`、`gateway` |
| M5 持久化回放 | `persistence`、`apps/geoworld-replay` |
| M6 分布式 | `distribution`、`deploy` |
