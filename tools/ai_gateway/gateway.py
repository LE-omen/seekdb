#!/usr/bin/env python3
"""Evaluation AI Gateway for seekdb AI Column testing.

Features:
- Transparent proxying of OpenAI/Ollama compatible HTTP requests.
- Latency injection (configurable delay).
- Fault injection (HTTP 429, 5xx, connection drops).
- Response drop simulation: Upstream completes request, but response is dropped
  to simulate lost response and test idempotency / repeated inference.
- Token usage & cost accounting.
- Dynamic control API for configuration and metrics retrieval.
"""

import argparse
import http.client
import json
import logging
import os
import signal
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

logger = logging.getLogger("ai_gateway")


class GatewayStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        with self.lock:
            self.client_requests = 0
            self.upstream_requests = 0
            self.successful_responses = 0
            self.failed_responses = 0
            self.dropped_responses = 0
            self.prompt_tokens = 0
            self.completion_tokens = 0
            self.total_tokens = 0
            self.estimated_cost_usd = 0.0

    def record_client_request(self):
        with self.lock:
            self.client_requests += 1

    def record_upstream_request(self):
        with self.lock:
            self.upstream_requests += 1

    def record_success(self, prompt_tokens=0, completion_tokens=0, cost_per_1k_prompt=0.0015, cost_per_1k_comp=0.002):
        with self.lock:
            self.successful_responses += 1
            self.prompt_tokens += prompt_tokens
            self.completion_tokens += completion_tokens
            self.total_tokens += (prompt_tokens + completion_tokens)
            cost = (prompt_tokens / 1000.0) * cost_per_1k_prompt + (completion_tokens / 1000.0) * cost_per_1k_comp
            self.estimated_cost_usd += cost

    def record_failure(self):
        with self.lock:
            self.failed_responses += 1

    def record_drop(self, prompt_tokens=0, completion_tokens=0, cost_per_1k_prompt=0.0015, cost_per_1k_comp=0.002):
        with self.lock:
            self.dropped_responses += 1
            self.prompt_tokens += prompt_tokens
            self.completion_tokens += completion_tokens
            self.total_tokens += (prompt_tokens + completion_tokens)
            cost = (prompt_tokens / 1000.0) * cost_per_1k_prompt + (completion_tokens / 1000.0) * cost_per_1k_comp
            self.estimated_cost_usd += cost

    def to_dict(self):
        with self.lock:
            return {
                "client_requests": self.client_requests,
                "upstream_requests": self.upstream_requests,
                "successful_responses": self.successful_responses,
                "failed_responses": self.failed_responses,
                "dropped_responses": self.dropped_responses,
                "prompt_tokens": self.prompt_tokens,
                "completion_tokens": self.completion_tokens,
                "total_tokens": self.total_tokens,
                "estimated_cost_usd": round(self.estimated_cost_usd, 6),
            }


class GatewayConfig:
    def __init__(self, upstream_url="http://127.0.0.1:19120"):
        self.lock = threading.Lock()
        self.upstream_url = upstream_url.rstrip("/")
        self.latency_seconds = 0.0
        self.inject_status_code = 0  # 0 means don't inject
        self.simulate_response_drop = False
        self.drop_next_n_responses = 0
        self.cost_per_1k_prompt = 0.0015
        self.cost_per_1k_completion = 0.0020

    def update(self, data):
        with self.lock:
            if "upstream_url" in data:
                self.upstream_url = data["upstream_url"].rstrip("/")
            if "latency_seconds" in data:
                self.latency_seconds = float(data["latency_seconds"])
            if "inject_status_code" in data:
                self.inject_status_code = int(data["inject_status_code"])
            if "simulate_response_drop" in data:
                self.simulate_response_drop = bool(data["simulate_response_drop"])
            if "drop_next_n_responses" in data:
                self.drop_next_n_responses = int(data["drop_next_n_responses"])
            if "cost_per_1k_prompt" in data:
                self.cost_per_1k_prompt = float(data["cost_per_1k_prompt"])
            if "cost_per_1k_completion" in data:
                self.cost_per_1k_completion = float(data["cost_per_1k_completion"])

    def to_dict(self):
        with self.lock:
            return {
                "upstream_url": self.upstream_url,
                "latency_seconds": self.latency_seconds,
                "inject_status_code": self.inject_status_code,
                "simulate_response_drop": self.simulate_response_drop,
                "drop_next_n_responses": self.drop_next_n_responses,
                "cost_per_1k_prompt": self.cost_per_1k_prompt,
                "cost_per_1k_completion": self.cost_per_1k_completion,
            }


def estimate_tokens_from_body(body_bytes):
    try:
        data = json.loads(body_bytes.decode("utf-8"))
        prompt_text = ""
        if "messages" in data:
            for m in data["messages"]:
                prompt_text += m.get("content", "") + " "
        elif "input" in data:
            inp = data["input"]
            if isinstance(inp, list):
                prompt_text = " ".join(str(x) for x in inp)
            else:
                prompt_text = str(inp)
        # 1 token ~= 4 chars rule of thumb
        return max(1, len(prompt_text) // 4)
    except Exception:
        return max(1, len(body_bytes) // 4)


def estimate_completion_tokens(body_bytes):
    try:
        data = json.loads(body_bytes.decode("utf-8"))
        if "usage" in data and "completion_tokens" in data["usage"]:
            return data["usage"]["completion_tokens"]
        if "choices" in data and len(data["choices"]) > 0:
            content = data["choices"][0].get("message", {}).get("content", "")
            return max(1, len(content) // 4)
        if "data" in data and isinstance(data["data"], list):
            # embeddings
            return len(data["data"]) * 4
    except Exception:
        pass
    return max(1, len(body_bytes) // 4)


def create_gateway_handler(stats: GatewayStats, config: GatewayConfig):
    class GatewayHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def handle_admin_requests(self):
            parsed = urlparse(self.path)
            if parsed.path == "/_metrics" and self.command == "GET":
                data = json.dumps(stats.to_dict()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return True
            if parsed.path == "/_reset" and self.command == "POST":
                stats.reset()
                data = json.dumps({"status": "reset_ok"}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return True
            if parsed.path == "/_config":
                if self.command == "GET":
                    data = json.dumps(config.to_dict()).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return True
                elif self.command == "POST":
                    size = int(self.headers.get("Content-Length", 0))
                    raw = self.rfile.read(size)
                    try:
                        update_data = json.loads(raw.decode("utf-8"))
                        config.update(update_data)
                        data = json.dumps({"status": "config_updated", "config": config.to_dict()}).encode("utf-8")
                        self.send_response(200)
                    except Exception as e:
                        data = json.dumps({"error": str(e)}).encode("utf-8")
                        self.send_response(400)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return True
            return False

        def do_GET(self):
            if self.handle_admin_requests():
                return
            self.proxy_request()

        def do_POST(self):
            if self.handle_admin_requests():
                return
            self.proxy_request()

        def proxy_request(self):
            stats.record_client_request()

            # Read client body
            content_len = int(self.headers.get("Content-Length", 0))
            client_body = self.rfile.read(content_len) if content_len > 0 else b""

            prompt_toks = estimate_tokens_from_body(client_body)

            # Check latency injection
            with config.lock:
                latency = config.latency_seconds
                inject_status = config.inject_status_code
                should_drop = config.simulate_response_drop
                if config.drop_next_n_responses > 0:
                    should_drop = True
                    config.drop_next_n_responses -= 1
                upstream_base = config.upstream_url
                p_cost = config.cost_per_1k_prompt
                c_cost = config.cost_per_1k_completion

            if latency > 0:
                time.sleep(latency)

            # Check fault injection (without calling upstream)
            if inject_status > 0:
                stats.record_failure()
                msg = json.dumps({"error": {"message": f"Injected fault with status {inject_status}"}}).encode("utf-8")
                self.send_response(inject_status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(msg)))
                self.end_headers()
                self.wfile.write(msg)
                return

            # Forward to upstream
            stats.record_upstream_request()
            parsed_upstream = urlparse(upstream_base)
            host = parsed_upstream.hostname
            port = parsed_upstream.port or (443 if parsed_upstream.scheme == "https" else 80)

            try:
                if parsed_upstream.scheme == "https":
                    conn = http.client.HTTPSConnection(host, port, timeout=30)
                else:
                    conn = http.client.HTTPConnection(host, port, timeout=30)

                # Forward headers
                req_headers = {}
                for h in self.headers:
                    if h.lower() not in ("host", "content-length"):
                        req_headers[h] = self.headers[h]
                req_headers["Host"] = f"{host}:{port}"
                req_headers["Content-Length"] = str(len(client_body))

                upstream_path = parsed_upstream.path.rstrip("/") + self.path
                conn.request(self.command, upstream_path, body=client_body, headers=req_headers)
                resp = conn.getresponse()
                resp_status = resp.status
                resp_body = resp.read()
                resp_headers = resp.getheaders()
                conn.close()

                comp_toks = estimate_completion_tokens(resp_body)

                # Check response drop simulation
                if should_drop:
                    # Upstream finished and did the inference work!
                    stats.record_drop(prompt_tokens=prompt_toks, completion_tokens=comp_toks,
                                      cost_per_1k_prompt=p_cost, cost_per_1k_comp=c_cost)
                    # Simulate drop by closing connection abruptly or returning 504 Gateway Timeout
                    err_msg = json.dumps({"error": {"message": "Upstream response dropped / lost"}}).encode("utf-8")
                    self.send_response(504)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(err_msg)))
                    self.end_headers()
                    self.wfile.write(err_msg)
                    return

                # Normal response
                if 200 <= resp_status < 300:
                    stats.record_success(prompt_tokens=prompt_toks, completion_tokens=comp_toks,
                                         cost_per_1k_prompt=p_cost, cost_per_1k_comp=c_cost)
                else:
                    stats.record_failure()

                self.send_response(resp_status)
                for h_name, h_val in resp_headers:
                    if h_name.lower() not in ("content-length", "transfer-encoding"):
                        self.send_header(h_name, h_val)
                self.send_header("Content-Length", str(len(resp_body)))
                self.end_headers()
                self.wfile.write(resp_body)

            except Exception as e:
                stats.record_failure()
                err_data = json.dumps({"error": {"message": f"Gateway proxy error: {str(e)}"}}).encode("utf-8")
                self.send_response(502)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(err_data)))
                self.end_headers()
                self.wfile.write(err_data)

    return GatewayHandler


class AIGatewayServer:
    def __init__(self, host="127.0.0.1", port=19130, upstream_url="http://127.0.0.1:19120"):
        self.host = host
        self.port = port
        self.stats = GatewayStats()
        self.config = GatewayConfig(upstream_url=upstream_url)
        handler_cls = create_gateway_handler(self.stats, self.config)
        self.httpd = ThreadingHTTPServer((host, port), handler_cls)
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        if self.thread:
            self.thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description="SeekDB AI Evaluation Gateway")
    parser.add_argument("--port", type=int, default=19130, help="Listen port")
    parser.add_argument("--upstream", type=str, default="http://127.0.0.1:19120", help="Upstream model endpoint")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Bind host")
    args = parser.parse_args()

    server = AIGatewayServer(host=args.host, port=args.port, upstream_url=args.upstream)
    print(f"AI Gateway listening on {args.host}:{args.port}, forwarding to {args.upstream}")
    try:
        server.httpd.serve_forever()
    except KeyboardInterrupt:
        server.stop()
        print("Gateway stopped.")


if __name__ == "__main__":
    main()
