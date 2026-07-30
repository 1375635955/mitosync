#!/usr/bin/env bash
#
# Start, stop, or describe a local S3-compatible endpoint for the integration
# tests in tests/test_s3_integration.cpp.
#
# Those tests skip themselves unless MITO_TEST_S3_ENDPOINT is set, so a plain
# `ctest` on a machine without Docker stays green. To run them:
#
#   scripts/s3-emulator.sh start
#   eval "$(scripts/s3-emulator.sh env)"
#   ctest --test-dir build -L s3-integration
#   scripts/s3-emulator.sh stop
#
# MinIO is the endpoint here because it has earned it: it rejected two requests
# that MockS3Client accepted (issues #98 and #99), which is the entire reason
# these tests exist. Garage is lighter and equally strict about the multipart
# checksum contract, but mitosync cannot currently upload to it (issue #100).
set -euo pipefail

NAME="${MITO_S3_EMULATOR_NAME:-mito-s3-emulator}"
PORT="${MITO_S3_EMULATOR_PORT:-9010}"
IMAGE="${MITO_S3_EMULATOR_IMAGE:-minio/minio:latest}"
KEY="minioadmin"
SECRET="minioadmin"

usage() {
    cat <<USAGE
usage: $(basename "$0") {start|stop|env|status}

  start   run the emulator (no-op if already running)
  stop    remove it
  env     print the exports the tests need; use with: eval "\$(... env)"
  status  report whether it is up

environment:
  MITO_S3_EMULATOR_PORT   host port to bind (default ${PORT})
  MITO_S3_EMULATOR_IMAGE  container image (default ${IMAGE})
  MITO_S3_EMULATOR_NAME   container name (default ${NAME})
USAGE
}

is_running() {
    [ -n "$(docker ps --filter "name=^${NAME}$" --format '{{.Names}}' 2>/dev/null)" ]
}

case "${1:-}" in
start)
    if is_running; then
        echo "already running on port ${PORT}" >&2
        exit 0
    fi
    command -v docker >/dev/null 2>&1 || { echo "docker is required" >&2; exit 1; }
    docker run -d --rm --name "${NAME}" \
        -p "${PORT}:9000" \
        -e "MINIO_ROOT_USER=${KEY}" \
        -e "MINIO_ROOT_PASSWORD=${SECRET}" \
        "${IMAGE}" server /data >/dev/null

    # Wait for readiness rather than sleeping a guessed interval: the tests fail
    # confusingly against a server that is listening but not yet serving.
    for _ in $(seq 1 60); do
        if curl -sf "http://localhost:${PORT}/minio/health/live" >/dev/null 2>&1; then
            echo "ready on http://localhost:${PORT}" >&2
            exit 0
        fi
        sleep 1
    done
    echo "did not become ready within 60s" >&2
    docker logs "${NAME}" 2>&1 | tail -20 >&2
    exit 1
    ;;
stop)
    docker rm -f "${NAME}" >/dev/null 2>&1 || true
    echo "stopped" >&2
    ;;
env)
    echo "export MITO_TEST_S3_ENDPOINT=http://localhost:${PORT}"
    echo "export MITO_TEST_S3_REGION=us-east-1"
    echo "export AWS_ACCESS_KEY_ID=${KEY}"
    echo "export AWS_SECRET_ACCESS_KEY=${SECRET}"
    echo "export AWS_DEFAULT_REGION=us-east-1"
    # Without this the SDK probes IMDS on every client build, costing seconds
    # per test on anything that is not an EC2 instance.
    echo "export AWS_EC2_METADATA_DISABLED=true"
    ;;
status)
    if is_running; then echo "running on port ${PORT}"; else echo "not running"; fi
    ;;
*)
    usage
    exit 1
    ;;
esac
