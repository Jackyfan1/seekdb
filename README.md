# MiniVectorDB

2026 OceanBase 数据库大赛加赛 · 「轻量向量数据库（MiniVectorDB）」的实现。

从零实现一个单机、单进程、单客户端的轻量向量数据库，支持基础 SQL、向量检索、持久化存储、事务与崩溃恢复。

## 语言与构建

- 语言：C++17（标准库，无第三方存储/向量库依赖）
- 目标环境：Linux x86_64 容器（Ubuntu 24.04）
- 本地构建：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

产物为 `build/minivectordb`。

## 启动

```bash
./build/minivectordb --data-dir=/data
```

- 容器内以前台进程运行，只通过 **stdin/stdout 的 JSON Lines 协议** 与判题器通信；
  协议见根目录 [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)。
- 启动完成恢复后先输出一行 `{"type":"ready"}` 并 flush，然后逐行读取请求。
- 数据统一写入 `--data-dir`（默认 `/data`，也兼容 `VECTORDB_DATA_DIR` 环境变量与位置参数）。
- stderr 只写诊断日志，stdout 只输出协议报文。

请求/响应示例：

```json
{"id":1,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":1,"ok":true,"columns":["id","content"],"rows":[[1,"你好"]]}
```

`execute` 成功时按语句类型返回 `columns`+`rows`（查询）或 `affected_rows`（增删改）；
失败返回 `{"ok":false,"error":{"code":"duplicate_key","message":"..."}}`；
`{"op":"shutdown"}` 回滚未提交事务、落盘并以退出码 0 结束。

## SQL 支持范围

- `CREATE TABLE`：`BIGINT`（一个主键）、`VARCHAR(n)`、`VECTOR(n)`（每表最多一个向量列）。
- `INSERT` / `SELECT` / `UPDATE` / `DELETE`；多行 INSERT；`WHERE` 条件（`=`、`<`、`<=`、`>`、`>=`、`AND`）、`ORDER BY`、`LIMIT`。
- 向量检索：`L2_DISTANCE(col, '[a, b, ...]')` 精确 TopK，支持带普通列过滤；
  `CREATE VECTOR INDEX name ON t(col)` 与 `ann` 模式均可执行（精确扫描后端，召回率 1.0）。
- 事务：`BEGIN` / `COMMIT` / `ROLLBACK`（同一时刻最多一个显式事务）。
- 向量值以单引号字符串表示，如 `'[1, 0, 0]'`，按 float32 语义存储与计算。
- `ALTER TABLE` 等题目不支持的语句返回 `unsupported_sql`，不会静默执行。

## 存储与恢复

- 每次提交（自动提交或显式 COMMIT）后整库写快照到 `<data_dir>/minivectordb.snap`，采用
  「写临时文件 → fsync → rename → fsync 目录」保证提交成功即落盘。
- 启动时读取快照；进程被强制终止后重启，已提交数据保留，未提交事务回滚。
- 事务通过 undo-log 实现，语句失败时保证语句级原子性。

## 自测

```bash
# 1) 协议握手 + 20 个公开用例（官方题包自带工具）
python3 protocol_check/run.py --command "[\"$PWD/build/minivectordb\",\"{data_dir}\"]" \
  --handshake-only --output /tmp/pc-handshake
python3 protocol_check/run.py --command "[\"$PWD/build/minivectordb\",\"{data_dir}\"]" \
  --output /tmp/pc-public

# 2) 崩溃恢复（kill -9 后重启核对）
python3 tests/crash_check.py ./build/minivectordb

# 3) 正式提交前用实际镜像再跑一遍
docker build -t minivectordb:test .
python3 protocol_check/run.py --image minivectordb:test --output /tmp/pc-image
```
