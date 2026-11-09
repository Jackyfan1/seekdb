# MiniVectorDB

2026 OceanBase 数据库大赛加赛 · 赛题一「完成一个轻量向量数据库」的实现。

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
./build/minivectordb --data-dir=/data [--ready-marker=TOKEN]
```

- 通过 stdin 接收 SQL，stdout 返回结果，stderr 写日志。
- 数据统一写入 `--data-dir`（默认 `/data`）。
- `--ready-marker` 在每条语句响应后追加一行标记，便于测试驱动判断响应边界。

## SQL 支持范围

- `CREATE TABLE`：`BIGINT`（一个主键）、`VARCHAR(n)`、`VECTOR(n)`（每表最多一个向量列）。
- `INSERT` / `SELECT` / `UPDATE` / `DELETE`；多行 INSERT；`WHERE` 条件（`=`、`<`、`<=`、`>`、`>=`、`AND`）、`ORDER BY`、`LIMIT`。
- 向量检索：`L2_DISTANCE(col, '[a, b, ...]')` 精确 TopK，支持带普通列过滤。
- 事务：`BEGIN` / `COMMIT` / `ROLLBACK`（同一时刻最多一个显式事务）。
- 向量值以单引号字符串表示，如 `'[1, 0, 0]'`。

不支持的语句返回 `ERROR <NAME>: <message>`，不会静默执行。

## 存储与恢复

- 每次提交（自动提交或显式 COMMIT）后整库写快照到 `<data_dir>/minivectordb.snap`，采用「写临时文件 → fsync → rename → fsync 目录」保证提交成功即落盘。
- 启动时读取快照；进程被强制终止后重启，已提交数据保留，未提交事务回滚。
- 事务通过 undo-log 实现，语句失败时保证语句级原子性。

## 输出格式

- `SELECT`：结果行，制表符分隔，无表头。
- `INSERT`/`UPDATE`/`DELETE`：`OK <受影响行数>`。
- `CREATE TABLE` / 事务语句：`OK`。
- 错误：`ERROR <NAME>: <message>`。

## 自测

```bash
python3 tests/self_test.py     # 公开用例
python3 tests/crash_test.py    # 崩溃恢复
```
