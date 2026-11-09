#!/usr/bin/env python3
"""MiniVectorDB 公开用例自测驱动。

读取 mysqltest/cases/<name>.test 与 <name>.result，对每个用例使用独立空
数据目录启动引擎，逐条发送 SQL 并比较 stdout 响应与预期断言。
"""
import json
import os
import subprocess
import sys
import tempfile

BIN = os.environ.get("MVD_BIN", os.path.join(os.path.dirname(__file__), "..", "build", "minivectordb"))
CASES_DIR = os.path.join(os.path.dirname(__file__), "..", "mysqltest", "cases")
MARKER = "__MVD_READY_9f3c7a__"


def parse_value(s):
    s = s.strip()
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        pass
    return s


def values_equal(a, b):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) < 1e-5
    return a == b


class Proc:
    def __init__(self, datadir):
        self.p = subprocess.Popen(
            [BIN, "--data-dir=" + datadir, "--ready-marker=" + MARKER],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

    def request(self, stmt):
        self.p.stdin.write((stmt + "\n").encode("utf-8"))
        self.p.stdin.flush()
        lines = []
        while True:
            raw = self.p.stdout.readline()
            if not raw:
                raise RuntimeError("engine exited before answering: %r" % stmt)
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            if line == MARKER:
                return lines
            lines.append(line)

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


def split_response(lines):
    status = []
    rows = []
    for l in lines:
        if l.startswith("OK") or l.startswith("ERROR"):
            status.append(l)
        else:
            rows.append(l)
    return status, rows


def compare_rows(actual_rows, expected_rows, unordered):
    if len(actual_rows) != len(expected_rows):
        return False, "row count %d != %d" % (len(actual_rows), len(expected_rows))
    actual = [[parse_value(c) for c in r.split("\t")] for r in actual_rows]
    if unordered:
        # 多重集合比较
        actual = sorted(actual, key=lambda r: json.dumps(r, ensure_ascii=False))
        expected = sorted(expected_rows, key=lambda r: json.dumps(r, ensure_ascii=False))
    else:
        expected = expected_rows
    for i, (ar, er) in enumerate(zip(actual, expected)):
        if len(ar) != len(er):
            return False, "row %d width %d != %d" % (i, len(ar), len(er))
        for j, (a, e) in enumerate(zip(ar, er)):
            if not values_equal(a, e):
                return False, "row %d col %d: %r != %r" % (i, j, a, e)
    return True, ""


def run_case(name):
    test = os.path.join(CASES_DIR, name + ".test")
    result = os.path.join(CASES_DIR, name + ".result")
    stmts = [l.rstrip("\n") for l in open(test, encoding="utf-8") if l.strip()]
    expected = json.load(open(result, encoding="utf-8"))
    if len(stmts) != len(expected):
        return False, "statement count %d != expectation count %d" % (len(stmts), len(expected))

    tmp = tempfile.mkdtemp(prefix="mvd_")
    proc = Proc(tmp)
    try:
        for i, (stmt, exp) in enumerate(zip(stmts, expected)):
            lines = proc.request(stmt)
            status, rows = split_response(lines)
            errors = [s for s in status if s.startswith("ERROR")]

            if not exp:  # {}
                if errors:
                    return False, "[%d] %r -> %s" % (i, stmt, errors[0])
                continue

            if "affected_rows" in exp:
                if errors:
                    return False, "[%d] %r -> %s" % (i, stmt, errors[0])
                n = None
                for s in status:
                    parts = s.split()
                    if parts and parts[0] == "OK" and len(parts) > 1:
                        n = int(parts[1])
                if n != exp["affected_rows"]:
                    return False, "[%d] %r affected %r != %r" % (i, stmt, n, exp["affected_rows"])
                continue

            if "columns" in exp:
                if errors:
                    return False, "[%d] %r -> %s" % (i, stmt, errors[0])
                ok, msg = compare_rows(rows, exp["rows"], exp.get("unordered", False))
                if not ok:
                    return False, "[%d] %r -> %s" % (i, stmt, msg)
                continue

            return False, "[%d] unknown assertion: %r" % (i, exp)
    finally:
        proc.close()
    return True, ""


def main():
    names = [f[:-5] for f in os.listdir(CASES_DIR) if f.endswith(".test")]
    names.sort()
    all_ok = True
    for name in names:
        ok, msg = run_case(name)
        status = "PASS" if ok else "FAIL"
        if not ok:
            all_ok = False
        print("[%s] %s %s" % (status, name, msg))
    print("=" * 50)
    print("ALL PASS" if all_ok else "SOME FAILED")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
