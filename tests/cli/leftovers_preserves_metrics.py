#!/usr/bin/env python3
"""End-to-end guard for issue #35.

`mito leftovers` saved cumulative cloud metrics without loading them first, so
it replaced the user's accumulated S3 usage and cost history with just that one
run. Nothing in the gtest suite can catch a regression here: main.cpp is only
compiled into the mito executable, and run_leftovers_command is static. So this
drives the real binary.

It stands up a stub S3 endpoint that answers ListMultipartUploads with an empty
result, points the metrics file at a scratch directory, and checks that a
previous run's numbers are still there afterwards - and that the new run's
calls were added to them rather than replacing them.
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

EMPTY_UPLOADS = b"""<?xml version="1.0" encoding="UTF-8"?>
<ListMultipartUploadsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
<Bucket>b</Bucket><KeyMarker></KeyMarker><UploadIdMarker></UploadIdMarker>
<NextKeyMarker></NextKeyMarker><NextUploadIdMarker></NextUploadIdMarker>
<MaxUploads>1000</MaxUploads><IsTruncated>false</IsTruncated>
</ListMultipartUploadsResult>"""

SEEDED_CALLS = 4242


def metrics_path(root):
    """Where the binary writes cloud_metrics.json, given HOME and XDG_DATA_HOME set to root.

    Mirrors GetAppDataDirectory() in src/app_settings.cpp. The macOS branch ignores
    XDG_DATA_HOME entirely and derives the path from $HOME, so the layout differs by
    platform and hardcoding the Linux one makes this test seed a file the binary never
    reads (which is exactly how it failed on the macos-arm64 release builder).
    """
    if sys.platform == "darwin":
        return root / "Library" / "Application Support" / "MitoSync" / "cloud_metrics.json"
    return root / "mitosync" / "cloud_metrics.json"


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(EMPTY_UPLOADS)))
        self.end_headers()
        self.wfile.write(EMPTY_UPLOADS)

    def log_message(self, *args):
        pass


def seed(path):
    """A history left by previous runs, in the format the app writes."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({
        "metrics": {
            "HeadObject": {
                "bytes_down": 0, "bytes_server": 0, "bytes_up": 0,
                "calls": SEEDED_CALLS, "failures": 0, "retries": 0,
                "success": SEEDED_CALLS,
            }
        },
        "regions": ["ap-south-1"],
        "saved_at": "2026-01-01T00:00:00Z",
        "version": 1,
    }))


def main():
    if len(sys.argv) != 2:
        print("usage: leftovers_preserves_metrics.py <path-to-mito>", file=sys.stderr)
        return 2
    mito = sys.argv[1]

    from pathlib import Path

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    failures = []
    try:
        with tempfile.TemporaryDirectory() as tmp:
            metrics = metrics_path(Path(tmp))
            seed(metrics)

            env = dict(os.environ)
            env.update({
                # HOME as well as XDG_DATA_HOME: the macOS branch of
                # GetAppDataDirectory ignores XDG_DATA_HOME entirely and uses
                # $HOME/Library/Application Support, so redirecting only the
                # latter would point these runs at the developer's real
                # metrics file (issue #41).
                "HOME": tmp,
                "XDG_DATA_HOME": tmp,
                "AWS_EC2_METADATA_DISABLED": "true",
                "AWS_ACCESS_KEY_ID": "test",
                "AWS_SECRET_ACCESS_KEY": "test",
            })

            for run in (1, 2):
                proc = subprocess.run(
                    [mito, "leftovers", "s3://b/", "--region", "us-east-1",
                     "--endpoint-url", "http://127.0.0.1:%d" % port],
                    env=env, capture_output=True, text=True, timeout=120,
                )
                if proc.returncode != 0:
                    failures.append("run %d exited %d\nstdout: %s\nstderr: %s"
                                    % (run, proc.returncode, proc.stdout, proc.stderr))
                    break

                data = json.loads(metrics.read_text())
                ops = data.get("metrics", {})

                head = ops.get("HeadObject", {}).get("calls")
                if head != SEEDED_CALLS:
                    failures.append(
                        "after run %d the accumulated history is %r, expected %d "
                        "(issue #35: leftovers saved without loading first)"
                        % (run, head, SEEDED_CALLS))

                listed = ops.get("ListMultipartUploads", {}).get("calls")
                if listed != run:
                    failures.append(
                        "after run %d this command's own calls are %r, expected %d "
                        "- they must accumulate, not replace"
                        % (run, listed, run))

                regions = data.get("regions", [])
                if "ap-south-1" not in regions:
                    failures.append(
                        "after run %d the recorded regions are %r; the history's "
                        "region was dropped, which reprices past usage"
                        % (run, regions))
    finally:
        server.shutdown()

    for f in failures:
        print("FAIL: %s" % f, file=sys.stderr)
    if failures:
        return 1
    print("ok: accumulated metrics preserved and appended across 2 runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
