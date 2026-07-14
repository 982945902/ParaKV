# ParaKV Python integration tests

Black-box tests against a running ParaKV server. The C++ server uses
[brpc](https://github.com/apache/brpc), which auto-detects multiple wire
protocols on a single port — gRPC included — so a stock Python `grpcio`
client can talk to it without any extra configuration.

## Layout

```
test/
├── README.md
├── conftest.py                              # pytest fixtures (channel, stub, namespace)
├── kvcache_storage_service_pb2.py           # protoc-generated, do not edit
├── kvcache_storage_service_pb2_grpc.py      # protoc-generated, do not edit
├── protoc.sh                                # regenerate stubs from the .proto
├── requirements.txt
└── test_kvcache_storage_service.py          # BatchRead / BatchWrite tests
```

## Setup

```bash
cd test
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

## Regenerating stubs

Whenever `parakv/proto/kvcache_storage_service.proto` changes:

```bash
cd test
bash protoc.sh
```

## Running

1. Start the ParaKV server (built from the C++ tree):

   ```bash
   ./build/.../parakv/parakv --port=9200 --backend=memory
   ```

2. Point the tests at it and run:

   ```bash
   PARAKV_HOST=127.0.0.1 PARAKV_PORT=9200 pytest -v
   ```

`PARAKV_HOST` and `PARAKV_PORT` default to `127.0.0.1:9200` when unset.

## What's covered

| Test                                                         | Behaviour under test                                                |
|--------------------------------------------------------------|---------------------------------------------------------------------|
| `test_batch_write_then_read_roundtrip`                       | end-to-end Put/Get integrity, success/hit counters, result alignment |
| `test_batch_read_reports_not_found_for_missing_keys`         | missing keys return per-item `NOT_FOUND`, never omitted              |
| `test_overwrite_false_rejects_existing_key`                  | duplicate Put without `overwrite` returns `ALREADY_EXISTS`           |
| `test_overwrite_true_replaces_existing_key`                  | Put with `overwrite=true` replaces value + metadata                  |
| `test_include_value_false_returns_metadata_only`             | `include_value=false` probes existence without payload               |
| `test_namespaces_are_isolated`                               | same key in two namespaces stays independent                         |
| `test_empty_batch_write_returns_invalid_argument`            | request-level `INVALID_ARGUMENT` for empty input                     |
| `test_empty_batch_read_returns_invalid_argument`             | same, on the read path                                               |
| `test_empty_key_in_batch_is_per_item_error`                  | only the bad item is failed, others still succeed                    |
| `test_large_batch_roundtrip[1,16,256]`                       | parametrised stress over batch size                                  |
| `test_duplicate_keys_in_read_each_get_their_own_result`      | duplicate-key semantics in `BatchRead`                               |

Each test runs in its own UUID-suffixed namespace so the suite is safe to
re-run without restarting the server, and safe under `pytest -n auto` for
parallel execution.
