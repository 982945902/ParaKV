# -*- coding: utf-8 -*-


import os
import sys
import time
import random
import threading
import multiprocessing
import queue
import logging
import traceback
import json
import re
import hashlib
from typing import List, Tuple

import grpc
import numpy as np
import kvcache_storage_service_pb2 as pb
import kvcache_storage_service_pb2_grpc as pb_grpc


# Key 固定为 16 字节定长二进制串 (xxh3_128 / md5 等 128bit 摘要的常见长度)
KEY_BYTES = 16
# Value 为 1024 维 float32 向量, 序列化后共 4096 字节
VALUE_DIM = 1024
VALUE_DTYPE = np.float32
VALUE_BYTES = VALUE_DIM * np.dtype(VALUE_DTYPE).itemsize


def make_key(s: str) -> bytes:
    """将任意字符串映射为 16 字节定长 key (使用 md5 摘要, 128bit)."""
    return hashlib.md5(s.encode("utf-8")).digest()


def make_value(seed: int = 0) -> bytes:
    """生成一个 1024 维 float32 向量, 并以小端字节序列化为 bytes."""
    rng = np.random.default_rng(seed)
    arr = rng.standard_normal(VALUE_DIM).astype(VALUE_DTYPE, copy=False)
    return arr.tobytes()


def parse_value(buf: bytes) -> np.ndarray:
    """将服务端返回的 bytes 反序列化为 1024 维 float32 ndarray."""
    arr = np.frombuffer(buf, dtype=VALUE_DTYPE)
    if arr.size != VALUE_DIM:
        raise ValueError(
            f"unexpected value dim: got {arr.size}, expected {VALUE_DIM}"
        )
    return arr


class Client:
    def __init__(self, host: str, port: int, namespace: str):
        self.host = host
        self.port = port
        self.channel = grpc.insecure_channel(f"{host}:{port}")
        self.stub = pb_grpc.KVCacheStorageServiceStub(self.channel)
        self.namespace = namespace

    def close(self):
        self.channel.close()

    @staticmethod
    def _check_kv(key: bytes, value: bytes):
        if not isinstance(key, (bytes, bytearray)) or len(key) != KEY_BYTES:
            raise ValueError(
                f"key must be {KEY_BYTES} bytes, got "
                f"{type(key).__name__} len={len(key) if hasattr(key, '__len__') else 'NA'}"
            )
        if not isinstance(value, (bytes, bytearray)) or len(value) != VALUE_BYTES:
            raise ValueError(
                f"value must be {VALUE_BYTES} bytes ({VALUE_DIM}*float32), "
                f"got len={len(value) if hasattr(value, '__len__') else 'NA'}"
            )

    def put(self, key: bytes, value: bytes, overwrite: bool = True, durable: bool = False):
        """单条写入, 内部走 BatchWrite."""
        return self.batch_write([(key, value)], overwrite=overwrite, durable=durable)

    def get(self, key: bytes, include_value: bool = True):
        """单条读取, 内部走 BatchRead."""
        return self.batch_read([key], include_value=include_value)

    def batch_write(
        self,
        key_value_pairs: List[Tuple[bytes, bytes]],
        overwrite: bool = True,
        durable: bool = False,
    ):
        items = []
        for k, v in key_value_pairs:
            self._check_kv(k, v)
            items.append(pb.KVItem(key=k, value=v))
        request = pb.BatchWriteRequest(
            namespace=self.namespace,
            items=items,
            overwrite=overwrite,
            durable=durable,
        )
        return self.stub.BatchWrite(request)

    def batch_read(self, keys: List[bytes], include_value: bool = True):
        for k in keys:
            if not isinstance(k, (bytes, bytearray)) or len(k) != KEY_BYTES:
                raise ValueError(f"key must be {KEY_BYTES} bytes, got len={len(k)}")
        request = pb.BatchReadRequest(
            namespace=self.namespace,
            keys=list(keys),
            include_value=include_value,
        )
        return self.stub.BatchRead(request)


if __name__ == "__main__":
    client = Client("localhost", 9200, "test")

    # 16 字节 key + 1024 维 float32 向量 value
    key1, key2 = make_key("key1"), make_key("key2")
    value1, value2 = make_value(seed=1), make_value(seed=2)

    assert len(key1) == KEY_BYTES and len(key2) == KEY_BYTES
    assert len(value1) == VALUE_BYTES and len(value2) == VALUE_BYTES

    put_resp = client.put(key1, value1)
    print(
        f"Put       : code={put_resp.code} "
        f"success={put_resp.success_count} failure={put_resp.failure_count}"
    )

    get_resp = client.get(key1)
    print(
        f"Get       : code={get_resp.code} "
        f"hit={get_resp.hit_count} miss={get_resp.miss_count}"
    )
    for r in get_resp.results:
        if r.code == pb.OK and r.value:
            arr = parse_value(r.value)
            print(f"  key={r.key.hex()} dim={arr.shape[0]} sample={arr[:4]}")
        else:
            print(f"  key={r.key.hex()} code={r.code} message={r.message}")

    bw_resp = client.batch_write([(key1, value1), (key2, value2)])
    print(
        f"BatchWrite: code={bw_resp.code} "
        f"success={bw_resp.success_count} failure={bw_resp.failure_count}"
    )

    br_resp = client.batch_read([key1, key2])
    print(
        f"BatchRead : code={br_resp.code} "
        f"hit={br_resp.hit_count} miss={br_resp.miss_count}"
    )
    for r in br_resp.results:
        if r.code == pb.OK and r.value:
            arr = parse_value(r.value)
            print(f"  key={r.key.hex()} dim={arr.shape[0]} sample={arr[:4]}")
        else:
            print(f"  key={r.key.hex()} code={r.code} message={r.message}")

    client.close()
