# AI evaluation gateway

`ai_gateway.py` is a standalone, dependency-free (Python 3 standard library)
HTTP gateway for evaluating seekdb AI functions and asynchronous AI columns
under faults. It is **not** part of the database transaction boundary: seekdb
calls it like any model endpoint, and it forwards traffic to the real or mock
upstream.

## Features

| Feature | Description |
| --- | --- |
| Transparent proxy | Method, path, query, body and headers (including `Authorization`) are forwarded unchanged to `--upstream`; the upstream status/body are returned unchanged. Credentials are never logged or stored. |
| Latency injection | `delay_ms` + random `jitter_ms` before forwarding. |
| Fault injection | `fail_rate`/`fail_status` (e.g. 429, 500, 503) answered by the gateway without calling upstream; `reset_rate` (connection closed, no response); `timeout_rate`/`timeout_ms` (hold the connection, then close). |
| Lost response | `lost_response_rate`: the upstream **is called and completes**, but the response is dropped. A client retry then causes a duplicated inference, which is counted. |
| Scripted faults | `script`: `{"<n>": "429" \| "503" \| "reset" \| "timeout" \| "lost" \| "fail"}` applies a fault to the n-th request (1-based) - deterministic scenarios. |
| Path filter | `fault_paths`: only requests whose path contains one of the substrings receive faults. |
| Usage/cost stats | requests, calls that really reached upstream, delivered responses, status histogram, injected faults, `usage.prompt_tokens/completion_tokens/total_tokens`, estimated cost from `pricing` (USD per 1K tokens per request model, `*` default), per-model breakdown, average latency, `duplicate_inferences`. Tokens of lost responses are still billed (they were consumed upstream). |

Random faults use a seeded RNG (`seed`), so runs are reproducible.

## Usage

```bash
# start the mock used by mysqltest (or any OpenAI compatible upstream)
python3 tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py start 19122
# start the gateway in front of it
python3 tools/ai_gateway/ai_gateway.py --upstream http://127.0.0.1:19122 --port 19130 &

# point an endpoint at the gateway
#   call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep', '{"ai_model_name":"m",
#     "url":"http://127.0.0.1:19130/v1/chat/completions", "access_key":"k", "provider":"openai"}');

# inject faults at runtime
curl -s -XPOST localhost:19130/_gateway/config -d '{"delay_ms":200,"fail_rate":0.1,"fail_status":429}'
curl -s -XPOST localhost:19130/_gateway/config -d '{"script":{"1":"lost","2":"timeout"},"timeout_ms":5000}'
curl -s -XPOST localhost:19130/_gateway/config -d '{"pricing":{"gpt-x":{"prompt":0.5,"completion":1.5}}}'

# statistics / reset
curl -s localhost:19130/_gateway/stats
curl -s -XPOST localhost:19130/_gateway/reset
```

Initial configuration can also be given as a JSON file with `--config`.

## Admin API

| Method | Path | Body / result |
| --- | --- | --- |
| GET | `/_gateway/stats` | statistics JSON |
| GET | `/_gateway/config` | current configuration |
| POST | `/_gateway/config` | partial configuration, merged (`pricing` merged per model) |
| POST | `/_gateway/reset` | reset statistics and request sequence |

## Tests

```bash
python3 tools/ai_gateway/test_gateway.py
```

The tests start an in-process **fake upstream** (OpenAI compatible, reports
`usage`) on an ephemeral port; no real model and no credential are needed.
They cover transparent forwarding (path, body, Authorization), upstream error
pass-through, latency, scripted and rate-based HTTP faults, connection reset,
timeout, lost response with duplicate-inference accounting, token/cost
statistics, path filtering, reset and concurrency.
