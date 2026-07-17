# Pluggable Buffer Pool Manager with Modern Eviction Policies

A database **buffer pool manager** in C++17 — the component that caches fixed-size disk pages in
memory — with **five pluggable eviction policies**, benchmarked against each other:

| Policy | Idea | Origin |
|---|---|---|
| **FIFO** | evict the oldest inserted page | baseline |
| **LRU** | evict the least recently used page | textbook |
| **CLOCK** | LRU approximation: circular buffer + reference bits | what PostgreSQL actually uses |
| **SIEVE** | one FIFO queue + a persistent hand + a visited bit (lazy promotion) | [NSDI '24](https://cachemon.github.io/SIEVE-website/) |
| **S3-FIFO** | small (10%) + main (90%) + ghost queues; one-hit-wonders never reach main | [SOSP '23](https://s3fifo.com) |

Pages live in a **simulated disk** (a real file, addressed as fixed 4 KB pages at
`page_id * 4096`). The pool holds N frames in **one** preallocated `std::vector<char>` of
`N * 4096` bytes, with an `unordered_map` page table mapping `page_id -> frame index` and
pin/unpin semantics (a pinned page is never evicted). The metric that matters is **miss ratio** —
i.e. disk I/Os avoided.

Standard library only. No Boost, no threads, no networking, no `new`/`delete`, no smart pointers:
all frames and policy nodes are preallocated in `std::vector`s and linked by **integer index**
(`int prev, next;` with `-1` as null).

> **Status: Phase 4 of 6.** All five policies are implemented and tested; benchmarks run
> end to end (CSV -> PNGs -> summary table). Phase 5 sanity-checks the results against
> the papers' claims. See [PROJECT_TRACKER.md](PROJECT_TRACKER.md).

## Build and run (3 commands)

Requires **g++ (MinGW-w64)** on PATH — this repo is developed with MSYS2 UCRT64 g++ 13.1.0 at
`C:\msys64\ucrt64\bin`. **CMake is not required** (and is not installed here).

```powershell
.\build.ps1                    # 1. compile both binaries with -O2, then run the tests
.\build\runner.exe             # 2. run the benchmarks  -> results\results.csv
.\venv\Scripts\python plot.py  # 3. CSV -> PNGs + results\summary.md
```

Step 3 uses the venv at `venv/` (create once with `python -m venv venv`, then
`.\venv\Scripts\python.exe -m pip install matplotlib`).

`build.ps1` is just a wrapper around two g++ calls. The raw fallback, if you prefer:

```powershell
g++ -O2 -std=c++17 -Wall -Wextra -Iinclude src\*.cpp bench\runner.cpp    -o build\runner.exe
g++ -O2 -std=c++17 -Wall -Wextra -Iinclude src\*.cpp tests\run_tests.cpp -o build\run_tests.exe
```

**`-O2` matters**: the ops/sec micro-benchmark measures per-policy CPU overhead, and those numbers
are meaningless in an unoptimized build. All reported ops/sec figures use `-O2`.

If you would rather use CMake, `CMakeLists.txt` is committed and works:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Tests are a single self-contained `tests/run_tests.cpp` with a hand-rolled `CHECK` macro and a
pass/fail count — no GoogleTest, no Catch2. It exits non-zero on failure.

## Repo layout

```
include/
  policy.h            EvictionPolicy abstract base: on_access / on_insert / evict / name
                      + pin tracking shared by all policies
  disk_manager.h      simulated disk: 4 KB pages via fstream, counts every read/write
  buffer_pool.h       frames, page table, pin counts, hit/miss stats
  policies/           fifo.h lru.h clock.h sieve.h s3fifo.h   (header-only)
src/
  disk_manager.cpp  buffer_pool.cpp
bench/
  traces.h/.cpp       Zipfian / sequential-scan / hot-set-shift generators
  runner.cpp          sweeps workloads x policies x cache sizes -> results.csv
tests/
  run_tests.cpp       hand-computed expected evictions per policy + invariants
plot.py               the only Python: CSV -> miss-ratio-vs-cache-size PNGs + summary.md
results/              results.csv, PNGs, summary.md, and the *.db disk files
```

## What the benchmarks measure

- **Miss ratio vs. cache size**, one chart per workload, all five policies as curves. Workloads:
  Zipfian (α = 0.6 / 0.8 / 1.0), sequential scan (LRU's worst case), and a hot-set-shift mix.
- **Ops/sec**, disk I/O disabled, to isolate policy overhead — this is where SIEVE's O(1) hand
  walk shows up against LRU's list splice on *every* hit.

Expected (and to be verified in Phase 5): SIEVE and S3-FIFO beat LRU on Zipfian and resist scan
pollution; on a pure sequential scan every policy does badly.
