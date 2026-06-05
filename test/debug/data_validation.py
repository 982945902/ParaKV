# -*- coding: utf-8 -*-
"""Data validation: write a batch of KV pairs, then read back and verify.

Supports four modes for persistence testing across server restarts:

  1. roundtrip  - write + read-back in one shot (default)
  2. write      - write only; use before restarting the server
  3. verify     - read-only verification; use after restarting the server
  4. delete     - delete all generated keys

When --fixed is set, a deterministic prefix is used so the exact same keys and
values are generated across runs.  Typical persistence test workflow:

    # Step 1: write fixed data
    python data_validation.py --mode write --fixed --count 200

    # Step 2: restart the ParaKV server

    # Step 3: verify data survives the restart
    python data_validation.py --mode verify --fixed --count 200

    # Step 4: clean up
    python data_validation.py --mode delete --fixed --count 200
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pyclient"))

from client import Client, make_key, make_value, parse_value, VALUE_DIM
import kvcache_storage_service_pb2 as pb

FIXED_PREFIX = "dv-fixed"


def generate_pairs(prefix: str, count: int):
    """Return a list of (key, value) with deterministic content."""
    pairs = []
    for i in range(count):
        key = make_key(f"{prefix}-{i}")
        value = make_value(seed=i)
        pairs.append((key, value))
    return pairs


def do_write(client, pairs, batch_size):
    write_ok = 0
    write_fail = 0
    t0 = time.perf_counter()
    for start in range(0, len(pairs), batch_size):
        batch = pairs[start : start + batch_size]
        resp = client.batch_write(batch, overwrite=True)
        if resp.code != pb.OK:
            print(f"  [WRITE] batch starting at {start}: "
                  f"request-level error code={resp.code} msg={resp.message}")
        write_ok += resp.success_count
        write_fail += resp.failure_count
    elapsed = time.perf_counter() - t0
    print(f"Write done: ok={write_ok} fail={write_fail} "
          f"elapsed={elapsed:.3f}s")
    return write_fail


def do_verify(client, pairs, batch_size):
    keys = [k for k, _ in pairs]
    value_map = {k: v for k, v in pairs}

    hit = 0
    miss = 0
    mismatch = 0
    t0 = time.perf_counter()
    for start in range(0, len(keys), batch_size):
        batch_keys = keys[start : start + batch_size]
        resp = client.batch_read(batch_keys, include_value=True)
        if resp.code != pb.OK:
            print(f"  [READ] batch starting at {start}: "
                  f"request-level error code={resp.code} msg={resp.message}")

        for result in resp.results:
            if result.code != pb.OK:
                miss += 1
                print(f"  [MISS] key={result.key.hex()} "
                      f"code={result.code} msg={result.message}")
                continue

            expected = value_map.get(result.key)
            if expected is None:
                miss += 1
                print(f"  [ERROR] unexpected key {result.key.hex()} in response")
                continue

            if result.value == expected:
                hit += 1
            else:
                mismatch += 1
                exp_arr = np.frombuffer(expected, dtype=np.float32)
                got_arr = parse_value(result.value)
                diff = np.abs(exp_arr - got_arr)
                print(f"  [MISMATCH] key={result.key.hex()} "
                      f"max_diff={diff.max():.6e} "
                      f"expected_len={len(expected)} got_len={len(result.value)}")

    elapsed = time.perf_counter() - t0
    print(f"Read done:  hit={hit} miss={miss} mismatch={mismatch} "
          f"elapsed={elapsed:.3f}s")
    return hit, miss, mismatch


def do_delete(client, pairs, batch_size):
    keys = [k for k, _ in pairs]
    delete_ok = 0
    delete_fail = 0
    t0 = time.perf_counter()
    for start in range(0, len(keys), batch_size):
        batch_keys = keys[start : start + batch_size]
        resp = client.batch_delete(batch_keys)
        if resp.code != pb.OK:
            print(f"  [DELETE] batch starting at {start}: "
                  f"request-level error code={resp.code} msg={resp.message}")
        delete_ok += resp.success_count
        delete_fail += resp.failure_count
    elapsed = time.perf_counter() - t0
    print(f"Delete done: ok={delete_ok} fail={delete_fail} "
          f"elapsed={elapsed:.3f}s")
    return delete_fail


def run(args):
    client = Client(args.host, args.port, args.namespace)

    prefix = FIXED_PREFIX if args.fixed else f"dv-{int(time.time())}"
    pairs = generate_pairs(prefix, args.count)
    print(f"Generated {len(pairs)} key-value pairs "
          f"(prefix={prefix!r}, fixed={args.fixed}, value_dim={VALUE_DIM})")

    write_fail = 0
    delete_fail = 0
    hit, miss, mismatch = 0, 0, 0

    if args.mode in ("write", "roundtrip"):
        write_fail = do_write(client, pairs, args.batch_size)
        if write_fail > 0:
            print("[WARN] some writes failed; read-back may report mismatches")

    if args.mode in ("verify", "roundtrip"):
        hit, miss, mismatch = do_verify(client, pairs, args.batch_size)

    if args.mode == "delete":
        delete_fail = do_delete(client, pairs, args.batch_size)

    # ---- Summary ----
    total = len(pairs)
    print("=" * 60)
    if args.mode == "write":
        passed = write_fail == 0
        if passed:
            print(f"WRITE OK  {total} entries written successfully")
        else:
            print(f"WRITE FAILED  write_fail={write_fail}/{total}")
    elif args.mode == "verify":
        passed = miss == 0 and mismatch == 0 and hit == total
        if passed:
            print(f"VERIFY OK  {total}/{total} entries validated")
        else:
            print(f"VERIFY FAILED  hit={hit}/{total} miss={miss} "
                  f"mismatch={mismatch}")
    elif args.mode == "delete":
        passed = delete_fail == 0
        if passed:
            print(f"DELETE OK  {total} entries deleted successfully")
        else:
            print(f"DELETE FAILED  delete_fail={delete_fail}/{total}")
    else:
        passed = (write_fail == 0 and miss == 0
                  and mismatch == 0 and hit == total)
        if passed:
            print(f"PASSED  {total}/{total} entries validated successfully")
        else:
            print(f"FAILED  hit={hit}/{total} miss={miss} mismatch={mismatch} "
                  f"write_fail={write_fail}")
    print("=" * 60)

    client.close()
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description="ParaKV data validation")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9200)
    parser.add_argument("--namespace", default="default")
    parser.add_argument("--count", type=int, default=100,
                        help="number of KV pairs to write/validate")
    parser.add_argument("--batch_size", type=int, default=32,
                        help="items per batch request")
    parser.add_argument("--mode", choices=["roundtrip", "write", "verify", "delete"],
                        default="roundtrip",
                        help="roundtrip: write+verify; "
                             "write: write only; "
                             "verify: read-only verify; "
                             "delete: delete all generated keys")
    parser.add_argument("--fixed", action="store_true",
                        help="use a fixed prefix so the same data is "
                             "generated across runs (for persistence tests)")
    args = parser.parse_args()
    sys.exit(run(args))


if __name__ == "__main__":
    main()
