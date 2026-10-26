#!/usr/bin/env python3
"""Unit tests for ai_gateway.py using an in-process fake upstream.

No real model or credential is used.  Run:  python3 tools/ai_gateway/test_gateway.py
"""

import http.client
import json
import os
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_gateway  # noqa: E402


class FakeUpstream:
    """OpenAI compatible fake: echoes the user message and reports usage."""

    def __init__(self):
        self.calls = []
        self.lock = threading.Lock()
        outer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                return

            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(size).decode("utf-8") or "{}")
                with outer.lock:
                    outer.calls.append({"path": self.path, "body": body,
                                        "auth": self.headers.get("Authorization", "")})
                if self.path.endswith("/embeddings"):
                    resp = {"data": [{"index": 0, "embedding": [1.0, 2.0]}],
                            "usage": {"prompt_tokens": 3, "total_tokens": 3}}
                    status = 200
                elif "status500" in self.path:
                    resp = {"error": {"message": "upstream 500"}}
                    status = 500
                else:
                    text = body["messages"][-1]["content"]
                    resp = {"choices": [{"message": {"role": "assistant", "content": "echo:" + text}}],
                            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}}
                    status = 200
                payload = json.dumps(resp).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return "http://127.0.0.1:%d" % self.server.server_address[1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.upstream = FakeUpstream()
        self.gateway = ai_gateway.create_server(self.upstream.url, port=0)
        self.thread = threading.Thread(target=self.gateway.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.gateway.server_address[1]

    def tearDown(self):
        self.gateway.shutdown()
        self.gateway.server_close()
        self.upstream.close()

    def request(self, method, path, body=None, headers=None, timeout=5):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        data = json.dumps(body).encode() if body is not None else None
        hdrs = {"Content-Type": "application/json"}
        hdrs.update(headers or {})
        try:
            conn.request(method, path, body=data, headers=hdrs)
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

    def chat(self, text, model="m1", path="/v1/chat/completions", timeout=5):
        return self.request("POST", path, {"model": model, "messages": [
            {"role": "system", "content": "sys"}, {"role": "user", "content": text}]},
            headers={"Authorization": "Bearer fake-key"}, timeout=timeout)

    def config(self, patch):
        status, _ = self.request("POST", "/_gateway/config", patch)
        self.assertEqual(200, status)

    def stats(self):
        status, body = self.request("GET", "/_gateway/stats")
        self.assertEqual(200, status)
        return json.loads(body)

    def test_transparent_proxy(self):
        status, body = self.chat("hello")
        self.assertEqual(200, status)
        self.assertEqual("echo:hello", json.loads(body)["choices"][0]["message"]["content"])
        self.assertEqual(1, len(self.upstream.calls))
        call = self.upstream.calls[0]
        self.assertEqual("/v1/chat/completions", call["path"])
        self.assertEqual("Bearer fake-key", call["auth"])
        self.assertEqual("hello", call["body"]["messages"][-1]["content"])
        status, body = self.request("POST", "/v1/embeddings", {"model": "e", "input": "x"})
        self.assertEqual(200, status)
        self.assertEqual([1.0, 2.0], json.loads(body)["data"][0]["embedding"])

    def test_upstream_error_passthrough(self):
        status, body = self.chat("x", path="/v1/status500/completions")
        self.assertEqual(500, status)
        self.assertIn("upstream 500", body.decode())
        self.assertEqual(1, self.stats()["status"]["500"])

    def test_delay_injection(self):
        self.config({"delay_ms": 300})
        begin = time.time()
        status, _ = self.chat("slow")
        self.assertEqual(200, status)
        self.assertGreaterEqual(time.time() - begin, 0.3)

    def test_fault_injection_status(self):
        self.config({"script": {"1": "429", "2": "503"}})
        self.assertEqual(429, self.chat("a")[0])
        self.assertEqual(503, self.chat("b")[0])
        self.assertEqual(200, self.chat("c")[0])
        st = self.stats()
        self.assertEqual(3, st["requests"])
        self.assertEqual(1, st["upstream_calls"])  # injected faults never reach upstream
        self.assertEqual(2, st["injected"]["fail"])
        self.assertEqual(1, len(self.upstream.calls))

    def test_rate_based_fault_is_deterministic(self):
        self.config({"fail_rate": 1.0, "fail_status": 502})
        self.assertEqual(502, self.chat("a")[0])
        self.config({"fail_rate": 0.0})
        self.assertEqual(200, self.chat("b")[0])

    def test_connection_reset(self):
        self.config({"script": {"1": "reset"}})
        with self.assertRaises((http.client.HTTPException, ConnectionError, OSError)):
            self.chat("r")
        self.assertEqual(0, len(self.upstream.calls))
        self.assertEqual(1, self.stats()["injected"]["reset"])

    def test_timeout(self):
        self.config({"script": {"1": "timeout"}, "timeout_ms": 1500})
        with self.assertRaises((OSError, http.client.HTTPException)):
            self.chat("t", timeout=0.5)
        self.assertEqual(1, self.stats()["injected"]["timeout"])

    def test_lost_response_counts_duplicate_inference(self):
        self.config({"script": {"1": "lost"}})
        with self.assertRaises((http.client.HTTPException, ConnectionError, OSError)):
            self.chat("dup")
        # client retries the same request
        status, _ = self.chat("dup")
        self.assertEqual(200, status)
        self.assertEqual(2, len(self.upstream.calls))
        st = self.stats()
        self.assertEqual(2, st["upstream_calls"])
        self.assertEqual(1, st["responses_delivered"])
        self.assertEqual(1, st["injected"]["lost"])
        self.assertEqual(1, st["duplicate_inferences"])
        # tokens of the lost call are still billed
        self.assertEqual(30, st["total_tokens"])

    def test_usage_and_cost(self):
        self.config({"pricing": {"m1": {"prompt": 1.0, "completion": 2.0},
                                 "*": {"prompt": 0.5, "completion": 0.5}}})
        self.chat("a", model="m1")
        self.chat("b", model="m2")
        st = self.stats()
        self.assertEqual(20, st["prompt_tokens"])
        self.assertEqual(10, st["completion_tokens"])
        self.assertEqual(30, st["total_tokens"])
        # m1: (10*1 + 5*2)/1000 = 0.02 ; m2: (10*0.5 + 5*0.5)/1000 = 0.0075
        self.assertAlmostEqual(0.0275, st["cost"], places=6)
        self.assertEqual(1, st["by_model"]["m1"]["calls"])
        self.assertAlmostEqual(0.02, st["by_model"]["m1"]["cost"], places=6)

    def test_fault_paths_filter(self):
        self.config({"fail_rate": 1.0, "fault_paths": ["/embeddings"]})
        self.assertEqual(200, self.chat("ok")[0])
        status, _ = self.request("POST", "/v1/embeddings", {"model": "e", "input": "x"})
        self.assertEqual(503, status)

    def test_reset_stats(self):
        self.chat("a")
        self.assertEqual(1, self.stats()["requests"])
        self.assertEqual(200, self.request("POST", "/_gateway/reset", {})[0])
        self.assertEqual(0, self.stats()["requests"])

    def test_concurrent_requests(self):
        results = []

        def worker(i):
            results.append(self.chat("c%d" % i)[0])
        threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual([200] * 16, results)
        self.assertEqual(16, self.stats()["upstream_calls"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
