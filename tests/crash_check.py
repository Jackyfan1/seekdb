#!/usr/bin/env python3
"""崩溃恢复自测：按 JSONL 协议与数据库进程交互，kill -9 后重启核对数据。

用法：
  python3 tests/crash_check.py ./build/minivectordb [数据目录]
不依赖 Docker，适用于 Linux 本机自测。
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile


class Client:
    def __init__(self, binary, data_dir):
        self.binary = binary
        self.data_dir = data_dir
        self.proc = None
        self.next_id = 1

    def start(self):
        self.proc = subprocess.Popen(
            [self.binary, self.data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
        )
        ready = json.loads(self.proc.stdout.readline())
        assert ready == {"type": "ready"}, ready

    def execute(self, sql, mode="exact"):
        request = {"id": self.next_id, "op": "execute", "sql": sql, "search_mode": mode}
        self.next_id += 1
        self.proc.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        assert line, "database exited before response"
        response = json.loads(line)
        assert response.get("id") == request["id"], response
        assert type(response.get("ok")) is bool, response
        return response

    def shutdown(self):
        request = {"id": self.next_id, "op": "shutdown"}
        self.next_id += 1
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        response = json.loads(self.proc.stdout.readline())
        assert response == {"id": request["id"], "ok": True}, response
        self.proc.stdin.close()
        assert self.proc.wait(timeout=30) == 0
        self.proc = None

    def kill(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait(timeout=30)
        self.proc = None


def rows(client, sql):
    response = client.execute(sql)
    assert response["ok"] is True, response
    return response.get("rows")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    data_dir = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="mvd-crash-")
    os.makedirs(data_dir, exist_ok=True)
    try:
        client = Client(binary, data_dir)
        client.start()
        client.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));")
        client.execute("INSERT INTO docs VALUES (1, '一'), (2, '二');")
        client.shutdown()

        client.start()
        assert rows(client, "SELECT id FROM docs ORDER BY id;") == [[1], [2]]
        client.execute("BEGIN;")
        client.execute("INSERT INTO docs VALUES (3, '三');")
        client.execute("UPDATE docs SET content = '改' WHERE id = 1;")
        client.kill()

        client.start()
        assert rows(client, "SELECT id FROM docs ORDER BY id;") == [[1], [2]], "未提交事务不能生效"
        assert rows(client, "SELECT content FROM docs WHERE id = 1;") == [["一"]], "未提交更新不能生效"
        assert rows(client, "SELECT id FROM docs WHERE id = 3;") == []
        client.shutdown()
        print("CRASH RECOVERY PASS")
        return 0
    finally:
        if len(sys.argv) <= 2 and os.path.isdir(data_dir):
            shutil.rmtree(data_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
