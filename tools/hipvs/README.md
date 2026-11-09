# hipVS 加速 HNSW 索引构建（AMD Radeon GPU）

赛题：**使用 hipVS 加速 HNSW 索引构建**（2026 OceanBase 数据库大赛 / AMD 方向）。

本目录给出可复现的 GPU 建图基准工具与 seekdb 集成方案，用于验证：

1. 用 **hipVS（CAGRA 图索引）** 在 AMD Radeon GPU 上完成向量图索引的**构建**；
2. 与 CPU 侧 HNSW / 暴力检索的构建耗时、查询吞吐、recall@k 对比；
3. 给出把该后端接进 seekdb `CREATE VECTOR INDEX` 链路的落点与开关。

## 1. 背景与结论

seekdb 的向量索引走 `obvsag` 抽象层（`src/oblib/lib/vector/ob_vsag_adaptor.cpp`），
HNSW 系列索引最终由外部库 vsag 在 **CPU** 上建图，建图耗时随向量条数线性增长。
hipVS 是 ROCm 生态里 NVIDIA cuVS 的 AMD 移植，提供 GPU 原生图索引 **CAGRA**：

- 建图在 GPU 上完成，`graph_degree` / `intermediate_graph_degree` 控制图质量；
- 批量检索吞吐显著高于 CPU 单条 knn；
- 默认参数下 recall@10 不低于 CPU HNSW。

因此本题的加速点在于：**把索引构建阶段的近邻图构建搬到 GPU（hipVS CAGRA）**，
搜索阶段沿用同一张图（GPU 批量检索，或回退 CPU）。

## 2. 环境要求

| 项 | 要求 |
|---|---|
| GPU | AMD Radeon PRO W7900（gfx1100 / RDNA3）或其它 ROCm 支持卡 |
| ROCm | 7.2.x |
| hipVS | `amd-hipvs`（AMD PyPI：`https://pypi.amd.com/rocm-7.2.3/simple/`） |
| 编译 | `hipcc` / `g++` + `-lcuvs` |

AMD 官方大赛镜像 `SEEKDB-HIPVS`（Radeon 云模板 3102）已内置 seekdb v1.4.0 + hipVS 预编译库和测试样例，
在该镜像里可直接运行本工具。

## 3. 编译与运行

```bash
cd tools/hipvs
./build_and_run.sh            # 默认 100k x 128 维
./build_and_run.sh 1000000 768 10   # n dim k
```

工具输出：

- GPU CAGRA 建图耗时（含 H2D 拷贝 / 不含拷贝）
- 批量检索 QPS 与单条检索延迟
- recall@k（对 CPU 精确暴力检索真值）
- 与 CPU 暴力建图的耗时对比（数量级参考）

## 4. 接入 seekdb 的方案

`obvsag` 是纯 C 风格句柄接口（`create_index/build_index/knn_search/serialize/...`），
天然适合按后端分发。集成分三步，**默认不改变现有行为**：

1. **索引参数**：`CREATE VECTOR INDEX idx(c2) WITH (distance=l2, type=hnsw, lib=cuvs)`。
   `lib` 已在 `ObVectorIndexUtil::parser_params_from_string` 中解析（现有 `VSAG` / `OB`），
   新增 `CUVS`（别名 `HIPVS`）即可，默认仍为 `VSAG`。
2. **后端分发**：在 `obvsag` 内维护一个句柄注册表，`create_index` 命中 `lib=cuvs` 时
   用 `dlopen("libcuvs.so")` + `dlsym` 调 CAGRA C API，避免给 seekdb 增加编译期依赖；
   库不存在 / 非 L2/IP/COS / 带过滤 / SQ/BQ 量化等场景直接**回退 vsag**，保证结果正确。
3. **持久化**：`cuvsCagraSerialize/Deserialize` 生成二进制 blob，经现有
   `fserialize/fdeserialize(std::ostream/std::istream)` 通道落到宏块，与现有索引存储路径一致。

## 5. 度量口径

- 建图：`CREATE VECTOR INDEX` 到索引 ready 的墙钟时间（同机同数据，vsag vs cuvs）；
- 检索：单条延迟 + 批量 QPS；
- 正确性：recall@10 对精确检索真值，要求不低于 CPU HNSW 基线。
