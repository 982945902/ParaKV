# Copyright 2026 The ParaKV Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://gitcode.com/xLLM-AI/ParaKV/blob/main/LICENSE
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# =============================================================================

"""Pytest fixtures for ParaKV KVCacheStorageService integration tests.

The tests target a running ParaKV server (the C++ brpc binary) reachable over
gRPC. Configure the endpoint via environment variables:

    PARAKV_HOST  default 127.0.0.1
    PARAKV_PORT  default 9200

The tests use unique namespaces per-test to stay isolated from each other and
from previous runs so they can be re-executed without restarting the server.
"""

from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid

import grpc
import pytest

# Make the generated *_pb2 / *_pb2_grpc modules importable regardless of the
# directory pytest was invoked from.
sys.path.insert(0, os.path.dirname(__file__))

import kvcache_storage_service_pb2 as pb  # noqa: E402
import kvcache_storage_service_pb2_grpc as pb_grpc  # noqa: E402


def _endpoint() -> str:
    host = os.environ.get("PARAKV_HOST", "127.0.0.1")
    port = os.environ.get("PARAKV_PORT", "9200")
    return f"{host}:{port}"


def _wait_ready(channel: grpc.Channel, timeout_s: float = 5.0) -> None:
    """Block until the channel reports READY or raise."""
    deadline = time.time() + timeout_s
    last_state = None
    while time.time() < deadline:
        try:
            grpc.channel_ready_future(channel).result(
                timeout=max(0.1, deadline - time.time())
            )
            return
        except grpc.FutureTimeoutError:
            last_state = channel.get_state(try_to_connect=True)
    raise RuntimeError(
        f"ParaKV server at {_endpoint()} not reachable "
        f"(last channel state: {last_state}). "
        f"Start it with `./parakv --port=9200` or set PARAKV_HOST/PARAKV_PORT."
    )

def _wait_ready_socket(host: str, port: int, timeout_s: float = 10.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            socket.create_connection((host, port), timeout=max(0.1, deadline - time.time()))
            return
        except socket.error:
            pass
    raise RuntimeError(f"ParaKV server at {host}:{port} not reachable.")


@pytest.fixture(scope="session")
def background_service():
    print("Starting ParaKV server...")
    proc = subprocess.Popen(["../../build/parakv/server/parakv", "--flagfile=conf/parakv.gflags"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    )
    _wait_ready_socket("127.0.0.1", 9200)

    yield {"host": "127.0.0.1", "port": 9200}

    try:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=30.0)
    except:
        proc.kill()
        proc.wait()

    # shutil.rmtree("logs", ignore_errors=True)
    # shutil.rmtree("segment_workspace", ignore_errors=True)


@pytest.fixture(scope="session")
def channel(background_service) -> grpc.Channel:
    ch = grpc.insecure_channel(
        _endpoint(),
        options=[
            ("grpc.max_send_message_length", 64 * 1024 * 1024),
            ("grpc.max_receive_message_length", 64 * 1024 * 1024),
        ],
    )
    _wait_ready(ch)
    yield ch
    ch.close()


@pytest.fixture(scope="session")
def stub(channel: grpc.Channel) -> pb_grpc.KVCacheStorageServiceStub:
    return pb_grpc.KVCacheStorageServiceStub(channel)


@pytest.fixture(params=[
    "pytest-ns-a", "pytest-ns-a", "pytest-ns-b", "random-ns"], 
    ids=["ns-a", "ns-a2", "ns-b", "random-ns"])
def namespace(request) -> str:
    """Per-test namespace so tests can run in parallel and be re-run safely."""
    if request.param == "random-ns":
        return f"pytest-{uuid.uuid4().hex[:12]}"

    return request.param


@pytest.fixture(scope="session")
def pb_module():
    return pb
