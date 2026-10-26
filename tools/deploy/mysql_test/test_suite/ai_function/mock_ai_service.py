#!/usr/bin/env python3
# Mock AI service for tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test.
# Listen on 0.0.0.0:8000 and serve the OpenAI, Ollama, and rerank payloads used by that test.
# Start: python3 tools/deploy/mysql_test/test_suite/ai_function/mock_ai_service.py

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def vector_for(text, dimension):
    if text == "BAD_DIM":
        values = [1]
    elif text == "a":
        values = [1, 0, 0]
    elif text == "b":
        values = [0, 1, 0]
    elif text == "c":
        values = [0, 0, 1]
    elif isinstance(text, str) and len(text) > 1 and text[0] == "r" and text[1:].isdigit():
        values = [int(text[1:]), 0, 0]
    else:
        values = [len(text) if isinstance(text, str) else 0, 0, 0]
    if dimension is None:
        dimension = 3
    if len(values) < dimension:
        values = values + [0] * (dimension - len(values))
    return values[:dimension]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def _read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length > 0 else b""
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def _send(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        try:
            request = self._read_json()
        except Exception:
            self._send(400, {"error": "invalid json"})
            return
        path = self.path.split("?", 1)[0]
        if path == "/v1/chat/completions":
            self._chat(request)
        elif path == "/v1/embeddings":
            self._embeddings(request)
        elif path == "/v1/rerank":
            self._rerank(request)
        elif path == "/api/generate":
            self._generate(request)
        elif path == "/api/embed":
            self._ollama_embed(request)
        else:
            self._send(404, {"error": "unknown path", "path": path})

    def _chat(self, request):
        messages = request.get("messages") or []
        content = ""
        if messages and isinstance(messages[-1], dict):
            content = messages[-1].get("content") or ""
        if content == "BAD_EMPTY":
            reply = ""
        else:
            reply = "ok:" + content
        if "temperature" in request or "max_tokens" in request:
            reply = "ok:%s|t=%s|m=%s" % (content, request.get("temperature"), request.get("max_tokens"))
        self._send(200, {"choices": [{"message": {"role": "assistant", "content": reply}}]})

    def _embeddings(self, request):
        inputs = request.get("input") or []
        if isinstance(inputs, str):
            inputs = [inputs]
        if any(item == "FAIL" for item in inputs):
            self._send(500, {"error": "injected embedding failure"})
            return
        dimension = request.get("dimensions")
        data = []
        for index, item in enumerate(inputs):
            data.append({"object": "embedding", "index": index, "embedding": vector_for(item, dimension)})
        data.reverse()
        self._send(200, {"data": data})

    def _rerank(self, request):
        documents = request.get("documents") or []
        results = []
        for index, document in enumerate(documents):
            score = 0
            if isinstance(document, str) and len(document) > 1 and document[0] in ("d", "s") and document[1:].isdigit():
                score = int(document[1:])
            results.append({"index": index, "relevance_score": score})
        results.reverse()
        self._send(200, {"results": results})

    def _generate(self, request):
        if request.get("stream") is not False:
            self._send(400, {"error": "stream must be false"})
            return
        prompt = request.get("prompt") or ""
        self._send(200, {"response": "ollama:" + prompt, "done": True})

    def _ollama_embed(self, request):
        inputs = request.get("input") or []
        if isinstance(inputs, str):
            inputs = [inputs]
        self._send(200, {"embeddings": [vector_for(item, None) for item in inputs]})


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 8000), Handler)
    server.serve_forever()
