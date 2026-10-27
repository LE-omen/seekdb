# SeekDB AI Evaluation Gateway (评测网关)

The SeekDB AI Evaluation Gateway is a lightweight, high-performance transparent proxy designed for the 6th OceanBase Database Competition (2026) AI Column track. It intercepts and monitors communication between the SeekDB database engine and upstream AI model providers (OpenAI, Ollama, vLLM, and Mock AI servers).

## Features

1. **Transparent Proxying**:
   - Supports `/v1/chat/completions` and `/v1/embeddings` APIs.
   - Forwards headers (`Authorization`, `Content-Type`), query parameters, and JSON payloads seamlessly.

2. **Fault & Chaos Injection**:
   - **Latency Injection**: Simulates network latency and model generation delays (`latency_seconds`).
   - **HTTP Fault Injection**: Simulates HTTP 429 (Rate Limit / Quota Exceeded), HTTP 500, HTTP 503, or custom error codes.
   - **Lost Response Simulation ("上游已完成但响应丢失")**:
     Forwards the request to the upstream AI service so the model performs actual inference (and consumes compute / tokens), but drops or returns HTTP 504 Gateway Timeout to the client. This allows evaluating SeekDB's retry idempotency and preventing duplicate inference issues.

3. **Usage & Cost Accounting**:
   - Tracks total client requests vs. actual requests reaching upstream.
   - Counts prompt tokens, completion tokens, and total tokens.
   - Calculates estimated model inference costs in USD based on configurable pricing models.

4. **Dynamic Administration & Control**:
   - `GET /_metrics`: Returns real-time metrics in JSON.
   - `POST /_config`: Dynamically updates latency, fault injection parameters, and pricing.
   - `POST /_reset`: Resets counters and statistics between test scenarios.

## Usage

### Starting the Gateway

```bash
python tools/ai_gateway/gateway.py --port 19130 --upstream http://127.0.0.1:19120
```

Arguments:
- `--port`: Listen port (default: `19130`).
- `--upstream`: Upstream AI model endpoint (default: `http://127.0.0.1:19120`).
- `--host`: Bind address (default: `127.0.0.1`).

### Running Tests

```bash
python tools/ai_gateway/test_gateway.py
```

All tests run self-contained with Python standard library.

### Control API Examples

#### 1. Retrieve Current Metrics
```bash
curl http://127.0.0.1:19130/_metrics
```
Response:
```json
{
  "client_requests": 10,
  "upstream_requests": 8,
  "successful_responses": 7,
  "failed_responses": 2,
  "dropped_responses": 1,
  "prompt_tokens": 1280,
  "completion_tokens": 340,
  "total_tokens": 1620,
  "estimated_cost_usd": 0.0026
}
```

#### 2. Inject Fault (e.g. HTTP 429 Rate Limit)
```bash
curl -X POST http://127.0.0.1:19130/_config \
  -H "Content-Type: application/json" \
  -d '{"inject_status_code": 429}'
```

#### 3. Inject Latency (e.g. 500ms delay)
```bash
curl -X POST http://127.0.0.1:19130/_config \
  -H "Content-Type: application/json" \
  -d '{"latency_seconds": 0.5}'
```

#### 4. Simulate Dropped Response After Completion
```bash
curl -X POST http://127.0.0.1:19130/_config \
  -H "Content-Type: application/json" \
  -d '{"simulate_response_drop": true}'
```

#### 5. Reset Counters
```bash
curl -X POST http://127.0.0.1:19130/_reset
```
