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

"""Black-box tests for KVCacheStorageService.BatchRead / BatchWrite over gRPC.

Run against a live ParaKV server:

    cd test
    pip install -r requirements.txt
    PARAKV_HOST=127.0.0.1 PARAKV_PORT=9200 pytest -v
"""

from __future__ import annotations

import os
import uuid
from typing import Iterable, List, Tuple

import pytest


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _key(prefix: str, i: int) -> bytes:
    """16-byte key matching the typical xxh3_128 layout used by the engine."""
    return f"{prefix}-{i:013d}".encode("ascii")


def _value(i: int, size: int = 64) -> bytes:
    """Deterministic blob whose first bytes encode `i` for easy debugging."""
    head = f"v{i}|".encode("ascii")
    return (head + b"\xab" * size)[:size]


def _build_items(pb, prefix: str, n: int, value_size: int = 64,
                 ttl_ms: int = 0) -> List:
    items = []
    for i in range(n):
        items.append(
            pb.KVItem(
                key=_key(prefix, i),
                value=_value(i, value_size),
                ttl_ms=ttl_ms,
                metadata=f"meta-{i}".encode("ascii"),
            )
        )
    print(f"items: {items}")
    return items


def _write(stub, pb, namespace: str, items: Iterable, *,
           overwrite: bool = True, durable: bool = False):
    req = pb.BatchWriteRequest(
        request_id=uuid.uuid4().hex,
        namespace=namespace,
        items=list(items),
        overwrite=overwrite,
        durable=durable,
    )
    return stub.BatchWrite(req, timeout=10.0)


def _read(stub, pb, namespace: str, keys: Iterable[bytes], *,
          include_value: bool = True, touch_on_hit: bool = False):
    req = pb.BatchReadRequest(
        request_id=uuid.uuid4().hex,
        namespace=namespace,
        keys=list(keys),
        include_value=include_value,
        touch_on_hit=touch_on_hit,
    )
    return stub.BatchRead(req, timeout=10.0)


# ---------------------------------------------------------------------------
# BatchWrite + BatchRead happy path
# ---------------------------------------------------------------------------


def test_batch_write_then_read_roundtrip(stub, pb_module, namespace, background_service):
    pb = pb_module
    n = 32
    items = _build_items(pb, "rt", n)

    w = _write(stub, pb, namespace, items)
    assert w.code == pb.OK, w.message
    assert w.success_count == n
    assert w.failure_count == 0
    assert len(w.results) == n
    for item, result in zip(items, w.results):
        assert result.key == item.key
        assert result.code == pb.OK, result.message

    keys = [item.key for item in items]
    r = _read(stub, pb, namespace, keys)
    assert r.code == pb.OK
    assert r.hit_count == n
    assert r.miss_count == 0
    assert len(r.results) == n
    for item, result in zip(items, r.results):
        assert result.key == item.key
        assert result.code == pb.OK
        assert result.value == item.value
        assert result.metadata == item.metadata


# ---------------------------------------------------------------------------
# Per-key NOT_FOUND for misses; result order matches request order
# ---------------------------------------------------------------------------


# def test_batch_read_reports_not_found_for_missing_keys(stub, pb_module,
#                                                        namespace, background_service):
#     pb = pb_module
#     items = _build_items(pb, "miss", 3)
#     _write(stub, pb, namespace, items)

#     present_keys = [item.key for item in items]
#     missing_keys = [b"absent-1", b"absent-2"]
#     interleaved = [present_keys[0], missing_keys[0], present_keys[1],
#                    missing_keys[1], present_keys[2]]

#     r = _read(stub, pb, namespace, interleaved)
#     assert r.code == pb.OK
#     assert r.hit_count == 3
#     assert r.miss_count == 2
#     assert [res.key for res in r.results] == interleaved
#     codes = [res.code for res in r.results]
#     assert codes == [pb.OK, pb.NOT_FOUND, pb.OK, pb.NOT_FOUND, pb.OK]
#     for res in r.results:
#         if res.code == pb.NOT_FOUND:
#             assert res.value == b""


# ---------------------------------------------------------------------------
# Overwrite semantics
# ---------------------------------------------------------------------------


# def test_overwrite_false_rejects_existing_key(stub, pb_module, namespace, background_service):
#     pb = pb_module
#     items = _build_items(pb, "ow", 2)

#     w1 = _write(stub, pb, namespace, items, overwrite=False)
#     assert w1.success_count == 2

#     items2 = [
#         pb.KVItem(key=items[0].key, value=b"NEW-VALUE-0"),
#         pb.KVItem(key=items[1].key, value=b"NEW-VALUE-1"),
#     ]
#     w2 = _write(stub, pb, namespace, items2, overwrite=False)
#     assert w2.success_count == 0
#     assert w2.failure_count == 2
#     assert all(res.code == pb.ALREADY_EXISTS for res in w2.results)

#     r = _read(stub, pb, namespace, [items[0].key, items[1].key])
#     assert [res.value for res in r.results] == [items[0].value, items[1].value]


# def test_overwrite_true_replaces_existing_key(stub, pb_module, namespace, background_service):
#     pb = pb_module
#     items = _build_items(pb, "ow2", 2)
#     _write(stub, pb, namespace, items, overwrite=False)

#     new_items = [
#         pb.KVItem(key=items[0].key, value=b"NEW-0", metadata=b"m0"),
#         pb.KVItem(key=items[1].key, value=b"NEW-1", metadata=b"m1"),
#     ]
#     w = _write(stub, pb, namespace, new_items, overwrite=True)
#     assert w.success_count == 2
#     assert w.failure_count == 0

#     r = _read(stub, pb, namespace, [items[0].key, items[1].key])
#     assert r.results[0].value == b"NEW-0"
#     assert r.results[0].metadata == b"m0"
#     assert r.results[1].value == b"NEW-1"
#     assert r.results[1].metadata == b"m1"


# ---------------------------------------------------------------------------
# Read-side flags
# ---------------------------------------------------------------------------


# def test_include_value_false_returns_metadata_only(stub, pb_module, namespace, background_service):
#     pb = pb_module
#     items = _build_items(pb, "iv", 4)
#     _write(stub, pb, namespace, items)

#     r = _read(stub, pb, namespace, [item.key for item in items],
#               include_value=False)
#     assert r.hit_count == 4
#     for item, res in zip(items, r.results):
#         assert res.code == pb.OK
#         assert res.value == b"", "value should be omitted when include_value=False"
#         assert res.metadata == item.metadata


# ---------------------------------------------------------------------------
# Namespace isolation
# ---------------------------------------------------------------------------


# def test_namespaces_are_isolated(stub, pb_module, background_service):
#     pb = pb_module
#     ns_a = f"pytest-iso-a-{uuid.uuid4().hex[:8]}"
#     ns_b = f"pytest-iso-b-{uuid.uuid4().hex[:8]}"
#     shared_key = b"shared-key-0000"

#     _write(stub, pb, ns_a, [pb.KVItem(key=shared_key, value=b"in-A")])
#     _write(stub, pb, ns_b, [pb.KVItem(key=shared_key, value=b"in-B")])

#     ra = _read(stub, pb, ns_a, [shared_key])
#     rb = _read(stub, pb, ns_b, [shared_key])
#     assert ra.results[0].code == pb.OK and ra.results[0].value == b"in-A"
#     assert rb.results[0].code == pb.OK and rb.results[0].value == b"in-B"


# ---------------------------------------------------------------------------
# Empty / malformed batches
# ---------------------------------------------------------------------------


# def test_empty_batch_write_returns_invalid_argument(stub, pb_module, namespace, background_service):
#     pb = pb_module
#     resp = _write(stub, pb, namespace, [])
#     assert resp.code == pb.INVALID_ARGUMENT
#     assert resp.success_count == 0
#     assert resp.failure_count == 0
#     assert len(resp.results) == 0


# def test_empty_batch_read_returns_invalid_argument(stub, pb_module, namespace, background_service):
#     pb = pb_module
#     resp = _read(stub, pb, namespace, [])
#     assert resp.code == pb.INVALID_ARGUMENT
#     assert resp.hit_count == 0
#     assert resp.miss_count == 0
#     assert len(resp.results) == 0


# def test_empty_key_in_batch_is_per_item_error(stub, pb_module, namespace, background_service):
#     pb = pb_module
#     items = [
#         pb.KVItem(key=b"good-key-1", value=b"ok-1"),
#         pb.KVItem(key=b"", value=b"bad"),
#         pb.KVItem(key=b"good-key-2", value=b"ok-2"),
#     ]
#     w = _write(stub, pb, namespace, items)
#     assert w.code == pb.OK
#     assert w.success_count == 2
#     assert w.failure_count == 1
#     assert [res.code for res in w.results] == [pb.OK, pb.INVALID_ARGUMENT,
#                                                pb.OK]


# ---------------------------------------------------------------------------
# Stress / sanity
# ---------------------------------------------------------------------------


# @pytest.mark.parametrize("batch_size", [1, 16, 256])
# def test_large_batch_roundtrip(stub, pb_module, namespace, background_service, batch_size):
#     pb = pb_module
#     items = _build_items(pb, f"big-{batch_size}", batch_size, value_size=512)

#     w = _write(stub, pb, namespace, items)
#     assert w.success_count == batch_size

#     r = _read(stub, pb, namespace, [item.key for item in items])
#     assert r.hit_count == batch_size
#     for item, res in zip(items, r.results):
#         assert res.code == pb.OK
#         assert res.value == item.value


# def test_duplicate_keys_in_read_each_get_their_own_result(stub, pb_module,
#                                                           namespace, background_service):
#     pb = pb_module
#     items = _build_items(pb, "dup", 2)
#     _write(stub, pb, namespace, items)

#     keys = [items[0].key, items[0].key, items[1].key, b"absent-x",
#             items[0].key]
#     r = _read(stub, pb, namespace, keys)
#     assert len(r.results) == len(keys)
#     assert [res.key for res in r.results] == keys
#     assert r.hit_count == 4
#     assert r.miss_count == 1
