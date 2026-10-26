#!/usr/bin/env python3
"""Evaluation gateway for seekdb AI COLUMN / AI function traffic.

A transparent HTTP proxy placed between seekdb AI model endpoints and the real
(or mock) upstream.  It forwards requests unchanged and can inject:

  * latency (fixed + jitter) before forwarding,
  * HTTP faults (429 / 5xx / any status) without reaching the upstream,
  * connection resets (connection closed without a response),
  * timeouts (hold the connection longer than the client timeout),
  * "lost responses": the upstream is called and completes, but the response
    is dropped, so a client retry causes a duplicated inference.

It records usage statistics: requests seen, calls that actually reached the
upstream, status codes, token usage reported by the upstream (`usage` field of
OpenAI compatible responses) and an estimated cost from a configurable price
table.  Only standard library is used; no credentials are stored: the
Authorization header is forwarded as-is and never logged.

Admin API (JSON, same port, prefix /_gateway):
  GET  /_gateway/stats    -> statistics
  POST /_gateway/config   -> merge fault/latency/pricing configuration
  POST /_gateway/reset    -> reset statistics (and optionally config)
"""

import argparse
import json
import random
import socket
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ADMIN_PREFIX = "/_gateway"

DEFAULT_CONFIG = {
    # latency injected before forwarding (milliseconds)
    "delay_ms": 0,
    "jitter_ms": 0,
    # probability based faults, evaluated in this order; deterministic with seed
    "fail_rate": 0.0,          # answer fail_status without calling upstream
    "fail_status": 503,
    "reset_rate": 0.0,         # close connection without response
    "timeout_rate": 0.0,       # sleep timeout_ms then close
    "timeout_ms": 30000,
    "lost_response_rate": 0.0, # call upstream, then drop the response
    # sequence based faults: 1-based request numbers -> fault kind
    # e.g. {"1": "429", "3": "lost", "4": "reset", "5": "timeout"}
    "script": {},
    # only requests whose path contains one of these substrings are subject to
    # faults (empty = all)
    "fault_paths": [],
    "seed": 0,
    # USD per 1K tokens, keyed by request model name; "*" is the default
    "pricing": {"*": {"prompt": 0.0, "completion": 0.0}},
    "upstream_timeout_s": 60,
}


class GatewayState:
    def __init__(self, upstream, config=None):
        self.upstream = upstream.rstrip("/")
        self.lock = threading.Lock()
        self.config = json.loads(json.dumps(DEFAULT_CONFIG))
        if config:
            self.config.update(config)
        self.rng = random.Random(self.config.get("seed", 0))
        self.reset_stats()

    def reset_stats(self):
        with self.lock:
            self.stats = {
                "requests": 0,
                "upstream_calls": 0,
                "responses_delivered": 0,
                "injected": {"fail": 0, "reset": 0, "timeout": 0, "lost": 0},
                "status": {},
                "prompt_tokens": 0,
                "completion_tokens": 0,
                "total_tokens": 0,
                "cost": 0.0,
                "latency_ms_total": 0.0,
                "by_model": {},
                "by_path": {},
            }
            self.seq = 0

    def update_config(self, patch):
        with self.lock:
            for key, value in patch.items():
                if key == "pricing" and isinstance(value, dict):
                    self.config["pricing"].update(value)
                else:
                    self.config[key] = value
            if "seed" in patch:
                self.rng = random.Random(patch["seed"])

    def snapshot(self):
        with self.lock:
            out = json.loads(json.dumps(self.stats))
        delivered = max(out["responses_delivered"], 1)
        out["avg_latency_ms"] = out["latency_ms_total"] / delivered
        out["duplicate_inferences"] = max(
            0, out["upstream_calls"] - out["responses_delivered"]
            - out["injected"]["fail"])
        return out

    def next_fault(self, path):
        """Return (seq, fault) where fault is None or a fault kind string."""
        with self.lock:
            self.seq += 1
            seq = self.seq
            self.stats["requests"] += 1
            self.stats["by_path"][path] = self.stats["by_path"].get(path, 0) + 1
            cfg = self.config
            paths = cfg.get("fault_paths") or []
            if paths and not any(p in path for p in paths):
                return seq, None
            scripted = cfg.get("script", {}).get(str(seq))
            if scripted:
                return seq, str(scripted)
            draw = self.rng.random()
            acc = 0.0
            for kind, key in (("fail", "fail_rate"), ("reset", "reset_rate"),
                              ("timeout", "timeout_rate"), ("lost", "lost_response_rate")):
                acc += float(cfg.get(key, 0.0))
                if draw < acc:
                    return seq, kind
            return seq, None

    def delay_s(self):
        with self.lock:
            base = float(self.config.get("delay_ms", 0))
            jitter = float(self.config.get("jitter_ms", 0))
            extra = self.rng.uniform(0, jitter) if jitter > 0 else 0.0
        return (base + extra) / 1000.0

    def record(self, status, body_bytes, model, latency_ms, delivered, upstream_called, injected=None):
        usage = {}
        try:
            parsed = json.loads(body_bytes.decode("utf-8")) if body_bytes else {}
            if isinstance(parsed, dict) and isinstance(parsed.get("usage"), dict):
                usage = parsed["usage"]
        except (ValueError, UnicodeDecodeError):
            pass
        prompt = int(usage.get("prompt_tokens", 0) or 0)
        completion = int(usage.get("completion_tokens", 0) or 0)
        total = int(usage.get("total_tokens", prompt + completion) or 0)
        with self.lock:
            st = self.stats
            if upstream_called:
                st["upstream_calls"] += 1
                # tokens are consumed upstream even if the response is lost
                st["prompt_tokens"] += prompt
                st["completion_tokens"] += completion
                st["total_tokens"] += total
                price = self.config["pricing"].get(model) or self.config["pricing"].get("*", {})
                cost = (prompt * float(price.get("prompt", 0.0))
                        + completion * float(price.get("completion", 0.0))) / 1000.0
                st["cost"] += cost
                m = st["by_model"].setdefault(model or "", {"calls": 0, "tokens": 0, "cost": 0.0})
                m["calls"] += 1
                m["tokens"] += total
                m["cost"] += cost
            if injected:
                st["injected"][injected] += 1
            if status is not None:
                st["status"][str(status)] = st["status"].get(str(status), 0) + 1
            if delivered:
                st["responses_delivered"] += 1
                st["latency_ms_total"] += latency_ms


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def _send(self, status, payload, content_type="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def _read_body(self):
            size = int(self.headers.get("Content-Length", "0") or 0)
            return self.rfile.read(size) if size > 0 else b""

        def _admin(self, method, body):
            path = urlparse(self.path).path
            if method == "GET" and path == ADMIN_PREFIX + "/stats":
                return self._send(200, json.dumps(state.snapshot()).encode())
            if method == "GET" and path == ADMIN_PREFIX + "/config":
                with state.lock:
                    cfg = json.dumps(state.config).encode()
                return self._send(200, cfg)
            if method == "POST" and path == ADMIN_PREFIX + "/config":
                try:
                    patch = json.loads(body.decode("utf-8") or "{}")
                except ValueError:
                    return self._send(400, b'{"error":"invalid json"}')
                state.update_config(patch)
                return self._send(200, b'{"ok":true}')
            if method == "POST" and path == ADMIN_PREFIX + "/reset":
                state.reset_stats()
                return self._send(200, b'{"ok":true}')
            return self._send(404, b'{"error":"unknown admin path"}')

        def _abort(self):
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.close_connection = True

        def _proxy(self, method):
            body = self._read_body()
            path = urlparse(self.path).path
            if path.startswith(ADMIN_PREFIX):
                return self._admin(method, body)
            start = time.time()
            model = ""
            try:
                model = str(json.loads(body.decode("utf-8")).get("model", "")) if body else ""
            except (ValueError, UnicodeDecodeError, AttributeError):
                model = ""
            seq, fault = state.next_fault(path)
            delay = state.delay_s()
            if delay > 0:
                time.sleep(delay)
            if fault and fault.isdigit():
                payload = json.dumps({"error": {"message": "gateway injected %s" % fault}}).encode()
                state.record(int(fault), b"", model, (time.time() - start) * 1000, True, False, "fail")
                return self._send(int(fault), payload)
            if fault == "fail":
                status = int(state.config.get("fail_status", 503))
                payload = json.dumps({"error": {"message": "gateway injected failure"}}).encode()
                state.record(status, b"", model, (time.time() - start) * 1000, True, False, "fail")
                return self._send(status, payload)
            if fault == "reset":
                state.record(None, b"", model, 0, False, False, "reset")
                return self._abort()
            if fault == "timeout":
                state.record(None, b"", model, 0, False, False, "timeout")
                time.sleep(float(state.config.get("timeout_ms", 30000)) / 1000.0)
                return self._abort()
            # forward
            url = state.upstream + self.path
            headers = {k: v for k, v in self.headers.items()
                       if k.lower() not in ("host", "content-length", "connection")}
            req = urllib.request.Request(url, data=body if method != "GET" else None,
                                         headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=float(state.config.get("upstream_timeout_s", 60))) as resp:
                    status = resp.status
                    resp_body = resp.read()
                    content_type = resp.headers.get("Content-Type", "application/json")
            except urllib.error.HTTPError as err:
                status = err.code
                resp_body = err.read()
                content_type = err.headers.get("Content-Type", "application/json")
            except (urllib.error.URLError, OSError) as err:
                payload = json.dumps({"error": {"message": "upstream unreachable: %s" % err}}).encode()
                state.record(502, b"", model, (time.time() - start) * 1000, True, False)
                return self._send(502, payload)
            if fault == "lost":
                # upstream finished the inference, the caller never sees it
                state.record(status, resp_body, model, 0, False, True, "lost")
                return self._abort()
            state.record(status, resp_body, model, (time.time() - start) * 1000, True, True)
            return self._send(status, resp_body, content_type)

        def do_POST(self):
            self._proxy("POST")

        def do_GET(self):
            self._proxy("GET")

    return Handler


def create_server(upstream, host="127.0.0.1", port=0, config=None):
    state = GatewayState(upstream, config)
    server = ThreadingHTTPServer((host, port), make_handler(state))
    server.daemon_threads = True
    server.gateway_state = state
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--upstream", required=True, help="upstream base URL, e.g. http://127.0.0.1:19122")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19130)
    parser.add_argument("--config", help="JSON file with initial configuration")
    args = parser.parse_args()
    config = None
    if args.config:
        with open(args.config, "r", encoding="utf-8") as stream:
            config = json.load(stream)
    server = create_server(args.upstream, args.host, args.port, config)
    print("ai_gateway listening on %s:%d -> %s" % (args.host, server.server_address[1], args.upstream), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
