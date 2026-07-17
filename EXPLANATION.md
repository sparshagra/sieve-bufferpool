# The project, explained from the ground up

This assumes you've skimmed a DBMS course PDF and nothing more. It builds up in order, and
everything ties back to the actual code in this repo — file names, class names, function names.

Read it top to bottom once. Sections 1–3 are the concepts, 4 is our real data, 5 is the C++, 6
is how to talk about it, 7 is what you must know cold.

---

## 1. The problem: why a database needs its own page cache

### 1.1 The setup

A database is bigger than RAM. Your data lives in a file on disk, cut into fixed-size **pages**
(ours are 4 KB — see `PAGE_SIZE` in [`include/disk_manager.h`](include/disk_manager.h)). To read
or write a row, you must first get its page into memory.

The speed gap is the entire reason this component exists:

| operation | roughly |
|---|---|
| read 4 KB from RAM | ~0.1 µs |
| read 4 KB from SSD | ~100 µs |
| read 4 KB from spinning disk | ~10 000 µs |

An SSD page read is about **1000× slower** than a memory read. So you cache pages in RAM. The
thing that manages that cache is the **buffer pool**, and it is the single most important
performance component in a database.

In our code the simulated disk is [`DiskManager`](src/disk_manager.cpp): a real file, where page
`N` lives at byte offset `N * 4096`. It counts every `read_page`/`write_page`, because *disk I/Os
avoided* is the number this whole project measures.

### 1.2 Why not just let the OS do it?

The operating system already caches file pages. So why does every serious database (Postgres,
MySQL, Oracle, SQLite) build its own buffer pool anyway? Four reasons — and the second is the one
that actually forces the issue:

1. **The database knows things the OS cannot.** The OS sees "a read of block 4192". The database
   knows "this is a sequential scan of a 10 GB table that I will never touch again." Same syscall,
   completely different correct caching decision. The OS can't tell them apart; the DB can.

2. **Correctness — write-ahead logging.** For crash recovery, a DB must guarantee that the *log
   record* describing a change reaches disk **before** the *data page* itself does. With the OS
   page cache, the kernel flushes dirty pages whenever it feels like it. You cannot enforce that
   ordering, so you cannot guarantee recovery. Owning the buffer pool means owning exactly when a
   page is written back. *(We don't implement WAL here — but this is the honest answer to "why not
   the OS cache", and it's why the `dirty` flag and `flush_all()` exist in our API.)*

3. **Double buffering.** If the OS caches a page and the DB caches it too, you're storing it twice
   and wasting half your memory.

4. **Pinning.** A DB needs to guarantee a page stays put while a query is reading it. The OS
   offers nothing that precise.

Interview-safe one-liner: *"The OS page cache doesn't know what a scan is, and can't guarantee
write ordering for WAL. A database needs both, so it manages its own pages."*

### 1.3 The three data structures

This is the whole architecture, and it's in [`include/buffer_pool.h`](include/buffer_pool.h):

- **Page** — a fixed 4 KB chunk of the *disk file*, identified by `page_id`.
- **Frame** — a fixed 4 KB slot *in memory* that can hold one page. We allocate all of them as
  **one** `std::vector<char> frames_` of `num_frames * 4096` bytes; frame `i` is the slice at
  offset `i * 4096` (see `frame_data()`).
- **Page table** — `std::unordered_map<page_id_t, int> page_table_`, mapping `page_id → frame
  index`. This is how you answer "is this page already in memory, and where?"

A page is a thing on disk. A frame is a place in memory. The page table is the index between them.
Don't mix up the words in an interview; it's an instant tell.

The core loop is `BufferPool::fetch_page(pid)` in [`src/buffer_pool.cpp`](src/buffer_pool.cpp):

```
look up pid in page_table_
  found?     -> stats_.hits++,  pin it, tell the policy "this was a hit", return the frame
  not found? -> stats_.misses++, find a free frame (or evict someone to make one),
                read the page from disk into it, pin it, tell the policy "new page inserted"
```

### 1.4 Pin / unpin

`fetch_page` **pins** the page (`++meta_[frame].pin_count`). A pinned page must not be evicted —
some query is actively reading that memory, and yanking it would hand out a dangling frame.
`unpin_page(pid, is_dirty)` drops the pin, and `is_dirty=true` marks the frame as modified so it
gets written back to disk before eviction (`find_victim_frame` does that write).

The important subtlety: the pin state lives in **two** places that must never disagree — the
frame's `pin_count`, and the policy's own bitmap. `fetch_page`/`unpin_page` keep them in sync via
`policy_->set_pinned(pid, ...)`. The policy needs its own copy because *it* is the one scanning
for a victim, and it must skip pinned pages without calling back into the pool.

If every page is pinned, there is no legal victim: `evict()` returns `INVALID_PAGE` and
`fetch_page` returns `nullptr`. That's a real (if rare) condition in a real DB, and we test it
(`test_all_pinned_invariant`).

---

## 2. Why the eviction policy is the whole ballgame

The pool is full. A new page must come in. **Someone must leave.** Choosing who leaves is the
eviction policy, and it is the only interesting decision in the entire component.

Here's why it dominates everything else. Take our real measured numbers from `zipf_scan_mix` at
cache = 200 frames, and assume a 100 µs SSD read and a 0.1 µs memory hit:

| policy | miss ratio | average request latency |
|---|---|---|
| LRU | 0.2937 | 0.2937 × 100 µs + 0.7063 × 0.1 µs ≈ **29.4 µs** |
| SIEVE | 0.2065 | 0.2065 × 100 µs + 0.7935 × 0.1 µs ≈ **20.7 µs** |

Same hardware. Same memory budget. Same code path. **30 % lower latency**, purely from choosing
who to evict. Over our 20 000-request trace that's 1744 disk reads that simply never happen.

Now the counterintuitive part, which is a favourite interview follow-up. Our ops/sec bench says
SIEVE does 8.7 M ops/sec and LRU 7.5 M — i.e. SIEVE's bookkeeping is ~0.018 µs/op cheaper. But the
miss-ratio difference above is worth **8.7 µs/op**. The I/O effect is roughly **480× larger than
the CPU effect.**

> **The lesson:** miss ratio is what matters; policy CPU cost is rounding error — *unless* your
> working set fits entirely in memory, in which case there's no I/O left and CPU cost becomes
> everything. That's exactly why we measure both, and why the ops/sec bench runs with I/O disabled
> (`DiskManager::set_io_enabled(false)`) to isolate it.

---

## 3. The five policies

All five implement the same four-method interface,
[`EvictionPolicy`](include/policy.h) — `on_access` (a hit), `on_insert` (a page arrived),
`evict()` (pick a victim), `name()`. **A policy only ever sees `page_id`s.** It never touches
frame memory or the disk. That narrow interface is what "pluggable" means here.

To compare them fairly, every example below uses the **same access string** with a **3-frame
cache**:

```
1, 2, 3, 1, 4, 1, 5, 1
```

Page **1 is the hot page** — requested 4 times. Pages 2, 3, 4, 5 are touched once each. Any decent
policy should notice and keep page 1. These traces are verified against the real implementations,
not hand-waved.

**Scoreboard (hits out of 8):** FIFO 2 · LRU 3 · CLOCK 2 · SIEVE 3 · S3-FIFO 3

---

### 3.1 FIFO — the baseline

**Idea:** evict whatever went in first. A queue. A hit changes *nothing* — that's the entire
policy. (`FIFOPolicy::on_access` is literally an empty function body.)

| # | req | | cache (oldest→newest) | result |
|---|---|---|---|---|
| 1 | 1 | | `1` | miss |
| 2 | 2 | | `1 2` | miss |
| 3 | 3 | | `1 2 3` | miss |
| 4 | **1** | | `1 2 3` | **HIT** (no reordering!) |
| 5 | 4 | | `2 3 4` | miss — **evicts 1** |
| 6 | **1** | | `3 4 1` | miss — evicts 2 |
| 7 | 5 | | `4 1 5` | miss — evicts 3 |
| 8 | **1** | | `4 1 5` | **HIT** |

**2 hits.**

**The weakness, in one line:** at step 5, FIFO evicted page 1 — the hottest page in the trace —
*one step after it was used*, because FIFO refuses to look at usage at all. Age is not popularity.

**→ Which leads to LRU.**

---

### 3.2 LRU — track recency properly

**Idea:** evict the **least recently used** page. On a hit, move that page to the "most recently
used" end. Ordered by *last use*, not by *arrival*.

| # | req | | cache (LRU→MRU) | result |
|---|---|---|---|---|
| 1 | 1 | | `1` | miss |
| 2 | 2 | | `1 2` | miss |
| 3 | 3 | | `1 2 3` | miss |
| 4 | **1** | | `2 3 1` | **HIT** — 1 moves to MRU |
| 5 | 4 | | `3 1 4` | miss — evicts 2 (now LRU) |
| 6 | **1** | | `3 4 1` | **HIT** — 1 survived! |
| 7 | 5 | | `4 1 5` | miss — evicts 3 |
| 8 | **1** | | `4 5 1` | **HIT** |

**3 hits.** The promotion at step 4 is what saves page 1 at step 5. That's the whole difference.

**Implementation:** [`include/policies/lru.h`](include/policies/lru.h) — a doubly linked list where
`move_to_head()` unlinks and relinks the node on **every single hit**. That's O(1), but it's a
handful of pointer (index) writes on the hottest path in the system.

**Two weaknesses:**

1. **Cost.** Every hit does list surgery. In a real DB this list is shared across threads, so it
   needs a lock — and now your *cache hits*, the fast path, are serialising on a mutex. This is a
   genuine scalability problem in real systems, and it's the practical reason nobody ships textbook
   LRU.
2. **Scan pollution** (the big one). Read a 10 GB table once, sequentially. Every one of those
   pages is "recently used", so LRU cheerfully evicts your entire hot working set to make room for
   pages it will never see again. One scan destroys the cache. Our `zipf_scan_mix` benchmark shows
   this brutally (§4.1).

**→ CLOCK attacks weakness 1. SIEVE and S3-FIFO attack weakness 2.**

---

### 3.3 CLOCK — LRU's accuracy, without the list surgery

**Idea:** approximate LRU. Keep frames in a **circular array**, each with one **reference bit**.
A hit just sets the bit (`ref_bit = true`) — no list, no reordering, no lock needed for a hit; on
real hardware it's a single atomic store. To evict, a **hand** sweeps forward: if a slot's bit is
set, clear it and move on ("second chance"); the first slot with a clear bit is the victim.

This is what PostgreSQL actually uses. That's a good thing to know.

Our trace — slots shown as `page*` when the ref bit is set, hand marked `^`:

| # | req | | slots | result |
|---|---|---|---|---|
| 1 | 1 | | `[1* ^, –, –]` | miss (new pages get bit **set**) |
| 2 | 2 | | `[1* ^, 2*, –]` | miss |
| 3 | 3 | | `[1* ^, 2*, 3*]` | miss |
| 4 | **1** | | `[1* ^, 2*, 3*]` | **HIT** — just sets 1's bit (already set) |
| 5 | 4 | | `[4*, 2 ^, 3]` | miss — **evicts 1** (see below) |
| 6 | **1** | | `[4*, 1*, 3 ^]` | miss — evicts 2 |
| 7 | 5 | | `[4* ^, 1*, 5*]` | miss — evicts 3 |
| 8 | **1** | | `[4* ^, 1*, 5*]` | **HIT** |

**2 hits — same as FIFO.** Look closely at step 5, because it's the lesson. *Every* bit was set, so
the hand swept the whole array clearing bits (1→0, 2→0, 3→0), wrapped around to slot 0, found page
1 now clear, and evicted it — **the page that had just been hit one step earlier.**

**The weakness:** when all reference bits are set, **CLOCK degenerates into exactly FIFO.** The bit
is one bit — it records "used at least once since the hand last passed", not *how recently* or *how
often*. Here, pages 2 and 3 got their bits set for free just by being inserted, so they had "credit"
they never earned, and the genuinely hot page paid for it.

**→ SIEVE fixes precisely this, with a change so small it's almost annoying.**

---

### 3.4 SIEVE — one small change to CLOCK (NSDI '24)

SIEVE looks like CLOCK. Two differences, both tiny, both load-bearing:

1. **New pages are inserted at the head with `visited = false`** — no free pass. (CLOCK gives new
   pages `ref_bit = true`.)
2. **The hand is *persistent*.** It does **not** reset to the tail on each eviction; it stays
   exactly where it stopped and resumes from there next time. It moves tail→head, and wraps back to
   the tail when it runs off the head.

Everything else is CLOCK: a hit sets the visited bit, and *nothing moves*.

The two names to know:

- **Lazy promotion.** A hit only flips a bit. The object **never changes position** in the queue.
  Compare LRU, which does list surgery on every hit. Objects earn their keep passively — they're
  promoted "lazily", only in the sense that they survive the hand's next pass.
- **Quick demotion.** New objects enter unprotected (`visited=false`) *near the hand's eventual
  path*, so an object that is never re-accessed gets thrown out **fast**. This is the key insight:
  most cached objects are never re-used, so the cache's real job is to *get rid of them quickly*,
  not to rank the survivors precisely.

Our trace (list shown head→tail; `v` = visited bit; `^` = hand):

| # | req | | list (head→tail) | result |
|---|---|---|---|---|
| 1 | 1 | | `1(v0)` | miss — inserted at head, **unprotected** |
| 2 | 2 | | `2(v0) 1(v0)` | miss |
| 3 | 3 | | `3(v0) 2(v0) 1(v0)` | miss |
| 4 | **1** | | `3(v0) 2(v0) 1(v1)` | **HIT** — sets 1's bit, **1 does not move** |
| 5 | 4 | | `4(v0) 3(v0)^ 1(v0)` | miss — evicts 2 |
| 6 | **1** | | `4(v0) 3(v0)^ 1(v1)` | **HIT** — sets bit again |
| 7 | 5 | | `5(v0) 4(v0)^ 1(v1)` | miss — evicts 3 |
| 8 | **1** | | `5(v0) 4(v0)^ 1(v1)` | **HIT** |

**3 hits — matches LRU, and page 1 never moved once.**

Step 5 is the one to understand. The hand starts at the tail (page 1). Page 1 has `v=1` → clear it
to `v=0` and step toward the head. Page 2 has `v=0` → **evict page 2**. The hand stops there
(at page 3) and *stays* — so at step 7 it evicts page 3 immediately, with no re-scanning. That
persistence is why SIEVE doesn't repeatedly re-walk the same protected pages.

And notice page 1 survives despite its bit being cleared at step 5, because it gets re-accessed at
step 6 before the hand ever comes back around. **That is the whole algorithm**: hot pages keep
re-arming their bit faster than the hand can lap them.

**Why it's fast:** a hit is one bit write. Compare `LRUPolicy::move_to_head`. Our bench: SIEVE
8.7 M ops/sec vs LRU 7.5 M.

**The weakness — and we measured it (§4.3):** because objects *never move*, SIEVE has no way to
express "this page was hot an hour ago but is cold now." A stale hot page keeps `visited=1` and
sits exactly where it is; the hand must lap the queue **twice** to remove it (once to clear the
bit, once to evict). So when the working set **shifts**, SIEVE adapts slowly. We measured it losing
to *plain FIFO* on such a workload. LRU, which reorders constantly, handles it fine.

**→ S3-FIFO keeps quick demotion but adds a way back in.**

---

### 3.5 S3-FIFO — three queues (SOSP '23)

**The founding observation: most objects are one-hit wonders.** In real cache workloads, the large
majority of objects are requested **exactly once** and never again. If that's true, the cache's
main job isn't ranking — it's *keeping one-hit wonders out of the good real estate entirely*.

So: **three FIFO queues**
([`include/policies/s3fifo.h`](include/policies/s3fifo.h)):

- **small** — ~10 % of capacity. **Every new page enters here.** The probation ward.
- **main** — ~90 % of capacity. Pages that proved themselves. Uses **FIFO-reinsertion** (CLOCK-like,
  with counters capped at 3 instead of a single bit).
- **ghost** — **keys only, no page data.** Just a record of "this key was evicted from small
  recently." Sized ≈ main's object count. Because it stores no 4 KB payloads, it's almost free.

The rules:

- **Miss, key not in ghost** → insert into **small**.
- **Miss, key IS in ghost** → insert **straight into main**. Being in ghost is *proof* the page was
  requested at least twice in a short window — so it is, by definition, not a one-hit wonder.
- **Evicting from small:** accessed ≥1 time → **promote to main**. Never accessed → **evict, and
  write its key to ghost.**
- **Evicting from main:** counter > 0 → decrement and reinsert at head (second chance); counter 0 →
  evict.

The ghost queue is the clever bit. It's a **second chance for the cache to change its mind, at
metadata cost only.** A one-hit wonder dies in small and never pollutes main. But a page that
*looked* like a one-hit wonder and wasn't gets re-admitted straight to main on its next request —
no second probation period.

Our trace (`f` = frequency counter):

| # | req | | small (head→tail) | main | ghost | result |
|---|---|---|---|---|---|---|
| 1 | 1 | | `1(f0)` | – | – | miss → small |
| 2 | 2 | | `2 1` | – | – | miss |
| 3 | 3 | | `3 2 1(f0)` | – | – | miss |
| 4 | **1** | | `3 2 1(f1)` | – | – | **HIT** — f1 → 1 |
| 5 | 4 | | `4 3` | `1(f0)` | `{2}` | miss — 1 **promoted to main** (f≥1), 2 evicted → ghost |
| 6 | **1** | | `4 3` | `1(f1)` | `{2}` | **HIT** — 1 is safe in main |
| 7 | 5 | | `5 4` | `1(f1)` | `{2,3}` | miss — evicts 3 → ghost |
| 8 | **1** | | `5 4` | `1(f1)` | `{2,3}` | **HIT** |

**3 hits.** At step 5, page 1 earned its promotion to main by being accessed while in small, and
from then on it's insulated from the churn of one-hit wonders cycling through small. Meanwhile
pages 2 and 3 died in small — exactly as intended — without ever touching main.

**The cost:** three queues + a ghost hash lookup on every miss makes it our **slowest** policy
(5.8 M ops/sec vs FIFO's 9.1 M). That's the price of scan resistance, and per §2 it's a price worth
paying whenever real I/O is involved.

---

## 4. Reading our actual charts

Run `.\build\runner.exe` then `.\venv\Scripts\python.exe plot.py`. Full tables in
[`results/summary.md`](results/summary.md). All curves are **miss ratio (lower = better) vs. cache
size**, so every curve slopes **down** — a bigger cache always helps. What matters is the *gaps*.

### 4.1 `zipf_scan_mix` — the headline

Zipfian hot traffic, interrupted every 800 requests by a 200-page sequential scan of cold data.

| cache | FIFO | LRU | CLOCK | SIEVE | S3-FIFO |
|---|---|---|---|---|---|
| 100 | 0.2937 | 0.2937 | 0.2937 | **0.2087** | **0.2134** |
| 200 | 0.2937 | 0.2937 | 0.2937 | **0.2065** | **0.2059** |

**Why FIFO, LRU and CLOCK are byte-identical (14127 hits / 5873 misses, exactly).** This is not a
bug — I checked. A 200-page scan burst into a ≤200-frame cache **flushes it completely**. Whatever
subtle differences those three policies have are irrelevant, because after each burst all three are
in the identical state: holding nothing but scan pages. They've collapsed into the same "cache
totally destroyed" regime. Their curves lie exactly on top of each other.

**Why SIEVE/S3-FIFO hold at ~0.206.** The scan pages are textbook one-hit wonders. SIEVE inserts
them with `visited=0`, so the hand kills them almost immediately (quick demotion). S3-FIFO never
even lets them into main. The hot set survives.

**Why ~0.206 and not lower — the floor.** 20 % of requests *are* scan pages, each seen exactly
once. A cold page's first access is a **compulsory miss** — no policy can avoid it. So 0.20 is the
theoretical floor, and SIEVE/S3-FIFO are within ~0.006 of perfect. They're not "somewhat better",
they're **nearly optimal**, while LRU is 47 % above the floor.

### 4.2 Zipfian (α = 0.6 / 0.8 / 1.0)

At α = 1.0, cache = 100: SIEVE **0.3380**, S3-FIFO **0.3452**, LRU 0.4250, CLOCK 0.4378, FIFO
0.4798. SIEVE/S3-FIFO win at **every** cache size and every α — this reproduces both papers' central
claim. As α rises (more skew, hotter hot set) every curve drops, because a small hot set is easier
to cache. The SIEVE/S3-FIFO gap over LRU comes from quick demotion: the long tail of rarely-touched
pages gets flushed fast instead of being ranked politely.

### 4.3 `hot_set_shift` — where SIEVE *loses*, and why

The hot set is 150 pages, and it **jumps to a different 150 pages** every 4000 requests.

| cache | FIFO | LRU | SIEVE |
|---|---|---|---|
| 200 | 0.2326 | **0.1356** | 0.2526 ← worse than FIFO |
| 400 | 0.1022 | **0.0942** | 0.1461 |

SIEVE loses to *plain FIFO*. This looked like a bug, so I isolated it: same generator, but with the
hot set **static** instead of shifting.

| hot set | cache | FIFO | LRU | SIEVE |
|---|---|---|---|---|
| **static** | 200 | 0.2251 | 0.1103 | **0.0924** ← SIEVE *wins* |
| **shifting** | 200 | 0.2326 | 0.1356 | 0.2526 ← SIEVE loses |

**Same code, same cache, opposite verdict.** So it's not a bug — it's **lazy promotion's bill**.
SIEVE never moves anything, so a page that was hot but has gone cold keeps `visited=1` and sits
exactly where it always sat. The hand must lap the queue twice to evict it. LRU reorders on every
access, so a stale page slides to the LRU end and dies at once. S3-FIFO (0.1499) largely escapes
this because the ghost queue gives it an adaptive re-admission path.

Both papers evaluate on skewed, fairly **stable** web-cache traces — precisely the regime where
this weakness never appears. This is the most interesting result in the project: a real, measured
limitation of a 2024 paper, explained by its own mechanism.

### 4.4 `sequential_scan` — a flat line at 1.0

Every policy, every cache size: **miss ratio exactly 1.0000.** 1000 distinct pages scanned in a
loop, largest cache 400 frames — by the time page 0 comes round again, 999 other pages have
demanded a frame, so it's always gone. **No policy can win**; the access pattern has zero locality
to exploit. Cache the wrong 400 pages, and you cache the wrong 400 pages.

This is a real result (it confirms "a pure scan defeats everything") but a **useless chart**, and
it's exactly why `zipf_scan_mix` had to be added: a pure scan has no hot set to *pollute*, so it
cannot demonstrate scan **resistance**. That distinction is worth being able to make out loud.

---

## 5. The C++ decisions (interviewers will ask)

### 5.1 "Why no `new`/`delete`? Why integer indices instead of pointers?"

Every list in this project — LRU's queue, SIEVE's queue, S3-FIFO's small/main — is an **intrusive
doubly linked list built out of a preallocated `std::vector<Node>`, linked by `int` indices** with
`-1` as null, rather than by pointers:

```cpp
struct Node { page_id_t page_id; int prev; int next; bool visited; };
std::vector<Node> nodes_;   // sized to capacity ONCE, in the constructor
int head_ = -1, tail_ = -1, hand_ = -1;
```

Four honest reasons:

1. **The cache has a fixed capacity, so the node count is known up front.** A buffer pool with
   1000 frames needs at most 1000 nodes — ever. Allocating them one at a time would be pure waste.
   `nodes_.resize(capacity)` in the constructor, then **zero allocation for the rest of the
   program's life.** No `new` on the hot path is a real database requirement, not a stunt: an
   allocator call in your eviction path is a latency spike and a lock.
2. **No lifetime bugs, by construction.** There is no `delete`, so there is no use-after-free, no
   double-free, no leak. A stale index is at worst *wrong*; a stale pointer is *undefined
   behaviour*. Debugging a corrupt intrusive list at 2 a.m. is a rite of passage I'd rather skip.
3. **Cache locality.** All nodes are contiguous in one allocation. Walking SIEVE's hand touches
   neighbouring memory, which the CPU prefetcher loves. Pointer-chasing through
   individually-`new`'d nodes scattered across the heap is how you turn an O(1) algorithm into a
   cache-miss festival.
4. **Half the memory.** An `int` is 4 bytes; a pointer is 8 on x64.

The "free list" is just `std::vector<int> free_` of unused node slots — `pop_back()` to allocate,
`push_back()` to free. That's the entire memory manager.

> If asked "isn't `-1` a magic number?" — it's the same sentinel as `INVALID_PAGE`, one concept
> reused for both "no such page" and "no such node", declared once in `policy.h`.

### 5.2 "Why a virtual base class?"

[`EvictionPolicy`](include/policy.h) is an abstract base with four virtual methods.
`BufferPool` holds an `EvictionPolicy*` and never knows which policy it has. That's the "pluggable"
in the project title: swapping FIFO for S3-FIFO changes **one line** of the benchmark, and zero
lines of `BufferPool`.

Yes, a virtual call costs an indirect jump and blocks inlining. **That cost is deliberate and
irrelevant here**, and §2 has the numbers to prove it: policy overhead is ~0.018 µs against ~8.7 µs
of I/O difference. Paying a vtable lookup to make the *comparison itself* possible is the entire
point of the project. (If you needed it inlined, you'd make the policy a template parameter of
`BufferPool` and lose runtime swappability — that's the real trade, and it's worth saying out loud.)

Note also what's **not** virtual: `set_pinned`/`is_pinned` are concrete in the base, because all
five policies handle pinning identically. Shared behaviour goes in the base; only genuinely
divergent behaviour is virtual.

### 5.3 "What does the ops/sec number actually measure?"

Exactly one thing: **policy bookkeeping cost**, with disk I/O switched off
(`set_io_enabled(false)`), timed with `std::chrono::steady_clock`, compiled `-O2`.

| policy | ops/sec | why |
|---|---|---|
| FIFO | 9.1 M | `on_access` is an empty function |
| SIEVE | 8.7 M | a hit is one bit write |
| CLOCK | 7.6 M | one bit write + hand sweep on eviction |
| LRU | 7.5 M | unlink + relink on **every hit** |
| S3-FIFO | 5.8 M | three queues + ghost hash lookup per miss |

The SIEVE-vs-LRU gap (~16 %) is the concrete, measured cost of LRU's list splice. That's the number
that makes "lazy promotion is cheaper" a *fact* rather than a claim.

Three caveats worth volunteering before someone else raises them: it's **single-threaded**, so it
misses LRU's real-world killer (lock contention on the shared list, which would widen this gap a
lot); `steady_clock` on one machine is not a rigorous benchmark; and per §2, this number is nearly
irrelevant next to miss ratio unless everything fits in RAM.

---

## 6. How to present this

### The 30-second pitch

> "I built a database buffer pool manager in C++ — the component that caches disk pages in memory —
> with five pluggable eviction policies behind one interface: FIFO, LRU, CLOCK, and two recent
> research algorithms, SIEVE from NSDI 2024 and S3-FIFO from SOSP 2023. I benchmarked all five
> across 150 configurations. The headline result is that under scan pollution, SIEVE and S3-FIFO cut
> the miss ratio about 30 % versus LRU — they get within a fraction of the theoretical floor while
> LRU sits 47 % above it. I also found a case where SIEVE loses to plain FIFO, and traced it to a
> real limitation of its design."

### The 2-minute pitch

Add, in this order:

1. **The problem.** A DB is bigger than RAM; an SSD read is ~1000× a memory read. The buffer pool
   caches 4 KB pages in frames, with a hash page table and pin counts. Miss ratio *is* latency.
2. **The interesting bit.** The only real decision is *who gets evicted*. LRU is the textbook
   answer, but it does list surgery on every hit and one sequential scan wipes out your whole
   working set.
3. **The new algorithms.** SIEVE is CLOCK with two changes: new pages get no free pass, and the
   hand persists between evictions. That gives *lazy promotion* (a hit is one bit write, nothing
   moves) and *quick demotion* (one-hit wonders die fast). S3-FIFO adds a small/main/ghost queue
   split on the observation that most objects are requested exactly once.
4. **The results, with numbers.** 30 % on scan pollution, ~20 % on Zipfian, SIEVE ~16 % faster than
   LRU on top.
5. **The honest part — lead with this if you can.** "One workload contradicted the paper: SIEVE lost
   to FIFO when the working set shifted. I isolated it by re-running with a static hot set, where
   SIEVE beat everything — so it wasn't a bug, it was lazy promotion's cost. Because SIEVE never
   reorders, a stale hot page needs two hand laps to evict. The papers evaluate on stable traces, so
   it never shows up there."

That last point is the one that separates you from everyone who reimplemented an algorithm. It
shows you *measured*, *disbelieved your own result*, *isolated the variable*, and *explained the
mechanism.* **Practise saying it in 30 seconds.**

### The 5 design decisions to raise *before* you're asked

1. **Policies see `page_id`s only** — never frames, never disk. Four-method interface. That's what
   makes them swappable and makes the ops/sec comparison fair.
2. **Zero heap allocation** — all frames and nodes preallocated in `std::vector`s, linked by `int`
   index. No `new`, no smart pointers, no lifetime bugs, better locality, half the memory.
3. **Virtual dispatch is a deliberate cost** — and I have the numbers showing it's ~480× smaller
   than the effect I'm measuring.
4. **Correctness before benchmarks** — every policy is tested against eviction traces I computed by
   hand *before* writing the code, plus invariants (never exceed capacity; never evict a pinned
   page) run against all five. A silent policy bug would have quietly poisoned every number.
5. **I added a workload because the original couldn't test the claim.** Pure `sequential_scan` puts
   every policy at 100 % miss — it proves a scan defeats everything, but it can't show scan
   *resistance*, because there's no hot set to pollute. `zipf_scan_mix` was built for that, and it's
   where the headline result came from.

---

## 7. The 10 concepts to define cold

Be able to say each in 1–2 sentences, **without notes**:

1. **Buffer pool** — the DB's own in-memory cache of fixed-size disk pages, and why a DB has one
   instead of using the OS page cache (the OS can't see a scan for what it is, and can't guarantee
   the write ordering WAL requires).
2. **Page vs. frame vs. page table** — a page is a 4 KB chunk *on disk*; a frame is a 4 KB slot *in
   memory*; the page table is the hash map `page_id → frame index` between them.
3. **Pin count** — a counter of active users of a frame; a page with `pin_count > 0` can never be
   evicted, because someone is reading that memory right now.
4. **Dirty page / write-back** — a frame modified since it was loaded; it must be written to disk
   before its frame is reused, or the change is lost.
5. **Miss ratio** — misses ÷ total requests. The metric that matters, because a miss is a disk I/O
   and a disk I/O is ~1000× a memory hit.
6. **Compulsory miss** — the unavoidable first access to a page. It sets the floor no policy can
   beat (our `zipf_scan_mix` floor of ~0.20).
7. **Scan pollution / sequential flooding** — one large sequential scan evicts the entire hot
   working set, because every scan page looks "recently used" to LRU while being useless.
8. **Reference bit / second chance** — CLOCK's one-bit approximation of recency: set on hit, cleared
   by the passing hand; the first page found with a clear bit is evicted. When every bit is set,
   CLOCK degenerates to FIFO.
9. **Lazy promotion** — SIEVE's idea: a hit only sets a bit, the object **never moves**. Cheap (one
   write, no lock) — but the cost is slow adaptation when the working set shifts, which we measured.
10. **Quick demotion & one-hit wonders** — most cached objects are requested exactly once, so the
    cache's real job is evicting them *fast*, not ranking survivors precisely. SIEVE does it by
    inserting unprotected; S3-FIFO does it by making everything start in a small probation queue and
    recording evicted keys in a ghost queue so a genuine re-request can be re-admitted straight to
    main.

---

*Everything above is reproducible: `.\build.ps1` (36 tests), `.\build\runner.exe` (the sweep),
`.\venv\Scripts\python.exe plot.py` (the charts).*
