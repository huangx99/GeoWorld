# data schemas

FlatBuffers 数据面 schema：快照、delta、keyframe、事件和投影元数据。

现有 `runtime_snapshot.fbs` 是 ECS 运行时快照，不是客户端流协议。M4 将使用独立的 `GWSF`/`GWCF` 文件标识，详细契约见 [`docs/M4.md`](../../docs/M4.md)。
