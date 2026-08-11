# Third-party dependencies

GeoWorld 优先使用 vcpkg manifest 管理三方库。`vendor/` 仅用于必须随仓库冻结的源码；`vcpkg-overlays/` 用于本地 port 与补丁。当前 WasmEdge overlay 处理 GCC 16 新增诊断与上游 `-Werror` 的兼容问题。安装结果、下载缓存和生成代码均放在 `build/`，不提交。
