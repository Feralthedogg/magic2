# magic2

<p align="center">
  <img src="./magic2_logo.png" alt="magic2 logo" width="560">
</p>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)
![C](https://img.shields.io/badge/language-C11-informational)
![C++](https://img.shields.io/badge/C%2B%2B-C%2B%2B17-informational)
![Target](https://img.shields.io/badge/target-x86--64-brightgreen)
![Runtime](https://img.shields.io/badge/runtime-CPU--only-orange)
![Header](https://img.shields.io/badge/distribution-single--header-lightgrey)

**magic2** is a CPU-only adaptive native execution runtime for x86-64 Linux and
Windows. It began as a fast native implementation selector and now represents
the complete execution choice: implementation, ISA, worker budget, tile size,
scheduling policy, memory placement, and graph concurrency.

The runtime keeps a small family of immutable execution plans and chooses among
them from measured workload and resource context. A prepared plan can be sealed
and reused on the data path, while exploration and profile updates stay on the
control path.

> [!IMPORTANT]
> magic2 never guesses that an opaque C callback is safe to partition. A kernel
> must declare its shape, access, alias, tail, scratch, reduction, and failure
> contract before the runtime can execute it as tiles or graph nodes.

> [!NOTE]
> The repository is CPU-only. GPU backends and LLAM integration are deliberately
> outside this project. The public artifact is `magic2.h`.

---

## Why

One ISA is rarely the best execution strategy for every input and every load
level. A memory-bound transform, a compute-heavy map, a tiny request, and a
contended service can need different choices even on the same machine.

magic2 makes those choices explicit and measurable:

- **Execution strategy selection**: native implementation × ISA × worker count ×
  grain × scheduling policy.
- **Resource-aware execution**: worker claims, CPU affinity, CPU-time quota,
  processor groups, NUMA metadata, and workspace budget.
- **Prepared data path**: exact-shape plans and bound frames remove repeated
  binding work from steady-state calls.
- **Adaptive operation**: context buckets, bounded exploration, EWMA, hysteresis,
  resource slack, and safe caller-thread fallback.
- **Parallel dependency graph**: ready-node scheduling, happens-before memory
  reuse, worker-private scratch, and drained per-node failure reporting.
- **Portable knowledge**: checksummed stable-ID profiles can warm-start another
  process or machine without serializing pointers or executable code.

Always benchmark the workload that matters. A larger worker count is not assumed
to be faster, and a remote profile is treated as evidence rather than proof of
local legality or performance.

## Execution model

```text
semantic operation + exact shape
                │
                ▼
        legality and alias checks
                │
                ▼
   strategy recipes → immutable CPU plans
                │
                ├───────────────┐
                ▼               ▼
        adaptive family     sealed graph
                │               │
                ▼               ▼
        context selection   ready dependencies
                │               │
                └───────┬───────┘
                        ▼
              admission and CPU domain
                        │
                        ▼
             persistent executor workers
                        │
                        ▼
                 native kernels
                        │
                        ▼
              bounded observations/profile
```

The plan is immutable after creation. Policy may select another plan between
activations, but it never changes a running plan's worker width, reduction order,
or workspace layout.

## CPU execution plans

`magic2_cpu_plan` fixes an exact `magic2_shape`, a tile callback, a logical grain,
a worker limit, a schedule, CPU feature requirements, and per-slot scratch.
The callback receives a half-open logical range `[begin, end)`, bound ports, a
slot number, and scratch that is exclusive to that slot for the callback.

| Schedule | Behavior | Typical use |
|---|---|---|
| `MAGIC2_CPU_SCHEDULE_CALLER` | Runs on the submitting thread without worker publication | Tiny work and clean fallback |
| `MAGIC2_CPU_SCHEDULE_STATIC` | Assigns tile indices at a fixed stride to worker slots | Regular, balanced maps |
| `MAGIC2_CPU_SCHEDULE_DYNAMIC` | Workers claim the next tile atomically | Irregular tile cost |

A parallel plan declares `MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE`. If a tile
fails, new tiles stop, already-running callbacks drain, and earlier writes stay
visible. The runtime does not retry a partially executed native operation with a
different plan.

## Persistent executor and domain

`magic2_cpu_executor` owns a bounded persistent worker pool. It uses pthreads on
Unix-like systems and Win32 threads and condition variables on Windows. An
activation is admitted as one job or rejected with `MAGIC2_EBUSY`; a rejected
job has invoked no tile callback and has not initialized user ports.

`magic2_cpu_query_resources` reports online CPUs, effective affinity, processor
groups, visible CPU quota, and NUMA node count when the operating system exposes
them. `MAGIC2_CPU_EXECUTOR_AUTO_WORKERS` uses the most conservative visible
capacity, while keeping a fractional quota usable with at least one worker.

`MAGIC2_CPU_EXECUTOR_PIN_WORKERS` pins only workers owned by that executor.
Linux affinity and Windows processor-group locations can also be supplied as
copied `magic2_cpu_location` descriptors. The caller's thread affinity and
other libraries' workers are left unchanged.

The executor intersects CPU features observed by every owned worker. A plan that
requires AVX2 or AVX512 is eligible only when that feature is present across the
whole execution domain.

## Bound frames and asynchronous tickets

For repeated exact-shape work, bind regions once:

```text
magic2_cpu_frame_init
        ↓
magic2_cpu_frame_bind
        ↓
magic2_cpu_frame_run  ← repeat while the frame remains bound
        ↓
magic2_cpu_frame_unbind
```

`magic2_cpu_frame` retains its plan and fixes region addresses and workspace
layout. One frame is exclusive to one activation at a time.

Worker-scheduled plans can be submitted without blocking:

```text
submit → poll / wait → optional cancel → terminal state → release
```

Cancellation is cooperative at tile boundaries. A ticket is not releasable and
its buffers are not reusable until every native callback has returned. A timeout
means “still running”; it does not mean quiescence.

## Adaptive plan families

`magic2_cpu_family` holds equivalent exact-shape plans with unique stable IDs.
Each context bucket independently performs bounded exploration, then chooses from
the measured plans:

1. Observe every eligible plan for the configured local initial sample count.
2. Maintain end-to-end latency EWMA and failure counts.
3. Prefer a plan with fewer worker claims when it is within the configured
   fastest-plan slack.
4. Keep the current plan through small changes using switch hysteresis.
5. Re-explore the least-sampled plan at a bounded interval.

The caller supplies a bucket and optional slot budget. This keeps context
extraction out of the hot path and lets an application map its own workload
phases, queue depth, or admission budget to a stable policy key.

If a selected worker plan encounters a clean executor `MAGIC2_EBUSY`, the family
may execute an eligible caller-scheduled plan once. Native failures and partial
writes are never retried automatically.

## Parallel sealed graphs

Compile a sealed graph with `MAGIC2_SEALED_GRAPH_PARALLEL_SAFE` and a fixed
`parallel_workers` capacity to enable the parallel path. That flag is also the
caller's attestation that opaque node environments and side effects can overlap
according to their declared buffer dependencies.

The graph compiler computes transitive reachability. Two internal regions may
share storage only when every access to one happens-before every access to the
other; unordered branch regions remain distinct. Native scratch is assigned a
worker-private stride.

At runtime, workers claim dependency-ready nodes from an atomic bitset. The
sum of each node's declared `worker_claim` cannot exceed the graph budget. On a
failure, in-flight nodes drain and the result reports completed, failed, and
cancelled node bitsets plus each node's native status.

## Portable profiles

`magic2_cpu_family_profile_export` and `magic2_cpu_family_profile_import` use a
checksummed little-endian `M2CPROF` envelope. A profile contains:

- operation and semantic identity;
- stable plan IDs and context buckets;
- local sample counts and EWMA measurements;
- failure counts and bounded provenance data.

It never contains function pointers, environment addresses, workspace pointers,
or downloaded code. Imported evidence is capped and weighted as a warm-start
prior. Each plan still receives its required local initial measurements, and
re-export includes local evidence only so remote samples do not amplify through a
fleet.

## API map

| API | Purpose |
|---|---|
| `magic2_cpu_executor_create/destroy` | Own a persistent CPU worker domain |
| `magic2_cpu_query_resources` | Inspect OS-visible CPU capacity and quota |
| `magic2_cpu_plan_create/release` | Build an immutable exact-shape strategy |
| `magic2_cpu_plan_run` | Checked one-shot execution |
| `magic2_cpu_frame_*` | Reuse validated region bindings |
| `magic2_cpu_plan_submit` | Publish a worker-scheduled async activation |
| `magic2_cpu_ticket_*` | Poll, wait, cancel, and release an activation |
| `magic2_cpu_family_*` | Select, observe, reset, and profile equivalent plans |
| `magic2_sealed_graph_run_parallel_bound` | Run a parallel-safe sealed graph |

The existing adaptive pair/buffer, native lease, prepared batch, and sequential
sealed graph APIs remain in the same header. The CPU layer is additive and does
not require an external runtime library.

## Minimal plan setup

The complete contract includes two ports and two caller-owned regions. The
following shows the key plan fields; the repository package contains full
contract tests in `tests/`.

```c
#define MAGIC2_IMPLEMENTATION
#include "magic2.h"

static int tile(const void *env, void *const *ports,
                size_t begin, size_t end, uint32_t slot, void *scratch) {
    float *out = (float *)ports[0];
    const float *in = (const float *)ports[1];
    size_t i;
    (void)env; (void)slot; (void)scratch;
    for (i = begin; i < end; ++i) out[i] = in[i] * 2.0f;
    return 0;
}

/* Fill two magic2_port entries: writable output region 0 and readable input
 * region 1, both with the exact byte extent and alignment for count values. */
magic2_cpu_plan_spec spec = MAGIC2_CPU_PLAN_SPEC_INIT;
spec.flags = MAGIC2_CPU_PLAN_ALLOW_PARTIAL_FAILURE;
spec.schedule = MAGIC2_CPU_SCHEDULE_DYNAMIC;
spec.shape.count = count;
spec.shape.port_count = 2;
spec.shape.region_count = 2;
spec.shape.ports = ports;
spec.tile = tile;
spec.grain = 4096;
spec.worker_limit = 4;
spec.failure_contract = MAGIC2_FAILURE_PARTIAL;

magic2_cpu_executor_config ec = MAGIC2_CPU_EXECUTOR_CONFIG_INIT;
ec.max_workers = 4;
magic2_cpu_executor *executor = NULL;
magic2_cpu_plan *plan = NULL;
magic2_cpu_executor_create(&ec, &executor);
magic2_cpu_plan_create(&spec, &plan);
magic2_cpu_plan_run(plan, executor, regions, 2,
                    workspace, workspace_bytes, NULL);
magic2_cpu_plan_release(&plan);
magic2_cpu_executor_destroy(&executor);
```

Use `magic2_cpu_plan_get_info` before allocating `workspace`. A bound frame is
preferable when the same exact regions are reused across many activations.

## Build

The header is usable from C11 and C++17. The built-in executor uses the platform
thread implementation only when its implementation translation unit is compiled.

```sh
# Linux / macOS / other Unix-like hosts
clang -std=c11 -O2 -pthread -c impl.c -o magic2-runtime.o

# MinGW-w64 x86-64 cross-build
x86_64-w64-mingw32-gcc -std=c11 -O2 -c impl.c -o magic2-runtime.o

# MSVC Developer Command Prompt (C11 mode)
cl /nologo /TC /std:c11 /O2 /I. your_client.c magic2-runtime.c
```

The full source-only validation kit is available as `outputs/magic2.zip` in the
development workspace; the repository itself contains the header, examples,
tests, and CI workflow needed to build the runtime directly.

## CI

Every push to `main` and every pull request runs [`.github/workflows/ci.yml`](./.github/workflows/ci.yml).
The workflow builds and runs the example and CPU/graph tests with Clang and GCC
under C11 and C++17 AddressSanitizer/UndefinedBehaviorSanitizer, checks separate
implementation linkage, cross-builds MinGW-w64 x86-64 Windows artifacts, and
runs the example plus public client with MSVC on `windows-latest`.

## Verification and measured behavior

The implementation was checked with:

- Clang C11/C++17 ASan and UBSan full regression tests;
- CPU and graph ThreadSanitizer suites;
- GCC 14.2 Linux amd64 execution under a constrained container;
- MinGW-w64 x86-64 strict builds and static PE64 execution through Wine;
- deterministic random-DAG differential tests and profile mutation tests.

On an Apple M4 test host, the same compute kernel in a parallel sealed graph
measured the following median speedups over sequential execution:

| Independent nodes | Median speedup |
|---:|---:|
| 2 | 1.938× |
| 4 | 3.870× |
| 8 | 4.881× |

These are scoped M4 measurements. They are not x86-64 or Windows performance
claims, and they do not guarantee a particular worker count on another machine.

## Scope and limits

- Opaque callbacks must provide truthful contracts; arbitrary pointer aliasing
  cannot be inferred safely.
- A native callback that creates its own threads must declare its inclusive
  `worker_claim` so graph admission can bound nested parallelism.
- Cancellation cannot forcibly stop a native function that has no cooperative
  checkpoint.
- Graph execution reports partial writes after failure; whole-graph rollback
  requires an explicit staging and commit design.
- Native Windows/MSVC execution and native x86-64 performance campaigns remain
  separate validation work from the emulated and cross-compiled evidence above.

## License

magic2 is released under the [MIT License](./LICENSE).

Copyright (c) 2026 Feralthedogg.
