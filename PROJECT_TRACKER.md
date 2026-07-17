# PROJECT_TRACKER.md

## Project summary

A database **buffer pool manager** in C++17 (standard library only) with **five pluggable
eviction policies** — FIFO, LRU, CLOCK, SIEVE (NSDI'24), and S3-FIFO (SOSP'23) — benchmarked
against each other on synthetic access traces. Pages live in a simulated disk (a real file
addressed as fixed 4 KB pages), the pool holds N frames in one preallocated byte array, and a
hash-map page table maps `page_id -> frame index`. The metric that matters is **miss ratio**,
i.e. disk I/Os avoided; a separate `chrono` micro-benchmark reports **ops/sec** with disk I/O
disabled to isolate per-policy CPU overhead. The goal is to reproduce the papers' qualitative
claim — SIEVE and S3-FIFO beat LRU on Zipfian traces and resist scan pollution — with our own
numbers and charts.

## Phase table

| Phase | Work | Model | Status |
|---|---|---|---|
| 0 | Plan & scaffold; interfaces + stub main; **prove the toolchain compiles** | Opus, high | ✅ |
| 1 | Core buffer pool + FIFO + LRU (index-linked intrusive list) | Sonnet, medium | ✅ |
| 2 | CLOCK + SIEVE + hand-computed tests | Sonnet, high | ⬜ |
| 3 | S3-FIFO (small/main/ghost queues) + tests | Opus, medium | ⬜ |
| 4 | Trace generators + benchmark runner + `plot.py` | Sonnet, medium | ⬜ |
| 5 | Sanity-check results vs. papers + polish + resume bullets | Opus, medium | ⬜ |
| 6 | Teaching phase → `EXPLANATION.md` | Opus, high | ⬜ |

## Decisions log

| # | Decision | Why |
|---|---|---|
| D1 | **Policies are header-only** (`include/policies/*.h`), no `src/policies/*.cpp` | Each policy is ~60–100 lines; a split .h/.cpp doubles the file count for no benefit. `src/policies/` is kept (empty) and globbed by CMake in case S3-FIFO outgrows a header. |
| D2 | **g++ one-liner is the primary build path; CMake is optional** | CMake is **not installed** on this machine. g++ 13.1.0 (MSYS2 UCRT64) is, at `C:\msys64\ucrt64\bin`. `CMakeLists.txt` is committed and correct, but the README leads with `build.ps1` / the raw g++ command. |
| D3 | **Pinning lives in the `EvictionPolicy` base class** as a preallocated `vector<uint8_t>` indexed by page_id | Every policy handles pinning identically (a pinned page is not a legal victim), so it is written once. Preallocating the full page-id space means `set_pinned` never allocates. |
| D4 | **Policies track `page_id` only** — never frames, never disk | Keeps the interface narrow (4 virtual methods) and makes the ops/sec micro-benchmark trivially fair: it measures policy bookkeeping and nothing else. |
| D5 | `page_id_t = int`, `INVALID_PAGE = -1` | `-1` doubles as the null link for the index-linked intrusive lists, so one sentinel covers both uses. |
| D6 | Prior abandoned **Python** scaffold moved to `_old_python_attempt/` rather than deleted | It was pure `NotImplementedError` stubs, superseded by the C++ plan. Delete it whenever; it is not referenced by anything. |
| D7 | `DiskManager::set_io_enabled(false)` makes I/O a counted no-op | Phase 4's ops/sec bench needs disk disabled; a flag on the existing class beats a second fake-disk type. |
| D8 | `gh` CLI not installed at end of Phase 0; GitHub push deferred until user installs it and runs `gh auth login` | Cannot create/push to a GitHub repo without it; global git identity (`sparshagra`) was already correctly configured, so local commits proceeded without waiting. Resolved: `gh` installed, authenticated as `sparshagra`, repo created and pushed at start of Phase 1. |
| D9 | FIFO/LRU internal lists use `std::unordered_map<page_id_t,int>` from page_id to node index, with a `capacity_`-sized (not `num_pages_`-sized) node array | Matches the master prompt's spec exactly and mirrors how `BufferPool`'s own page table works: node storage is bounded by how many pages can actually be resident, not by the whole page-id space, which is the realistic constraint for a real cache. |
| D10 | Hand-computed policy tests call `on_access`/`on_insert`/`evict()` directly on a bare policy via a small `simulate()` test helper, not through `BufferPool`/`DiskManager` | Isolates policy logic from disk I/O so the expected-eviction traces are exact and fast; `BufferPool`-level integration is covered separately by `bench/runner.cpp`. |

## Git / GitHub workflow (effective Phase 1 onward)

- Repo was `git init`'d locally at the end of Phase 0; Phase 0's scaffold is committed as
  4 logical commits (scaffold/config, interfaces, disk manager + pool skeleton, stub runner/tests).
- Remote: public GitHub repo **`sieve-bufferpool`** under the user's account, created via `gh repo
  create` once `gh auth login` is done (blocked on `gh` CLI install as of end of Phase 0 —
  see Decisions log D8).
- **Every phase from Phase 1 onward ends with: build passes → tests pass → commit → push.**
  Never batch multiple phases into one commit; never push broken code; never `--force`;
  never fake/backdate commit timestamps.
- Each phase is split into **2–4 logical commits**, not one dump (e.g. Phase 2 →
  "Add CLOCK eviction policy" / "Add SIEVE eviction policy (NSDI'24)" /
  "Add hand-computed policy tests").
- Commit messages: short imperative subject line; a body line explaining **why** only when
  the reason isn't obvious from the diff (e.g. why SIEVE's hand persists across evictions).
- **Commit authorship is the user's alone** — global git identity is already set to
  `sparshagra <sparsh51@outlook.com>`; nothing in this repo's history should mention Claude,
  Anthropic, or any AI coding agent. Do not add Co-Authored-By trailers here.
- Python venv for `plot.py` (Phase 4) lives at `venv/` in the repo root, gitignored.

## Phase 1 — what exists now

- `include/policies/fifo.h`, `include/policies/lru.h` — both header-only, both an
  index-linked doubly linked list over a `capacity_`-sized node array + `unordered_map`
  from page_id to node index (see D9). FIFO's `on_access` is a no-op; LRU's splices the
  hit node to the newest end.
- `src/buffer_pool.cpp` — `fetch_page`, `unpin_page`, `flush_all`, `find_victim_frame` are
  all real. Pin count and the policy's pinned flag are kept in sync on every pin/unpin.
- `tests/run_tests.cpp` — 23 assertions: original Phase 0 tests + hand-computed FIFO/LRU
  eviction traces (worked by hand in the commit messages) + pin invariants (pinned page
  never evicted; evict() returns `INVALID_PAGE` when everything is pinned) run against
  both policies.
- `bench/runner.cpp` — no longer uses the Phase 0 `NoopPolicy`; drives a real FIFO pool
  through 5 fetch/unpin calls on a 4-frame pool and confirms the correct page gets evicted.

**Verified:** `.\build.ps1` compiles both binaries with zero warnings and reports
`23 passed, 0 failed`; `.\build\runner.exe` confirms page 0 is evicted (FIFO, oldest) to
make room for page 4, and page 4 is resident afterward.

**GitHub:** repo created at `github.com/sparshagra/sieve-bufferpool` (public), Phase 0
scaffold + Phase 1 pushed as 9 commits total, all authored solely as `sparshagra`.

## Phase 0 — what exists now

```
sieve-bufferpool/
  README.md  PROJECT_TRACKER.md  CMakeLists.txt  build.ps1  .gitignore
  include/  policy.h  disk_manager.h  buffer_pool.h  policies/   (empty until Phase 1)
  src/      disk_manager.cpp (real)  buffer_pool.cpp (ctor real, methods stubbed)  policies/ (empty)
  bench/    runner.cpp   (stub main; Phase 4 makes it the real runner)
  tests/    run_tests.cpp (CHECK/CHECK_EQ macros + 15 assertions)
  results/  (generated: *.db, results.csv, PNGs, summary.md)
```

Interfaces frozen in Phase 0:
- `EvictionPolicy` — `on_access(pid)`, `on_insert(pid)`, `evict() -> page_id_t`,
  `name()`; plus concrete `set_pinned/is_pinned/capacity` in the base.
- `BufferPool` — `fetch_page(pid) -> char*` (pins), `unpin_page(pid, is_dirty)`,
  `flush_all()`, `stats()`, `is_resident(pid)`.
- `DiskManager` — `read_page/write_page` at `pid * 4096`, `num_reads/num_writes`,
  `set_io_enabled`.

**Verified:** both binaries compile with `-O2 -std=c++17 -Wall -Wextra` and **zero warnings**;
runner prints its config; tests report `15 passed, 0 failed`.

`NoopPolicy` is duplicated in `bench/runner.cpp` and `tests/run_tests.cpp` purely to exercise
virtual dispatch in Phase 0. **Phase 1 deletes both copies** once FIFO exists.

## How to resume in a fresh session

Read **only** this tracker plus the files of the current phase — do not read the whole repo.

- Phase 1 → `include/policy.h`, `include/buffer_pool.h`, `src/buffer_pool.cpp`, `tests/run_tests.cpp`
- Phase 2 → `include/policy.h`, `include/policies/lru.h` (as the pattern to follow), `tests/run_tests.cpp`
- Phase 3 → `include/policy.h`, `include/policies/sieve.h`, `tests/run_tests.cpp`
- Phase 4 → `include/buffer_pool.h`, `bench/runner.cpp`, all of `include/policies/`
- Phase 5 → `results/results.csv`, `results/summary.md`, `README.md`
- Phase 6 → everything, but for explaining rather than editing

Standing rules: one phase per response, then stop and wait for "continue". Every phase ends with
code that **compiles and whose tests pass — build and run them before reporting done**. Keep each
source file under ~150 lines. No `new`/`delete`, no smart pointers, no threads, no external deps.
Preallocate `std::vector`s and link nodes by **integer index** (`int prev, next; -1 = null`).
Update this tracker at the end of every phase.

Build + test in one command from the repo root:

```powershell
.\build.ps1
```
