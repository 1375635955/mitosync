#!/usr/bin/env python3
"""Assert structural properties of the release workflow that no single job can check.

These are the properties that only make sense across jobs: whether the matrix and the
publish-side verification agree on the platform list, and whether a best-effort platform can
actually fail without withholding the release. Both are invisible to a YAML linter because
every file involved is individually valid.

Prints one `status|message` line per assertion and exits non-zero if any failed.
"""

import sys

import yaml

OPTIONAL_PLATFORM = "linux-aarch64"


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else ".github/workflows/release.yml"
    with open(path) as fh:
        wf = yaml.safe_load(fh)

    lines: list[tuple[str, str]] = []

    def check(condition: bool, good: str, bad: str) -> None:
        lines.append(("ok", good) if condition else ("fail", bad))

    # YAML 1.1 reads a bare `on` as the boolean True, so the key may be either.
    triggers = wf.get("on", wf.get(True, {})) or {}
    jobs = wf.get("jobs", {})
    build = jobs.get("build", {})
    publish = jobs.get("publish", {})

    # --- the dry-run path exists ------------------------------------------------
    inputs = (triggers.get("workflow_dispatch") or {}).get("inputs") or {}
    check(
        "dry_run" in inputs,
        "workflow_dispatch accepts dry_run",
        "workflow_dispatch has no dry_run input, so a release cannot be rehearsed",
    )

    # --- a best-effort platform must not block the release ----------------------
    # A matrix `needs` is satisfied only when every leg succeeds, so `needs: build` with no
    # `if:` makes every platform mandatory whatever the comments say.
    gate = str(publish.get("if") or "")
    check(
        "cancelled" in gate or "always" in gate,
        "publish survives a failed build leg (if: %s)" % (gate or "none"),
        "publish is gated on every matrix leg succeeding, so a failed or unscheduled "
        "%s build would withhold the whole release" % OPTIONAL_PLATFORM,
    )

    # --- the matrix and the verification agree ----------------------------------
    matrix = ((build.get("strategy") or {}).get("matrix") or {}).get("include") or []
    matrix_platforms = {entry["platform"] for entry in matrix if "platform" in entry}
    check(
        bool(matrix_platforms),
        "build matrix declares %d platforms" % len(matrix_platforms),
        "could not read any platforms from the build matrix",
    )

    check(
        (build.get("strategy") or {}).get("fail-fast") is False,
        "fail-fast is off, so one platform failing does not cancel the others",
        "fail-fast is not disabled; one platform failing would cancel the rest",
    )

    verify_step = next(
        (
            step
            for step in publish.get("steps") or []
            if "for p in" in str(step.get("run") or "")
        ),
        None,
    )
    if verify_step is None:
        lines.append(("fail", "found no artifact verification step in publish"))
    else:
        body = str(verify_step["run"])
        required = set()
        for line in body.splitlines():
            if "for p in" in line:
                required = set(
                    line.split("for p in", 1)[1].split(";", 1)[0].split()
                )
                break

        check(
            required <= matrix_platforms,
            "every required platform is in the build matrix",
            "publish requires %s, which the matrix does not build"
            % ", ".join(sorted(required - matrix_platforms)),
        )

        # Everything the matrix builds is either required or the one best-effort platform.
        # Anything else is built and then never verified by publish, which is how a platform
        # ships without its manifest ever being checked.
        unexpected = matrix_platforms - required - {OPTIONAL_PLATFORM}
        check(
            not unexpected,
            "%s is the only best-effort platform, and it is verified when present"
            % OPTIONAL_PLATFORM,
            "built but neither required nor handled as best-effort by publish: %s"
            % ", ".join(sorted(unexpected)),
        )
        check(
            OPTIONAL_PLATFORM in body,
            "publish handles %s explicitly" % OPTIONAL_PLATFORM,
            "publish never mentions %s, so a best-effort build would go unverified"
            % OPTIONAL_PLATFORM,
        )

    # --- the publish step must NOT mark releases as prereleases -----------------
    # Inverted deliberately. Marking 0.x as a prerelease looks right and breaks the install
    # path: /releases/latest skips prereleases, so with only prereleases published every
    # /releases/latest/download/... URL in the docs 404s.
    publish_step = next(
        (
            step
            for step in publish.get("steps") or []
            if "action-gh-release" in str(step.get("uses") or "")
        ),
        None,
    )
    if publish_step is None:
        lines.append(("fail", "found no release-publishing step in publish"))
    else:
        check(
            "prerelease" not in (publish_step.get("with") or {}),
            "the publish step does not mark prereleases, so /releases/latest resolves",
            "the publish step sets prerelease; /releases/latest skips prereleases, so every "
            "/releases/latest/download/... URL in the README, the book and install.sh will 404",
        )

    # --- both jobs validate the tag the same way -------------------------------
    for name, job in (("build", build), ("publish", publish)):
        bodies = " ".join(str(step.get("run") or "") for step in job.get("steps") or [])
        check(
            "-rc[0-9]+" in bodies,
            "%s accepts a -rcN candidate tag" % name,
            "%s does not accept a -rcN tag, so a rehearsal would be refused there" % name,
        )

    for status, message in lines:
        print("%s|%s" % (status, message))
    return 1 if any(status == "fail" for status, _ in lines) else 0


if __name__ == "__main__":
    sys.exit(main())
