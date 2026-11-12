#!/usr/bin/env python3
"""Deterministic OpenAI/Ollama-compatible HTTP mock for AI function mysqltests.

The competition ships ai_function_enhancement.test against a mock service that
listens on 0.0.0.0:8000.  This script provides the same contract so the suite can
run locally:

  * POST /compatible-mode/v1/chat/completions  -> OpenAI chat completion
  * POST /compatible-mode/v1/embeddings         -> OpenAI embeddings
  * POST /v1/rerank                              -> SiliconFlow style rerank
  * POST /api/generate                           -> Ollama native generation
  * POST /api/embed                              -> Ollama native embeddings

Usage:
  ai_function_mock.py start [port]
  ai_function_mock.py stop  [port]
  ai_function_mock.py serve [port]
"""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


DEFAULT_PORT = 8000
EMBED_DIMENSION = 768


def state_dir(port):
    return "/tmp/seekdb_ai_function_mock_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
    except (ValueError, ProcessLookupError):
        pass
    try:
        os.unlink(pid_path)
    except FileNotFoundError:
        pass


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    output = open(state_path(port, "server.out"), "a", encoding="utf-8")
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(process.pid))
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.02)
    raise RuntimeError("AI function mock did not start on port %d" % port)


def vector(index):
    return [float(index + 1)] + [0.0] * (EMBED_DIMENSION - 1)


def serve(port):
    lock = threading.Lock()
    log_path = state_path(port, "events.jsonl")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            path = parsed.path

            if path.endswith("/embeddings"):
                values = body.get("input", [])
                if not isinstance(values, list):
                    values = [values]
                response = {
                    "data": [
                        {"index": index, "embedding": vector(index)}
                        for index, _ in enumerate(values)
                    ]
                }
            elif path.endswith("/api/embed"):
                values = body.get("input", [])
                if not isinstance(values, list):
                    values = [values]
                response = {
                    "model": body.get("model", "mock-embed"),
                    "embeddings": [vector(index) for index, _ in enumerate(values)],
                }
            elif path.endswith("/rerank"):
                documents = body.get("documents", [])
                if not isinstance(documents, list):
                    documents = [documents]
                response = {
                    "results": [
                        {"index": index, "relevance_score": 1.0 - index * 0.1}
                        for index, _ in enumerate(documents)
                    ]
                }
            elif path.endswith("/api/generate"):
                prompt = body.get("prompt", "")
                response = {"model": body.get("model", "mock-ollama"),
                            "response": "mock:" + str(prompt),
                            "done": True}
            else:
                messages = body.get("messages", [])
                user_text = ""
                for message in messages:
                    if message.get("role") == "user":
                        user_text = str(message.get("content", ""))
                response = {
                    "choices": [
                        {"index": 0,
                         "message": {"role": "assistant", "content": "mock:" + user_text},
                         "finish_reason": "stop"}
                    ]
                }

            event = {
                "path": path,
                "model": body.get("model", ""),
                "authorization": self.headers.get("Authorization", ""),
                "body": body,
            }
            with lock:
                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + "\n")

            payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    mode = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT
    if mode == "start":
        start_server(port)
        print("mock_started_%d" % port)
    elif mode == "stop":
        stop_server(port)
        print("mock_stopped_%d" % port)
    elif mode == "serve":
        serve(port)
    else:
        raise SystemExit("unknown mode: " + mode)


if __name__ == "__main__":
    main()
