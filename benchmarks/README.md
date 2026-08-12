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
