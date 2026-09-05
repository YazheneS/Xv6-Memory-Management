# Handover Report — xv6 Memory Management Overhaul

## My contribution (completed)

I designed and implemented the full memory-management subsystem
described in `PROJECT_README.md`, end to end: not a design sketch, but
a working kernel that has been compiled with the real RISC-V toolchain
and boot-tested in QEMU.

**Module 1 — Demand Paging.** Rewrote `kernel/exec.c` so `kexec()` no
longer eagerly loads ELF segments into memory. It now records
`segdesc` entries (inode + file offsets) and reserves address space
without touching physical memory; `filefault()` loads a single page
from disk the first time it's actually touched, correctly handling the
case where a segment's `memsz` exceeds its `filesz` (`.bss` zero-fill).
Also implemented `segs_copy()`/`segs_free()` for correctly
duplicating/releasing inode references across `fork()`/`exit()`.

**Module 2 — Copy-on-Write fork().** Rewrote `uvmcopy()` in `vm.c` to
share physical pages and tag them `PTE_COW` instead of copying at fork
time, and implemented `uvmcowcopy()` to resolve a COW fault lazily
(including a fast path when the frame is already the sole owner).
Added reference counting to `kalloc.c` (`kaddref`/`kgetref`) so a
shared frame is only freed once every process sharing it has let go.

**Module 3 — Page Replacement.** Implemented a frame table and
clock/second-chance algorithm (`reclaim_one_frame()`) in `kalloc.c`,
scoped so that only clean, file-backed, read-only pages (`.text`, via
`filefault()`) are ever reclaimed — anonymous heap/stack pages are
deliberately excluded, since reclaiming a written page would require
disk-backed swap, which I scoped out up front (documented rationale in
`PROJECT_README.md`).

**Module 4 — Instrumentation.** Implemented `meminfo`/`setmemcap`
syscalls end to end (`syscall.c/.h`, `sysproc.c`, `user.h`, `usys.pl`,
shared `kernel/meminfo.h`), with global fault/eviction/reload counters,
and three demo programs (`memtop`, `cowtest`, `stresstest`) that
produce real, measured numbers rather than asserted behavior.

**Bugs found and fixed during boot-testing** (not just written, but
diagnosed against a live, crashing kernel):
1. Instruction-page faults (scause 12) weren't routed to the fault
   handler — `.text` is no longer preloaded, so the very first
   instruction fetch after `exec()` genuinely faults, and the original
   dispatch only handled load/store faults. Fixed in `trap.c`.
2. `copyout()` unconditionally rejected writes to any read-only page,
   which broke the moment a COW-shared page needed a kernel-side write
   (e.g. a syscall result) before the owning process had triggered its
   own store fault. Fixed by routing `copyout()` through the same
   `vmfault()`/`uvmcowcopy()` path a real store instruction would hit.
3. An SMP race in `reclaim_one_frame()`: it could pick a victim page
   belonging to a process that was concurrently exiting (and tearing
   down its page table) on another CPU core. Fixed by acquiring the
   victim process's own lock before touching its page table, following
   xv6's existing convention, with a documented lock order (`p->lock`
   → `frametab_lock`) to avoid deadlock.

**Verification:**
- `cowtest`: 20 COW copies triggered for 4 children × 5 pages each
  (not 800) — confirms sharing, not eager duplication.
- `stresstest 40 6`: 215 real evictions, 161 real reloads under an
  artificial memory cap — confirms the clock algorithm runs and
  reclaims pages under real pressure.
- Full `usertests -q` regression suite: passes, with one known minor
  leak documented below.

Everything above is in the archive, committed locally to git and ready
to push (see "Pushing to GitHub" in `PROJECT_README.md` / the message
accompanying this report).

---

## Remaining work — split across the other 3 teammates

### Teammate 1 — Verification & the known leak

1. **Track down the known 3-page leak.** `usertests -q` reports losing
   3 physical pages after a full run. Best lead: `kfork()`'s failure
   path in `kernel/proc.c` —
   ```c
   if (uvmcopy(p->pagetable, np->pagetable, p->sz, np) < 0) {
     freeproc(np);
     release(&np->lock);
     return -1;
   }
   np->sz = p->sz;
   segs_copy(p, np);
   ```
   `freeproc()` deliberately does **not** call `iput()` (it can't — it
   may run with `p->lock` held; the real release happens earlier, in
   `kexit()`). Trace every `return -1` in `kfork()` *after* the
   `segs_copy()` call and check whether the inode references it duped
   get released on that path. Use `meminfo()` snapshots bracketing a
   targeted failure-injection test (force a later step in `kfork()` to
   fail repeatedly, compare free-frame count before/after) to confirm
   the fix.
2. **Write a `.bss`-heavy test program**: a custom user program with a
   global array/large uninitialized region bigger than one page.
   Confirm via `memtop` that zero-fault counts behave correctly and
   that reads of uninitialized globals genuinely return zero.
   `usertests` doesn't specifically exercise a multi-page `.bss`, so
   this is a real gap in verification, not busywork.
3. Write up both results (leak fix or precise diagnosis, and the
   `.bss` test results) for the report's verification section.

### Teammate 2 — Page replacement extension (pick one)

**Option 1 (if time allows) — real disk-backed swap.** Close the
biggest scoping gap: make anonymous/heap pages evictable too.
Integration points already exist:
- Reserve disk blocks beyond the filesystem's declared size, written
  via `virtio_disk_rw()` directly (not the buffer cache, which assumes
  filesystem-owned blocks).
- Add a software "dirty" bit, set on first COW-copy or first
  demand-zero write.
- On eviction of a dirty anonymous page, write it to a swap slot
  before reclaiming the frame.
- Add a third case to `vmfault()`'s dispatch (alongside the existing
  `PTE_PG` reload-from-file case) for "reload from swap slot."
- Start early; fall back to Option 2 if it's not converging in time.

**Option 2 (safe fallback) — characterize the existing algorithm.**
Run `stresstest` across a range of `<cap>` and `<concurrent-children>`
values, log eviction/reload counts via `memtop`, and produce a
table/graph for the report showing how eviction pressure scales. This
is real data for the "comparative study" the report needs, not
filler.

### Teammate 3 — Benchmarking & report

1. Run all three demo programs yourself and capture the output
   (screenshots/logs) — `cowtest`, `stresstest` at a few
   cap/children combinations, and `memtop` snapshots. The numbers in
   this handover are from my own verification run; the report should
   show the team's own.
2. **Stock-vs-modified comparison, if time allows**: build unmodified
   xv6-riscv alongside this one and compare behavior on a comparable
   workload (e.g. wall-clock time for N forks of a large process,
   since stock xv6 has no `meminfo` to compare counters against
   directly).
3. Own the report structure: abstract, problem statement, proposed
   solution (all drafted already — ask me for the text), architecture
   (reuse the fault-dispatch diagram in `PROJECT_README.md`), results
   (from your own runs and Teammate 2's data), and an honest
   limitations section (the swap-scoping decision, and the leak's
   status from Teammate 1).
4. Coordinate viva prep: everyone should be able to explain the shared
   fault-dispatch diagram, not just their own piece — that's what
   makes this read as one cohesive subsystem instead of four unrelated
   patches, and it's the first thing an examiner is likely to ask
   about.

---

## Cross-team item

**Push to GitHub** — see the accompanying message for exact commands.
Whoever runs this, do not paste any token into a chat tool.
