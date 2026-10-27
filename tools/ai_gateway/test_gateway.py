#!/usr/bin/env python3
"""Automated tests for AI Evaluation Gateway."""

import json
import socket
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import request as urllib_request
from urllib.error import HTTPError

from gateway import AIGatewayServer


class MockUpstreamHandler(BaseHTTPRequestHandler):
    request_count = 0

    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        MockUpstreamHandler.request_count += 1
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len)
        try:
            req_data = json.loads(body.decode("utf-8"))
        except Exception:
            req_data = {}

        if self.path.endswith("/embeddings"):
            input_val = req_data.get("input", ["test"])
            if not isinstance(input_val, list):
                input_val = [input_val]
            response = {
                "data": [
                    {"index": i, "embedding": [0.1, 0.2, 0.3, 0.4]}
                    for i in range(len(input_val))
                ],
                "usage": {"prompt_tokens": 8, "total_tokens": 8}
            }
        else:
            response = {
                "choices": [{
                    "message": {
                        "role": "assistant",
                        "content": "mock_response"
                    }
                }],
                "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
            }

        data = json.dumps(response).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TestAIGateway(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.upstream_port = find_free_port()
        cls.gateway_port = find_free_port()

        cls.upstream = ThreadingHTTPServer(("127.0.0.1", cls.upstream_port), MockUpstreamHandler)
        import threading
        cls.upstream_thread = threading.Thread(target=cls.upstream.serve_forever, daemon=True)
        cls.upstream_thread.start()

        cls.gateway = AIGatewayServer(
            host="127.0.0.1",
            port=cls.gateway_port,
            upstream_url=f"http://127.0.0.1:{cls.upstream_port}"
        )
        cls.gateway.start()
        time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.gateway.stop()
        cls.upstream.shutdown()
        cls.upstream.server_close()

    def setUp(self):
        # Reset stats and config before each test
        self._post_admin("/_reset", {})
        self._post_admin("/_config", {
            "latency_seconds": 0.0,
            "inject_status_code": 0,
            "simulate_response_drop": False,
            "drop_next_n_responses": 0,
        })
        MockUpstreamHandler.request_count = 0

    def _post_admin(self, path, payload):
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}{path}",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib_request.urlopen(req) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def _get_metrics(self):
        req = urllib_request.Request(f"http://127.0.0.1:{self.gateway_port}/_metrics")
        with urllib_request.urlopen(req) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def test_01_transparent_proxy_completion(self):
        req_body = json.dumps({
            "model": "test-model",
            "messages": [{"role": "user", "content": "Hello world"}]
        }).encode("utf-8")
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions",
            data=req_body,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib_request.urlopen(req) as resp:
            self.assertEqual(resp.status, 200)
            data = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(data["choices"][0]["message"]["content"], "mock_response")

        metrics = self._get_metrics()
        self.assertEqual(metrics["client_requests"], 1)
        self.assertEqual(metrics["upstream_requests"], 1)
        self.assertEqual(metrics["successful_responses"], 1)
        self.assertEqual(metrics["failed_responses"], 0)
        self.assertGreater(metrics["total_tokens"], 0)
        self.assertGreater(metrics["estimated_cost_usd"], 0.0)

    def test_02_transparent_proxy_embedding(self):
        req_body = json.dumps({
            "model": "embed-model",
            "input": ["doc1", "doc2"]
        }).encode("utf-8")
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}/v1/embeddings",
            data=req_body,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib_request.urlopen(req) as resp:
            self.assertEqual(resp.status, 200)
            data = json.loads(resp.read().decode("utf-8"))
            self.assertEqual(len(data["data"]), 2)

        metrics = self._get_metrics()
        self.assertEqual(metrics["client_requests"], 1)
        self.assertEqual(metrics["upstream_requests"], 1)
        self.assertEqual(metrics["successful_responses"], 1)

    def test_03_latency_injection(self):
        self._post_admin("/_config", {"latency_seconds": 0.2})
        t0 = time.time()
        req_body = json.dumps({"messages": [{"role": "user", "content": "test"}]}).encode("utf-8")
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions",
            data=req_body,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib_request.urlopen(req) as resp:
            self.assertEqual(resp.status, 200)
        elapsed = time.time() - t0
        self.assertGreaterEqual(elapsed, 0.18)

    def test_04_fault_injection_429(self):
        self._post_admin("/_config", {"inject_status_code": 429})
        req_body = json.dumps({"messages": [{"role": "user", "content": "rate limit test"}]}).encode("utf-8")
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions",
            data=req_body,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with self.assertRaises(HTTPError) as ctx:
            urllib_request.urlopen(req)
        self.assertEqual(ctx.exception.code, 429)

        metrics = self._get_metrics()
        self.assertEqual(metrics["client_requests"], 1)
        self.assertEqual(metrics["upstream_requests"], 0)  # Upstream not called for fast injected fault
        self.assertEqual(metrics["failed_responses"], 1)

    def test_05_response_drop_simulation(self):
        # Simulates: Upstream has completed request and consumed tokens/compute, but response was dropped/lost in transit
        self._post_admin("/_config", {"simulate_response_drop": True})
        req_body = json.dumps({"messages": [{"role": "user", "content": "lost response test"}]}).encode("utf-8")
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions",
            data=req_body,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with self.assertRaises(HTTPError) as ctx:
            urllib_request.urlopen(req)
        self.assertEqual(ctx.exception.code, 504)

        metrics = self._get_metrics()
        self.assertEqual(metrics["client_requests"], 1)
        self.assertEqual(metrics["upstream_requests"], 1)  # Upstream WAS called!
        self.assertEqual(metrics["dropped_responses"], 1)
        # Even though client saw failure, upstream tokens and cost are counted!
        self.assertGreater(metrics["total_tokens"], 0)
        self.assertGreater(metrics["estimated_cost_usd"], 0.0)

    def test_06_metrics_reset(self):
        # Execute request
        req_body = json.dumps({"messages": [{"role": "user", "content": "test"}]}).encode("utf-8")
        req = urllib_request.Request(
            f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions",
            data=req_body,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        urllib_request.urlopen(req)
        metrics = self._get_metrics()
        self.assertGreater(metrics["client_requests"], 0)

        # Reset
        self._post_admin("/_reset", {})
        cleared = self._get_metrics()
        self.assertEqual(cleared["client_requests"], 0)
        self.assertEqual(cleared["upstream_requests"], 0)
        self.assertEqual(cleared["total_tokens"], 0)
        self.assertEqual(cleared["estimated_cost_usd"], 0.0)


if __name__ == "__main__":
    unittest.main()
