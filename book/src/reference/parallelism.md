# Parallelism

A directory comparison runs in parallel in three different places, for three different reasons.
This page walks through each one and the settings that reach it.

```
   1. DISCOVERY          walking the folder trees
          │
          ▼
   2. FILE COMPARISON    many files at once, window width adapts
          │
          ▼
   3. CHUNK WORK         many chunks of one file at once
```

## 1. Discovery: walking the trees

Both sides are enumerated as a **breadth-first walk, one level at a time**. The directories at
each level are spread across a thread pool.

```
Level 0:  [root]                    1 directory, single-threaded
Level 1:  [dir1, dir2, dir3, ...]   N directories, in parallel
Level 2:  [sub1, sub2, sub3, ...]   M directories, in parallel
...
```

Each level is a barrier. Every directory at the current level finishes before the next level
starts.

**Why one level at a time, instead of just letting recursion run free?**

- **Memory stays bounded.** Only the current and next level are held, not the whole tree.
- **Progress is reportable.** You can say "level 3 of 6". A depth-first walk cannot.
- **Symlink loop detection stays cheap.** Canonical targets already visited go in a set behind a
  mutex. Only symlinks need tracking, because an ordinary directory cannot be its own ancestor.

| Setting | Default | Range |
|---------|---------|-------|
| `--parallel-discovery` / `--no-parallel-discovery` | on | |
| `--parallel-discovery-workers <N>` | 128 | 1 to 128 |

S3 listing is capped at **80 workers** whatever you ask for, to stay under request rate limits. A
higher value gets clamped down, with a log line saying so. Local scanning gets the full budget.

## 2. File comparison: the adaptive window

This is the interesting one.

Files are compared through a **sliding window**. The controller measures how fast work is
completing and adjusts how wide that window is.

```
┌──────────────────────────────────────────────────────┐
│  Thread pool, size = min(max_concurrency, num_files) │
└──────────────────────────────────────────────────────┘
                          ▲
                          │ the window bounds in-flight tasks
                          │
┌──────────────────────────────────────────────────────┐
│  Adaptive controller                                 │
│   start at initial_concurrency                       │
│   measure throughput every N completions             │
│   improved by >10%  → double the window              │
│   flat twice in a row → lock it                      │
└──────────────────────────────────────────────────────┘
```

**Why bother adapting?** Because the right concurrency for 100,000 tiny objects and the right
concurrency for four 50 GB objects differ by orders of magnitude. No single default serves both.

Too narrow, and an S3 comparison sits idle waiting on round trips. Too wide, and you run out of
threads, memory and request quota all at once.

So the controller walks toward the knee of the curve and stops when walking stops paying.

Where it starts depends on the shape of the work:

| Workload | `threads_per_file` | Initial window |
|----------|--------------------|----------------|
| All small files (single chunk) | 1 | `min(num_files, num_threads)` |
| All large files (multi-chunk) | `num_threads / num_files` | `num_files` |
| Mixed, more than 50% large | ~4 | `num_threads / 4` |
| Mixed, at most 50% large | 1 | `num_threads` |

| Setting | Default | Meaning |
|---------|---------|---------|
| `-t`, `--threads <N>` | 1024 | Total thread **budget**, not a fixed pool size |
| `-r`, `--ramp-up` | off | Start narrow and widen gradually. Helps when a cold DNS cache is the real bottleneck |

`--threads` is a ceiling, not a target. Real concurrency is `num_threads / threads_per_file`,
adapted downward from there by measurement.

### The file-descriptor ceiling above all of it

Before any of that, `--threads` gets clamped to what the process can actually open.

Each concurrent operation is budgeted **3 descriptors**, with **50** held back for everything
else. So the real ceiling is:

```
   (RLIMIT_NOFILE soft limit − 50) / 3
```

`mito` first tries to raise its own soft limit toward the hard limit. If that is still not
enough, it reduces the thread count and tells you, naming the `ulimit -n` value that would have
allowed what you asked for:

```text
Reducing threads from 1024 to 324 due to file descriptor limit.
To use 1024 threads, run: ulimit -n 3172
```

That example is a soft limit of 1024: `(1024 − 50) / 3 = 324`. And `1024 × 3 + 100 = 3172` to
lift it.

**This is the usual reason a `-t 1024` run behaves like a much smaller one.** A soft limit above
1,000,000 is treated as unlimited and nothing gets capped. The clamp applies to `diff`, `sync`
and `rm` alike.

## 3. Chunk work inside one file

Within a single large file, chunks are checksummed concurrently.

```
100 MB file, 8 MiB chunks:
┌─────────┬─────────┬─────────┬─────────┬─────────┐
│ Chunk 0 │ Chunk 1 │ Chunk 2 │ Chunk 3 │ Chunk 4 │
└────┬────┴────┬────┴────┬────┴────┬────┴────┬────┘
  Thread1   Thread2   Thread3   Thread4   Thread1
     ▼         ▼         ▼         ▼         ▼
   CRC32     CRC32     CRC32     CRC32     CRC32
```

This trades directly against level 2. More threads per file means one file finishes sooner and
fewer files are in flight. The `threads_per_file` table above is where that trade gets made.

## Choosing settings

The defaults are meant to work. If you are tuning anyway, these are reasonable starting points:

| Workload | Suggested |
|----------|-----------|
| Many small files, local | `-t 256 --parallel-discovery-workers 32` |
| Few large files, local | `-t 64 --parallel-discovery-workers 16` |
| S3 comparison | `-t 512 --parallel-discovery-workers 64` |
| Mixed local and S3 | `-t 512 --parallel-discovery-workers 64` |

## Costs worth keeping in mind

**Thread pools are not free.** They are created per enumeration:

| Pool size | Creation | Teardown |
|-----------|----------|----------|
| 32 threads | ~1 ms | ~5 ms |
| 128 threads | ~5 ms | ~20 ms |
| 500 threads | ~20 ms | ~100 ms |

Teardown costs more than creation, because stopping a pool means waking every idle thread so it
can notice the stop flag and exit.

**Stacks are reserved per thread**, typically 1 to 8 MB depending on the platform. So 128 workers
reserve somewhere between 128 MB and 1 GB of address space. Not necessarily committed memory, but
it is the practical reason the worker count is capped at 128.

## Cancellation

Every pool watches a shared atomic cancellation flag, checked both in the submission loop and
inside each worker.

Cancellation is **cooperative**. A task finishes the unit of work it is in before it notices. So
Ctrl-C during an 8 MiB chunk checksum returns after that chunk, not instantly.

That is on purpose: a half-written destination file is worse than a second of waiting. The exit
code is `130`.

## Synchronization

Condition variables for the sliding window and the level barriers. Mutexes around the shared
result collections and the visited-symlink set. Atomics for progress counters and the
cancellation flag.
