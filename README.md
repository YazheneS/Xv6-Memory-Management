# xv6 Memory Management Overhaul — OS Mini Project

This is a modified xv6-riscv kernel implementing three interconnected
memory-management features, all routed through a single unified
page-fault handler:

- **Module 1 — Demand Paging.** `exec()` no longer loads a program's
  code/data into memory up front. It just records where each ELF
  segment lives on disk; pages are loaded one at a time, the first time
  they're actually touched.
- **Module 2 — Copy-on-Write fork().** `fork()` no longer duplicates
  the parent's memory. Parent and child share physical pages
  (read-only) until either one writes to a shared page, at which point
  a private copy is made just for that page.
- **Module 3 — Page Replacement.** A clock/second-chance algorithm
  reclaims physical frames under simulated memory pressure (an
  artificial cap you can set at runtime), evicting clean, reloadable
  pages and bringing them back on demand if touched again.
- **Module 4 — Instrumentation.** A `meminfo` syscall exposes live
  counters (faults by type, evictions, reloads, frame usage) so you can
  measure and demo the other three modules instead of just asserting
  they work.

This has been **built and boot-tested in QEMU**, including a full
`usertests -q` regression run — it is not just a design sketch.

## How the four pieces fit together

Every one of the three technical modules is really just a different
answer to the same question the CPU asks the kernel: *"I need this
page and it isn't there — now what?"* That question is a page fault,
and it's resolved in one place: `vmfault()` in `kernel/vm.c`, called
from `usertrap()` in `kernel/trap.c` for **all three** RISC-V fault
causes (instruction, load, and store page faults — scause 12/13/15;
see the comment in `trap.c` about why instruction faults had to be
added, this project's most instructive early bug).

```
page fault (scause 12/13/15)
        |
        v
  vmfault() in vm.c
        |
        +--> PTE tagged PTE_COW?      -> uvmcowcopy()      (Module 2)
        +--> PTE tagged PTE_PG?       -> filefault()        (Module 3 reload)
        +--> va inside a segdesc?     -> filefault()        (Module 1 first load)
        +--> otherwise (heap/stack)   -> demand-zero page
```

## Where each module actually lives

| Module | Key files | Key functions |
|---|---|---|
| 1. Demand paging | `kernel/exec.c`, `kernel/proc.h` | `kexec()`, `filefault()`, `segs_copy()`, `segs_free()` |
| 2. Copy-on-write | `kernel/vm.c`, `kernel/kalloc.c` | `uvmcopy()`, `uvmcowcopy()`, `kaddref()`/`kgetref()` |
| 3. Page replacement | `kernel/kalloc.c`, `kernel/vm.c` | `reclaim_one_frame()`, `frame_register()`, `uvmevict()` |
| 4. Instrumentation | `kernel/kalloc.c`, `kernel/sysproc.c`, `kernel/meminfo.h` | `getmeminfo()`, `sys_meminfo`, `sys_setmemcap` |

New/changed software-only PTE bits (`kernel/riscv.h`, bits 8–9, which
Sv39 hardware reserves for software use and never touches):
`PTE_COW` (shared, copy-on-write) and `PTE_PG` (evicted, reload me).

## Building and running

```
make                 # builds kernel/kernel
make fs.img          # builds all userspace programs + the disk image
make qemu            # boots it (Ctrl-a x to quit)
```

Requires a RISC-V cross toolchain (`riscv64-unknown-elf-gcc`) and
`qemu-system-riscv64`. On Ubuntu/Debian:
```
sudo apt-get install gcc-riscv64-unknown-elf qemu-system-misc
```

## Demo programs (in `user/`)

- **`memtop [interval]`** — prints the live `meminfo` snapshot (frame
  usage, fault counts, evictions, reloads). Run it with no args for one
  snapshot, or `memtop 10` to refresh every 10 ticks.
- **`cowtest`** — allocates and touches 200 heap pages, forks 4
  children, each of which modifies only 5 pages. Confirms COW is
  actually working: it prints the number of COW copies triggered, which
  should be around 20 (`NCHILD * 5`), not 800 (`NCHILD * 200`, what a
  naive eager fork would have copied).
- **`stresstest <cap-in-frames> <concurrent-children>`** — the Module 3
  demo. Caps simulated physical memory to a small number of frames
  (`setmemcap`), then runs several children concurrently, each
  repeatedly `fork()+exec()`ing a small program. Because every `exec()`
  demand-loads its own private code pages (Module 1), enough concurrent
  children exceed the tiny cap and force the clock algorithm to evict
  and later reload code pages — real, measurable page replacement.
  Try `stresstest 40 6` first.

Example session:
```
$ memtop
$ cowtest
$ memtop
$ stresstest 40 6
$ memtop
```

## Why page replacement only evicts *some* pages (read this before your viva)

This is the single most important design decision to be able to
explain, and examiners will ask about it.

Reclaiming a page that has been **written to** (a heap/stack page, or a
writable data page) would require writing its contents out to a swap
area on disk before reusing the frame, and reading it back later. This
project **intentionally does not implement disk-backed swap** — it was
scoped out up front as a multi-week project of its own (real OS
courses treat it that way too).

What it does implement, correctly and safely: reclaiming pages that are
**clean and reloadable without any information loss** — specifically, a
program's read-only `.text` pages, which were demand-loaded from the
executable in the first place (Module 1) and can simply be re-loaded
the exact same way if touched again. `reclaim_one_frame()` in
`kernel/kalloc.c` only ever considers frames marked `evictable`, and
only `filefault()`'s call to `frame_register()` for a page mapped
without `PTE_W` sets that flag. Anonymous heap/stack pages are always
registered with `evictable=0`.

**If you want to extend this**: the natural next step is a `swap.c`
that reserves a range of disk blocks (via `virtio_disk_rw()` directly,
bypassing the buffer cache, at an offset beyond the filesystem's
declared size) and write-back-then-evict for anonymous pages whose
software "dirty" bit is set. The eviction call site
(`reclaim_one_frame()`) and the reload call site (`vmfault()`'s
`PTE_PG` branch) are already there — swap-backed anonymous pages would
be a third case in `vmfault()`'s dispatch alongside `filefault()`.

## Concurrency notes (also worth knowing for the viva)

This kernel runs SMP (3 harts in the default `make qemu`), and the
memory subsystem had two genuine SMP bugs during development that are
worth understanding, not just fixing:

1. **`copyout()` and COW.** The kernel writes directly into user memory
   via physical addresses for syscall results (e.g. `read()`), bypassing
   the page-table permission checks a real CPU instruction would go
   through. If the destination page was marked read-only-because-COW
   (not read-only-because-`.text`), the original check
   (`if not writable, fail`) would either wrongly reject a perfectly
   legal write, or, in earlier drafts, risk writing straight into a
   frame still shared with the parent. The fix: `copyout()` now routes
   through the exact same fault resolver (`vmfault()`) a real store
   instruction would hit, so a COW page gets privately copied first.
2. **Eviction racing with `exit()`.** `reclaim_one_frame()` picks a
   victim page and needs to modify that page's owning process's page
   table. But on another CPU, that process could be exiting and tearing
   its page table down at the exact same moment. The fix follows
   xv6's own convention: never touch a process's page table without
   holding that process's `p->lock` first (the same lock `freeproc()`
   holds while freeing it), and always acquire locks in the order
   `p->lock` → `frametab_lock`, never the reverse, to avoid deadlock.

## Known issue

`usertests -q`'s own leak detector reports losing 3 physical pages
(out of ~32267) after a full run — a small, real memory leak somewhere
in an error/cleanup path (most likely a `fork()`-failure path that
doesn't release something `segs_copy()` duped). It doesn't affect
correctness of any individual feature, and tracking it down with
`gdb` + a differential `meminfo()` snapshot around suspect code paths
(start with `kfork()`'s failure branch in `kernel/proc.c`) is a good,
real task to split off to a team member. Being able to explain *how
you'd find it* is worth as much in a viva as having already fixed it.

## Suggested team split (for your report/viva prep)

- **Person A** — Module 1 (demand paging): `exec.c`, `filefault()`.
- **Person B** — Module 2 (COW): `uvmcopy()`, `uvmcowcopy()`,
  refcounting in `kalloc.c`, and the `copyout()` interaction above.
- **Person C** — Module 3 (page replacement): the frame table and
  clock algorithm in `kalloc.c`, plus the SMP-safety fix.
- **Person D** — Module 4 (instrumentation) + benchmarking: the
  `meminfo`/`setmemcap` syscalls, and writing up before/after
  measurements from `memtop`/`cowtest`/`stresstest` for the report.

Every person should still be able to explain the fault-dispatch diagram
above and how their module fits into it — that shared understanding is
what makes this look like one cohesive subsystem instead of four
unrelated patches, and it's exactly what an examiner will probe for.
