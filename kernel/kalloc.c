// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
//
// --- OS mini-project: xv6 Memory Management Overhaul ---
// This file additionally implements:
//   Module 2 (COW):  per-physical-frame reference counts, so a page can
//                     be safely shared by several processes and only
//                     freed once nobody points at it any more.
//   Module 3 (page replacement): a frame table recording who owns each
//                     mapped user frame, plus a clock/second-chance
//                     algorithm that reclaims a frame when the
//                     artificial memory cap (memcap) is hit.
//   Module 4 (instrumentation): global fault/eviction counters exposed
//                     to userspace via the meminfo syscall.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// ---------------------------------------------------------------------
// Module 2: per-frame reference counts (needed so COW-shared pages are
// only freed once every process sharing them has let go).
// ---------------------------------------------------------------------
static struct spinlock reflock;
static uint8 refcount[NFRAMES];

#define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

// ---------------------------------------------------------------------
// Module 3: frame table + clock hand for page replacement.
// Only frames that were explicitly frame_register()'d (i.e. user pages
// backing a process's address space) are ever considered for eviction;
// kernel stacks, page-table pages, pipe buffers, etc. are left alone
// because their "used" flag is simply never set.
// ---------------------------------------------------------------------
struct frameinfo {
  uint8 used;      // 1 if this frame currently backs a mapped user page
  uint8 evictable; // 1 if it is safe to reclaim (clean + reloadable)
  uint8 accessed;  // software-approximated "recently used" bit
  uint8 kind;      // FRAME_ANON or FRAME_FILE
  struct proc *p;  // owning process
  uint64 va;       // owning virtual address (page-aligned)
};

static struct spinlock frametab_lock;
static struct frameinfo frametab[NFRAMES];
static uint clockhand;

// ---------------------------------------------------------------------
// Module 4: global counters + the artificial memory cap used to
// simulate a machine with less physical memory than QEMU actually
// gives us, so eviction (and, if you push it far enough, thrashing)
// can be triggered and demonstrated on demand.
// ---------------------------------------------------------------------
static struct spinlock statlock;
static uint allocated_frames; // frames currently handed out by kalloc()
static uint frame_cap;        // 0 means "no artificial cap"
static uint stat_zerofaults, stat_filefaults, stat_cowfaults;
static uint stat_evictions, stat_reloads;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&reflock, "refcnt");
  initlock(&frametab_lock, "frametab");
  initlock(&statlock, "memstat");
  frame_cap = 0; // uncapped until a demo asks for setmemcap()
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // COW bookkeeping: if this frame is still referenced by someone else
  // (i.e. it is shared and this is not the last owner), just drop our
  // share and don't actually free the underlying memory yet.
  acquire(&reflock);
  uint64 idx = PA2IDX(pa);
  if (refcount[idx] > 1) {
    refcount[idx]--;
    release(&reflock);
    return;
  }
  refcount[idx] = 0;
  release(&reflock);

  frame_unregister((uint64)pa);

  acquire(&statlock);
  if (allocated_frames > 0)
    allocated_frames--;
  release(&statlock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // Module 3: if we've been asked to simulate a smaller machine and
  // we're at the cap, try to reclaim a frame before giving up.
  acquire(&statlock);
  int need_reclaim = (frame_cap != 0 && allocated_frames >= frame_cap);
  release(&statlock);
  if (need_reclaim) {
    reclaim_one_frame(); // best-effort; if it fails we may still have
                         // real free frames below (cap was aspirational)
  }

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r == 0)
    return 0;

  memset((char *)r, 5, PGSIZE); // fill with junk

  acquire(&reflock);
  refcount[PA2IDX(r)] = 1;
  release(&reflock);

  acquire(&statlock);
  allocated_frames++;
  release(&statlock);

  return (void *)r;
}

// ---------------- Module 2: COW reference counting -------------------

void
kaddref(uint64 pa)
{
  acquire(&reflock);
  refcount[PA2IDX(pa)]++;
  release(&reflock);
}

int
kgetref(uint64 pa)
{
  int n;
  acquire(&reflock);
  n = refcount[PA2IDX(pa)];
  release(&reflock);
  return n;
}

// ---------------- Module 3: frame table + clock algorithm ------------

void
frame_register(uint64 pa, struct proc *p, uint64 va, int kind, int evictable)
{
  uint64 idx = PA2IDX(pa);
  acquire(&frametab_lock);
  frametab[idx].used = 1;
  frametab[idx].evictable = (uint8)evictable;
  frametab[idx].accessed = 1;
  frametab[idx].kind = (uint8)kind;
  frametab[idx].p = p;
  frametab[idx].va = va;
  release(&frametab_lock);
}

void
frame_unregister(uint64 pa)
{
  uint64 idx = PA2IDX(pa);
  acquire(&frametab_lock);
  frametab[idx].used = 0;
  frametab[idx].evictable = 0;
  frametab[idx].accessed = 0;
  frametab[idx].p = 0;
  frametab[idx].va = 0;
  release(&frametab_lock);
}

void
frame_touch(uint64 pa)
{
  uint64 idx = PA2IDX(pa);
  acquire(&frametab_lock);
  frametab[idx].accessed = 1;
  release(&frametab_lock);
}

// Run one pass of the clock (second-chance) algorithm: sweep the frame
// table starting from clockhand, giving accessed frames a second chance
// (clear the bit and move on) and reclaiming the first unaccessed,
// evictable frame we find. The victim's user mapping is invalidated
// (PTE_V cleared, PTE_PG set so the next access is recognised as "please
// reload me") and its physical frame is freed back to the allocator.
//
// Returns 1 if a frame was reclaimed, 0 if no evictable victim was found
// (e.g. every resident page is currently non-evictable anonymous memory
// -- a legitimate "thrashing / can't reclaim any further" state worth
// showing in the report).
int
reclaim_one_frame(void)
{
  acquire(&frametab_lock);

  uint scanned = 0;
  int victim = -1;

  while (scanned < 2 * NFRAMES) {
    uint i = clockhand;
    clockhand = (clockhand + 1) % NFRAMES;
    scanned++;

    if (!frametab[i].used || !frametab[i].evictable)
      continue;

    // Safety net: never evict a frame that is still shared (refcount>1)
    // -- uvmevict() only clears the PTE in *one* page table, so evicting
    // a shared frame would leave other processes pointing at memory we
    // are about to hand back to the allocator.
    {
      uint64 pa_check = KERNBASE + (uint64)i * PGSIZE;
      if (kgetref(pa_check) != 1)
        continue;
    }

    if (frametab[i].accessed) {
      // second chance: pretend we haven't seen it recently any more
      frametab[i].accessed = 0;
      continue;
    }

    victim = (int)i;
    break;
  }

  if (victim < 0) {
    release(&frametab_lock);
    return 0; // nothing evictable right now
  }

  struct proc *victim_p = frametab[victim].p;
  uint64 victim_va = frametab[victim].va;
  uint64 victim_pa = KERNBASE + (uint64)victim * PGSIZE;

  // Do NOT claim the frame yet. We must not touch victim_p->pagetable
  // without holding victim_p->lock first -- on SMP, that process could
  // be exiting on another CPU *right now*, and freeproc() tears its
  // page table down while holding exactly that lock. Grabbing
  // frametab_lock and then a proc lock would risk an AB-BA deadlock
  // with that path (which takes p->lock, then frametab_lock inside
  // kfree()/frame_unregister()), so we release frametab_lock first and
  // re-acquire the two locks in the same order everyone else uses:
  // p->lock, then frametab_lock.
  release(&frametab_lock);

  acquire(&victim_p->lock);

  acquire(&frametab_lock);
  int still_valid =
    frametab[victim].used && frametab[victim].p == victim_p &&
    frametab[victim].va == victim_va &&
    (victim_p->state == RUNNING || victim_p->state == RUNNABLE ||
     victim_p->state == SLEEPING);
  if (still_valid) {
    frametab[victim].used = 0;
    frametab[victim].evictable = 0;
    frametab[victim].p = 0;
    frametab[victim].va = 0;
  }
  release(&frametab_lock);

  if (!still_valid) {
    // Lost the race: the page was already reclaimed/reused by someone
    // else, or its owning process exited/was reaped in the meantime.
    // Give up on this attempt rather than risk touching a torn-down
    // page table; the caller (kalloc) will simply see less memory
    // freed this round.
    release(&victim_p->lock);
    return 0;
  }

  // Safe now: we hold victim_p->lock, so victim_p->pagetable cannot be
  // freed out from under us (freeproc() needs the same lock).
  uvmevict(victim_p->pagetable, victim_va);
  release(&victim_p->lock);

  kfree((void *)victim_pa);

  acquire(&statlock);
  stat_evictions++;
  release(&statlock);

  return 1;
}

void
setmemcap(int frames)
{
  acquire(&statlock);
  frame_cap = (frames <= 0) ? 0 : (uint)frames;
  release(&statlock);
}

int
getmemcap(void)
{
  int c;
  acquire(&statlock);
  c = (int)frame_cap;
  release(&statlock);
  return c;
}

// ---------------- Module 4: instrumentation ---------------------------
// Callers elsewhere (vmfault/filefault/uvmcowcopy) bump these directly
// via the small helper functions below.

void
memstat_zerofault(void)
{
  acquire(&statlock);
  stat_zerofaults++;
  release(&statlock);
}

void
memstat_filefault(void)
{
  acquire(&statlock);
  stat_filefaults++;
  release(&statlock);
}

void
memstat_cowfault(void)
{
  acquire(&statlock);
  stat_cowfaults++;
  release(&statlock);
}

void
memstat_reload(void)
{
  acquire(&statlock);
  stat_reloads++;
  release(&statlock);
}

void
getmeminfo(struct meminfo *mi)
{
  acquire(&kmem.lock);
  struct run *r = kmem.freelist;
  uint free = 0;
  while (r) {
    free++;
    r = r->next;
  }
  release(&kmem.lock);

  acquire(&statlock);
  mi->total_frames = NFRAMES;
  mi->free_frames = free;
  mi->allocated_frames = allocated_frames;
  mi->frame_cap = frame_cap;
  mi->n_zerofaults = stat_zerofaults;
  mi->n_filefaults = stat_filefaults;
  mi->n_cowfaults = stat_cowfaults;
  mi->n_evictions = stat_evictions;
  mi->n_reloads = stat_reloads;
  release(&statlock);
}
