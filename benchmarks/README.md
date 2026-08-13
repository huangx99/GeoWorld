# benchmarks

GeoWorld 自有基准与压测：R1 flecs/EnTT 负载、R3 2.5D AOI、R4 Gateway 扇出、R5 插件边界和跨平台确定性。

M3 R5 基准在 Release 构建中以相同的 `value > 0` 规则测量真实动态库 C ABI、Lua/sol2 和 WasmEdge 调用边界：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-m3-release
cmake --build --preset vcpkg-m3-release --target geoworld-m3-benchmark
./build/vcpkg-m3-release/geoworld-m3-benchmark
```

输出包含每条轨道的调用次数、总纳秒数和单次调用纳秒数。结果是当前机器的边界基线，不代表带序列化、宿主函数和领域逻辑后的容量。

M4 R4 扇出基准需要 `GW_BUILD_M4=ON` 与 vcpkg `m4` feature，Release 构建：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake -S . -B build/vcpkg-m4-release -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_FEATURES=m4 -DGW_BUILD_M4=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/vcpkg-m4-release --target geoworld-m4-benchmark
./build/vcpkg-m4-release/geoworld-m4-benchmark --scenario all
```

场景与默认值冻结于 docs/M4.md「R4 可复现基准」：`memory`（内存 sink，测投影 + 编码 + 入队）、
`loopback`（真实 WebSocket 传输）、以及各自 5% 慢客户端变体。实体数、连接数、AOI 可见数、
预热/采样时长、慢客户端占比和随机种子全部经 CLI 覆盖（`--help`）；标准负载为
100,000 实体 / 256 连接 / 每连接约 2,000 可见 / 20 Hz / 预热 30 s / 采样 10 min。
输出机器、编译器、构建类型、依赖版本与全部负载参数，结果是当前机器的容量记录。
基准进程默认将全局 operator new/delete 转发到 mimalloc（vcpkg `m4` feature 提供；
`benchmarks/mi_new_override.cpp`，库不感知），缺失该依赖时回退系统 malloc，仅影响性能不影响输出。
