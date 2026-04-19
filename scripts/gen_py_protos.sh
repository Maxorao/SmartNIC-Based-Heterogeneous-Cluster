#!/bin/bash
# gen_py_protos.sh — Generate Python bindings for orchestrator_agent.proto
#
# orchestrator.py imports from proto/orchestrator_agent_pb2.py; this script
# uses grpc_tools.protoc to generate it alongside the proto.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO_DIR="$(realpath "${SCRIPT_DIR}/../proto")"
OUT_DIR="${PROTO_DIR}"

if ! python3 -c "import grpc_tools.protoc" 2>/dev/null; then
    echo "grpcio-tools not installed. Install with:"
    echo "  pip install grpcio grpcio-tools protobuf"
    exit 1
fi

mkdir -p "${OUT_DIR}"
# Ensure __init__.py so `from proto import ...` works
touch "${OUT_DIR}/__init__.py"

python3 -m grpc_tools.protoc \
    --proto_path="${PROTO_DIR}" \
    --python_out="${OUT_DIR}" \
    --grpc_python_out="${OUT_DIR}" \
    "${PROTO_DIR}/orchestrator_agent.proto"

# Also cluster.proto if you want Python access to cluster messages
python3 -m grpc_tools.protoc \
    --proto_path="${PROTO_DIR}" \
    --python_out="${OUT_DIR}" \
    --grpc_python_out="${OUT_DIR}" \
    "${PROTO_DIR}/cluster.proto"

echo ""
echo "Generated:"
ls -1 "${OUT_DIR}"/*_pb2*.py

echo ""
echo "Note: orchestrator.py uses 'from proto import orchestrator_agent_pb2'."
echo "Ensure ${PROTO_DIR%/proto} is on PYTHONPATH or run orchestrator from there."
