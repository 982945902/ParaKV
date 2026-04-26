#!/usr/bin/env bash
# Regenerate Python protobuf + gRPC stubs from the canonical .proto.
# Re-run this whenever parakv/proto/kvcache_storage_service.proto changes.
set -euo pipefail

cd "$(dirname "$0")"

python -m grpc_tools.protoc \
  -I ../../parakv/proto \
  --python_out=. \
  --grpc_python_out=. \
  kvcache_storage_service.proto

echo "Generated:"
ls -1 kvcache_storage_service_pb2.py kvcache_storage_service_pb2_grpc.py
