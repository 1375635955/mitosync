# Parallelization Architecture

MitoSync does not use one giant "do everything faster" thread pool. It uses a
few separate lanes, each matched to the kind of waiting involved:

- directory and S3 prefix discovery wait on filesystem and listing calls
- file comparison waits on local reads, S3 checksum requests, and CRC work
- sync waits on uploads, downloads, server-side copies, local writes, and deletes
- `rm` waits on S3 delete calls

The common pattern is bounded parallelism: create a pool, keep only a controlled
number of jobs in flight, and adjust that number when the service starts pushing
back or throughput stops improving.

```
                   +----------------------+
                   | user command         |
                   | diff / sync / rm     |
                   +----------+-----------+
                              |
             +----------------+----------------+
             |                                 |
             v                                 v
      discovery pools                    execution pools
  local dirs / S3 prefixes          compare / transfer / delete
             |                                 |
             +----------------+----------------+
                              |
                              v
                     progress counters
```

## One Page Summary

| Area | Main code | Parallelism model |
|------|-----------|-------------------|
| Local directory discovery | `parallel_enumerate_local_directory()` in `src/directory_comparison.cpp` | Breadth-first directory levels, worker pool per enumeration |
| S3 prefix discovery | `parallel_enumerate_s3_prefix()` in `src/directory_comparison.cpp` | Breadth-first prefix levels, worker pool per enumeration, S3 worker cap |
| Directory diff file comparison | `run_directory_comparison()` in `src/directory_comparison.cpp` | Sliding window over common files, adaptive file concurrency |
| Local chunk CRC | `compute_crc32_chunks_boost_asio()` in `src/crc32_chunks.cpp` | Sequential for <= 4 chunks, thread pool for more chunks |
| S3 multipart checksum requests | `S3MultipartCopy::ParallelUploadPartCopyRequestsThreadPool()` in `src/s3_utils.cpp` | Sliding window over chunk requests, backoff on errors, ramp-up on success |
| Sync execution | `run_sync()` in `src/sync_task.cpp` | Sliding window over actionable files, plus prefetch and disk-writer pipelines for downloads |
| Recursive rm | `run_rm()` in `src/rm_task.cpp` | Sliding window over delete jobs or delete batches |

## Core Vocabulary

**Thread pool**: A fixed set of worker threads. Jobs are posted to it.

**In flight**: Jobs that have been submitted but have not finished yet.

**Sliding window**: The submitter only posts a new job when `in_flight` is below
the current limit.

```
max allowed in flight = 4

submitted:  [A] [B] [C] [D]   wait
finished:       B
submitted:  [A] [C] [D] [E]   wait
```

**Adaptive concurrency**: The current limit changes while the command runs.
MitoSync increases concurrency when work succeeds and reduces it when failures
look like service pressure.

## Discovery

Discovery means turning a directory or S3 prefix into a list of `DirectoryEntry`
values.

```
DirectoryEntry:
  relative_path
  size
  mtime
```

Directory comparison enumerates both sources at the same time. Sync also
enumerates source and destination before planning work.

```
source A discovery  ----+
                        +--> compare/build plan
source B discovery  ----+
```

### Local Directory Discovery

Local recursive discovery uses breadth-first search. It processes one directory
level at a time.

```
level 0:
  /

level 1:
  /a        /b        /c

level 2:
  /a/1      /a/2      /b/1      /c/1
```

Each level is a batch. If the level has one directory, it is processed directly.
If it has multiple directories, the code posts them to a `boost::asio::thread_pool`.

```
current_level = [a, b, c, d]

          +--> worker 1 scans a
thread    +--> worker 2 scans b
pool      +--> worker 3 scans c
          +--> worker 4 scans d

wait until pending_tasks == 0
merge files
move to next_level
```

Important details:

- local discovery clamps workers to `1..128`
- recursive local discovery uses the BFS implementation even when configured
  with one worker, because the BFS path has better error visibility than
  `recursive_directory_iterator`
- unreadable directories and real stat failures mark the listing incomplete
- symlink targets are tracked to avoid cycles
- duplicate symlink targets are reported separately, because sync may need to
  treat that as a partial source inventory

The pool is created once per enumeration. After all posted work is done, the
pool is moved into a detached cleanup thread that calls `join()`.

```
main work:
  wait until every posted directory task signaled completion
  publish entries and completeness flag
  return to caller

background cleanup:
  join thread_pool
```

This is why the implementation has careful "lock before decrement and notify"
comments: the caller must not return while workers can still touch captured
state.

### S3 Prefix Discovery

S3 recursive discovery uses the same BFS shape, but the nodes are prefixes
instead of local directories.

```
s3://bucket/root/
        |
        v
ListObjects(root/, delimiter="/")
        |
        +--> files in root/
        +--> common_prefixes:
              root/a/
              root/b/
              root/c/
```

Each discovered `common_prefix` becomes work for the next BFS level.

```
level 0:
  root/

level 1:
  root/a/    root/b/    root/c/

level 2:
  root/a/x/  root/b/y/
```

Important details:

- S3 discovery clamps workers to `1..80`
- non-recursive S3 discovery delegates to the sequential listing path
- listing failures mark the inventory incomplete
- truncated listings without a continuation token are treated as incomplete
- cancellation is cooperative and checked between listing calls

## Directory Diff Execution

After discovery, `run_directory_comparison()` builds maps by relative path:

```
source A entries -> map_a[path] = size
source B entries -> map_b[path] = size
```

It then splits the world into three sets:

```
only in A     -> recorded immediately
only in B     -> recorded immediately
in both       -> compared in parallel
```

Only the "in both" files go through the adaptive comparison pool.

### Choosing File-Level Concurrency

The code estimates the shape of the workload before it starts:

- small files fit in one chunk
- large files span multiple chunks and may benefit from per-file chunk work

```
common files
    |
    +--> count small files
    +--> count multi-chunk files
    +--> choose threads_per_file
    +--> choose initial_concurrency
```

The rough policy is:

| Workload | Strategy |
|----------|----------|
| all small files | maximize file-level concurrency |
| all large files | reserve more thread budget per file |
| mostly large mixed workload | use a few threads per file |
| mostly small mixed workload | keep one thread per file |

Then:

```
max_threads = config.num_threads or 1024
max_concurrency = max_threads / threads_per_file
```

### Adaptive File Window

File comparison uses a sliding window.

```
for each common file:
    wait until in_flight < current_max_concurrency
    ++in_flight
    post comparison job

job finishes:
    store result
    ++completed_files
    --in_flight
    notify submitter
```

Every measurement interval, the controller calculates throughput in files per
second.

```
if throughput improved by more than 10%:
    double current_max_concurrency, capped at max_concurrency

if throughput is flat twice in a row:
    lock concurrency
```

This avoids a fixed setting that is bad for half the workloads. A directory of
100,000 tiny files wants many concurrent file comparisons. A directory with a
few huge files wants fewer file comparisons and more work inside each file.

## Local Chunk CRC

Local chunk checksumming is handled by `compute_crc32_chunks_boost_asio()`.

The flow is:

```
open file
determine file size
build chunk id list
for each chunk:
    read 256 KiB blocks with pread
    crc32_hw(block bytes)
return vector<uint32_t>
```

For up to four chunks, the function stays sequential. For more than four, it
creates a thread pool sized from `std::thread::hardware_concurrency()`.

```
<= 4 chunks:
  chunk 0 -> chunk 1 -> chunk 2 -> chunk 3

> 4 chunks:
             +--> chunk 0
thread pool  +--> chunk 1
             +--> chunk 2
             +--> chunk 3
```

The CRC function itself is zlib-ng, called through `crc32_hw()`:

- zlib-ng detects the CPU at run time and uses PCLMULQDQ/VPCLMULQDQ on x86 or
  the CRC32 instructions on ARMv8, falling back to portable C elsewhere
- no compile flags select the path, so the build sets no ISA baseline

See `crc32-hardware-acceleration.md` for the checksum-specific story.

## S3 Chunk Checksums

For S3 objects larger than the simple local-download path, MitoSync uses
`UploadPartCopy` requests to ask S3 for CRC32 values of byte ranges.

```
chunk id
   |
   v
UploadPartCopy(source range)
   |
   v
x-amz-checksum-crc32
   |
   v
uint32_t result
```

`S3MultipartCopy::ParallelUploadPartCopyRequestsThreadPool()` uses a thread
pool plus a sliding request window:

```
current_max_concurrency starts at:
  ramp_up enabled  -> min(16, num_threads)
  ramp_up disabled -> num_threads

on failed chunk request:
  halve concurrency, floor at min(16, num_threads)

on successful completions:
  double concurrency at ramp thresholds, cap at num_threads
```

This path is network and service limited, not CPU limited. The point of the
window is to keep S3 busy without continuing to hammer it after errors.

## Sync Execution

`run_sync()` has two phases:

```
phase 1: enumerate and classify
phase 2: execute actionable entries
```

Skipped files are removed from the execution list. The remaining entries are
sorted with smaller transfers first and deletes last.

```
plan:
  skip          -> no execution task
  upload        -> task
  upload diff   -> task
  download      -> task
  download diff -> task
  S3 copy       -> task
  delete        -> task, sorted late
```

Execution uses a thread pool and a sliding window over actionable files.

```
max_concurrency = min(effective_max_threads, actionable.size())

upload/download/local:
  initial_concurrency = max_concurrency

S3-to-S3:
  initial_concurrency = min(max_concurrency, 64)
```

The controller reacts to task outcomes:

```
failure:
  current_max_concurrency = max(current / 2, 16)
  reset consecutive success counter

success:
  after enough successes, increase concurrency
  S3-to-S3 doubles every 100 successes
  other modes grow by about 50% every 10 successes
```

### Sync Download Pipeline

Downloads have extra plumbing so network reads and disk writes do not block
each other unnecessarily.

```
S3 prefetch threads
       |
       v
bounded memory cache
       |
       v
execution pool tasks
       |
       v
bounded write queue
       |
       v
disk writer threads
```

Key limits:

- small-file prefetch is capped by `PRE_READ_QUEUE_SIZE`
- total prefetch memory is capped by `PRE_READ_MAX_TOTAL_BYTES`
- disk writes use `DISK_WRITER_THREADS`
- the write queue has a backpressure limit
- differential downloads cap per-file parallel chunks to avoid thread explosion

The goal is to keep S3, CPU, and disk active without allowing memory or task
counts to grow without bound.

## Recursive rm Execution

`rm` first resolves whether the target is a single object or a prefix.

Single object deletion is direct. Recursive prefix deletion enumerates the
prefix, then executes deletion work in parallel.

```
prefix
  |
  v
parallel S3 enumeration
  |
  v
delete jobs
```

There are two modes:

| Mode | Work unit | Notes |
|------|-----------|-------|
| default | one `DeleteObject` call | more granular, easier accounting |
| `--batch` | up to 500 keys per `DeleteObjects` call | fewer API calls, larger failure scope |

Both modes use the same adaptive window:

```
initial_concurrency = min(max_threads, 64)

total batch failure:
  halve concurrency, floor at 16

every 10 successes:
  double concurrency, cap at max_threads
```

Batch mode uses 500 keys rather than S3's maximum 1000 so a partial failure
affects a smaller unit of work and progress updates are more granular.

## Configuration

CLI options:

```text
-t, --threads <N>                  Max execution threads
-r, --ramp-up                      Start some S3 checksum paths lower, then ramp
-P, --parallel-discovery           Enable parallel discovery (default)
--no-parallel-discovery            Disable parallel discovery
--parallel-discovery-workers <N>   Discovery workers, clamped to supported range
```

Internal defaults and caps:

| Setting | Value |
|---------|-------|
| default comparison thread budget | 1024 |
| local discovery cap | 128 workers |
| S3 discovery cap | 80 workers |
| sync S3-to-S3 initial concurrency | up to 64 |
| rm initial concurrency | up to 64 |
| S3 multipart checksum ramp-up start | up to 16 |
| download disk writer threads | 64 |
| default chunk size | 8 MiB |

## Cancellation

Cancellation is cooperative. Workers check an atomic flag at natural boundaries:

- before submitting more work
- before scanning another directory or prefix
- before retry loops continue
- before processing the next transfer/delete

```
main thread:
  progress.cancelled = true

workers:
  finish current small unit
  see cancelled
  return

submitter:
  stops posting new work
  waits for in-flight work to drain
```

This keeps shared data structures consistent. It does mean cancellation may wait
for the current S3 request, file copy, or chunk write to return.

## Design Rules

The implementation follows a few rules that are worth preserving:

1. Bound work submission with an in-flight counter.
2. Prefer one pool per phase over one global pool with unclear ownership.
3. Treat source discovery failures as correctness failures, not empty listings.
4. Use thread pools for many independent jobs; stay sequential when setup cost
   would dominate.
5. Separate network reads from disk writes when downloads are large enough to
   pipeline.
6. Reduce concurrency on failures before retry storms get worse.
7. Make progress counters atomic and result vectors mutex-protected.

## Known Tradeoffs

- Detached pool cleanup in discovery and directory diff reduces caller latency,
  but it relies on strict "all work stopped touching shared state" invariants.
- High thread budgets can reserve a lot of stack memory even when the memory is
  not committed.
- S3 limits vary by account, bucket, endpoint, and network path; adaptive
  windows are still heuristics.
- Parallel progress updates may not be perfectly smooth because work finishes
  out of order.
- `--no-parallel-discovery` is useful for debugging, but recursive local
  discovery still routes through the BFS enumerator at one worker for reliable
  error reporting.
