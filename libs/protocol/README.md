# protocol

承载 Protobuf/gRPC 控制面和 FlatBuffers 数据面的生成代码适配。schema 来源在仓库根目录 `schemas/`，生成物在 `build/generated/`。

M4 的完整协议版本、文件标识、错误码和三方库决策见 [`docs/M4.md`](../../docs/M4.md)。M4-B 已实现：版本协商、稳定错误码、`ProtocolLimits`、传输无关线结构、GWSF/GWCF 双向编解码（先校验后解码）、参考客户端 replica accumulator 与插值输入缓冲。
