# GeoWorld

GeoWorld 是面向数字孪生与推演场景的 headless 可计算三维世界运行平台。

M1 世界内核、M2 空间与流式、M3 规则与 AI 的核心范围已完成。工程采用 CMake + vcpkg，核心依赖通过 manifest 锁定；生成代码和三方安装结果均位于 `build/`，不提交到仓库。

- 当前阶段：[M3 规则与 AI](docs/M3.md)
- 阶段索引：[docs/README.md](docs/README.md)
- 工程结构：[工程文件组织](docs/engineering-layout.md)
- 开发规范：[AGENTS.md](AGENTS.md)

## 构建

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

## 离线成果演示

```bash
cmake --build --preset default --target geoworld-showcase
./build/default/geoworld-showcase
```

浏览器直接打开 `build/showcase/showcase.html`，不需要网络服务。演示说明见 [docs/SHOWCASE.md](docs/SHOWCASE.md)。

页面包含可旋转的三维园区疏散，以及使用 WGS84/ECEF/ENU 坐标链路、实际生成 20 万实体并执行确定性火力与战损结算的虚构演习场景。

包含 flecs 的完整 M1 构建使用 vcpkg preset：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg
cmake --build --preset vcpkg
ctest --preset vcpkg
```

ECS 对照基准使用单独 preset，并启用 EnTT feature：

```bash
cmake --preset vcpkg-bench
cmake --build --preset vcpkg-bench
./build/vcpkg-bench/geoworld-m1-ecs-comparison
```

M3 完整依赖构建启用 BehaviorTree.CPP、Lua 和 WasmEdge：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-m3
cmake --build --preset vcpkg-m3
ctest --preset vcpkg-m3
```
