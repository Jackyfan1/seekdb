#!/usr/bin/env python3
"""崩溃恢复测试：提交后强杀数据保留；未提交事务强杀后回滚。"""
import os
import signal
import subprocess
import sys
import tempfile

BIN = os.path.join(os.path.dirname(__file__), "..", "build", "minivectordb")
MARKER = "__MVD_READY__"


def start(datadir):
    return subprocess.Popen(
        [BIN, "--data-dir=" + datadir, "--ready-marker=" + MARKER],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def send(proc, stmt):
    proc.stdin.write((stmt + "\n").encode())
    proc.stdin.flush()
    lines = []
    while True:
        raw = proc.stdout.readline()
        if not raw:
            return lines, True
        line = raw.decode().rstrip("\n")
        if line == MARKER:
            return lines, False
        lines.append(line)


def kill9(proc):
    proc.send_signal(signal.SIGKILL)
    proc.wait()


def select_rows(datadir):
    p = start(datadir)
    lines, dead = send(p, "SELECT id, content FROM docs ORDER BY id;")
    p.stdin.close()
    p.wait(timeout=5)
    return lines


def main():
    tmp = tempfile.mkdtemp(prefix="mvd_crash_")

    # 场景 1：提交后强杀，数据保留
    p = start(tmp)
    send(p, "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));")
    send(p, "INSERT INTO docs VALUES (1, '一'), (2, '二');")  # 自动提交
    kill9(p)

    rows = select_rows(tmp)
    assert rows == ["1\t一", "2\t二"], "committed data lost after crash: %r" % rows
    print("[PASS] committed rows survive crash:", rows)

    # 场景 2：未提交事务强杀，回滚
    p = start(tmp)
    send(p, "BEGIN;")
    send(p, "INSERT INTO docs VALUES (3, '三');")
    kill9(p)  # 未 COMMIT

    rows = select_rows(tmp)
    assert rows == ["1\t一", "2\t二"], "uncommitted data leaked: %r" % rows
    print("[PASS] uncommitted rows rolled back:", rows)

    print("CRASH RECOVERY PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
