#!/usr/bin/env python3
"""End-to-end guard for issue #81.

CloudMetrics::save() writes to a temporary file and renames it over the real
one. That temporary used to be a fixed "<path>.tmp" shared by every mito
process on the machine, so two runs saving at the same time had one truncating
the file the other was about to rename into place. The result was either the
accumulated history wiped outright or an unparseable half-and-half file - the
damage issue #35 was about, reached by a different route.

Runs batches of mito processes concurrently against a stub S3 endpoint and
checks the seeded history is still intact afterwards.

This is a race detector, so it is one-sided: it never fails when the bug is
absent, but it does not catch it on every run either. Measured against the
shared-name version it tripped on roughly a third of runs at 8 rounds, which
is why it runs more of them now. At 50 rounds it caught the shared-name
version on every attempt measured.
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

EMPTY_UPLOADS = b"""<?xml version="1.0" encoding="UTF-8"?>
<ListMultipartUploadsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
<Bucket>b</Bucket><MaxUploads>1000</MaxUploads><IsTruncated>false</IsTruncated>
</ListMultipartUploadsResult>"""

# Only operation names CloudMetrics knows: it drops the ones it does not
# recognise on load, which would look like damage but is a separate defect.
SEEDED_OPS = ["HeadObject", "GetObject", "PutObject", "DeleteObject",
              "ListObjectsV2", "CopyObject", "GetBucketLocation"]
SEEDED_CALLS = 123456789
ROUNDS = 50
CONCURRENCY = 12


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


def history():
    op = {"bytes_down": 0, "bytes_server": 0, "bytes_up": 0,
          "calls": SEEDED_CALLS, "failures": 0, "retries": 0,
          "success": SEEDED_CALLS}
    return {"metrics": {name: dict(op) for name in SEEDED_OPS},
            "regions": ["us-east-1"],
            "saved_at": "2026-01-01T00:00:00Z",
            "version": 1}


def main():
    if len(sys.argv) != 2:
        print("usage: concurrent_metrics_saves.py <path-to-mito>", file=sys.stderr)
        return 2
    mito = sys.argv[1]

    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    failures = []
    try:
        with tempfile.TemporaryDirectory() as tmp:
            metrics = metrics_path(Path(tmp))
            metrics.parent.mkdir(parents=True)
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
            args = [mito, "leftovers", "s3://b/", "--region", "us-east-1",
                    "--endpoint-url", "http://127.0.0.1:%d" % port]

            for rnd in range(ROUNDS):
                metrics.write_text(json.dumps(history()))
                procs = [subprocess.Popen(args, env=env,
                                          stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL)
                         for _ in range(CONCURRENCY)]
                codes = []
                for p in procs:
                    try:
                        codes.append(p.wait(timeout=120))
                    except subprocess.TimeoutExpired:
                        p.kill()
                        codes.append("timeout")
                bad = [c for c in codes if c != 0]
                if bad:
                    failures.append("round %d: %d of %d runs did not succeed (%s)"
                                    % (rnd, len(bad), CONCURRENCY, sorted(set(map(str, bad)))))
                    break

                try:
                    data = json.loads(metrics.read_text())
                except ValueError as exc:
                    failures.append(
                        "round %d: the metrics file is not valid JSON (%s) - two "
                        "processes interleaved their writes" % (rnd, exc))
                    continue

                lost = [n for n in SEEDED_OPS
                        if data.get("metrics", {}).get(n, {}).get("calls") != SEEDED_CALLS]
                if lost:
                    failures.append(
                        "round %d: %d of %d seeded operations were lost (%s) - a "
                        "concurrent save destroyed the accumulated history"
                        % (rnd, len(lost), len(SEEDED_OPS), ", ".join(lost)))

                # A positive control. Every assertion above is about data
                # staying put, so all of them hold trivially against a binary
                # that never writes anything - /bin/true passed this test
                # before the check below existed.
                own = data.get("metrics", {}).get("ListMultipartUploads", {}).get("calls", 0)
                if not own:
                    failures.append(
                        "round %d: the concurrent runs recorded nothing at all, so "
                        "this round proved nothing about concurrent saves" % rnd)
                    break

                strays = [f.name for f in metrics.parent.iterdir() if ".tmp" in f.name]
                if strays:
                    failures.append("round %d: temporary files left behind: %s"
                                    % (rnd, strays))
    finally:
        server.shutdown()

    for f in failures:
        print("FAIL: %s" % f, file=sys.stderr)
    if failures:
        return 1
    print("ok: %d rounds of %d concurrent saves left the history intact"
          % (ROUNDS, CONCURRENCY))
    return 0


if __name__ == "__main__":
    sys.exit(main())
