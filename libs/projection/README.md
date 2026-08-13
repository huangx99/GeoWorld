# projection

把权威世界状态投影为客户端可消费的快照、delta、keyframe 和 FID↔WID 映射。

M4 的完整模块边界、数据契约和验收标准见 [`docs/M4.md`](../../docs/M4.md)。

M4-A 已实现：版本化确定性排序的 `ProjectedEntity`、`ProjectionPolicy` 字段白名单、基于 `SpatialQuery` 的 AOI 候选与相关性过滤、每连接 WID↔FID 映射与已确认基线、keyframe 及 enter/update/leave delta、ack 幂等与基线淘汰回退、规范化稳定 hash。编码与网络扇出属于后续批次，不在本模块。

热路径实现要点（语义与逐实体全量重算逐字节一致，见 `docs/M4.md` 容量记录）：脏扫描用按 WID 升序的世界指针快照与脏检查基线缓存两路归并（纯顺序读，成员变化才重建），跳过未变实体的 project+hash；空间索引只随真实位置变化更新，AOI 可见集按（空间修订号、bounds）缓存，缓存命中时帧构建只扫候选集（未确认操作 ∪ 本 tick 变化 ∩ 可见 ∪ 频率暂缓）；周期 keyframe 首轮按连接 ID 相位错峰、之后严格 interval 间隔（相位偏移永久保持，强制 keyframe 后重新错峰一次）；`set_thread_pool` 注入线程池后 project+hash、逐连接帧构建与历史环淘汰析构分块并行（策略 resolver 须保持纯净），未注入时保持串行，两种路径输出一致。
