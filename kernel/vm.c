#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy its memory into a child's
// page table for fork().
//
// --- OS mini-project, Module 2: Copy-on-Write ---
// Instead of physically duplicating every page (the original xv6
// behaviour), the child is given a page table entry that points at the
// *same* physical frame as the parent. Both the parent's and the
// child's PTE are marked read-only and tagged PTE_COW, and the frame's
// reference count is bumped. No physical copying happens here at all;
// it happens lazily, one page at a time, the moment either process
// actually tries to write to a shared page (see uvmcowcopy() below,
// invoked from the page-fault path in vmfault()/usertrap()).
//
// A page that was already read-only and not writable in the parent
// (e.g. a demand-paged .text page) is simply shared forever -- there is
// no need to ever copy it, since neither process can write to it.
// returns 0 on success, -1 on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz, struct proc *child)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // page not resident (lazy/unfaulted, or evicted) --
                // nothing to share yet; the child will demand-fault it
                // in independently via its own segs[] table.
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    if (flags & PTE_W) {
      // writable page: share it copy-on-write.
      flags = (flags & ~PTE_W) | PTE_COW;
      *pte = PA2PTE(pa) | flags; // parent's own PTE becomes read-only too
    }
    // (read-only pages, e.g. .text, are shared as-is, no COW tag needed)

    kaddref(pa);
    if (mappages(new, i, PGSIZE, pa, flags) != 0) {
      kfree((void *)pa); // undo the reference we just added
      goto err;
    }
    // Now more than one process may point at this frame -- disable
    // eviction for it (see reclaim_one_frame()'s refcount==1 check);
    // frame_register with evictable=0 keeps the frame table consistent
    // for the child's mapping as well.
    frame_register(pa, child, i, FRAME_ANON, 0);
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// --- OS mini-project, Module 2: resolve a copy-on-write fault ---
// Called from the page-fault handler when a store instruction faults on
// a page marked PTE_COW. If the frame is still shared (refcount > 1),
// allocate a private copy, copy the bytes over, and remap the faulting
// process's PTE as writable and non-COW. If we happen to be the last
// owner already (refcount == 1 -- the other sharer has since exited or
// exec'd), we can just reclaim the existing frame for ourselves instead
// of copying, which is a common and cheap fast path.
// Returns 0 on success, -1 on failure (out of memory).
int
uvmcowcopy(pagetable_t pagetable, uint64 va)
{
  va = PGROUNDDOWN(va);
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_COW) == 0)
    return -1;

  uint64 pa = PTE2PA(*pte);
  uint flags = PTE_FLAGS(*pte);

  if (kgetref(pa) <= 1) {
    // sole owner already -- just reclaim it in place, no copy needed.
    flags = (flags & ~PTE_COW) | PTE_W;
    *pte = PA2PTE(pa) | flags;
    frame_register(pa, myproc(), va, FRAME_ANON, 0);
    memstat_cowfault();
    return 0;
  }

  char *mem = kalloc();
  if (mem == 0)
    return -1;
  memmove(mem, (char *)pa, PGSIZE);

  flags = (flags & ~PTE_COW) | PTE_W;
  *pte = 0; // clear old mapping before remapping (mappages panics on remap)
  if (mappages(pagetable, va, PGSIZE, (uint64)mem, flags) != 0) {
    kfree(mem);
    return -1;
  }
  kfree((void *)pa); // drop our share of the original frame
  frame_register((uint64)mem, myproc(), va, FRAME_ANON, 0);
  memstat_cowfault();
  return 0;
}

// --- OS mini-project, Module 3: reclaim one resident page ---
// Called by the clock algorithm (reclaim_one_frame() in kalloc.c) to
// invalidate a single victim page in the *owning* process's page table.
// The PTE is cleared of PTE_V (so the hardware faults on next access)
// but PTE_PG is set so the fault handler knows "this was evicted, go
// reload it" rather than "this is an illegal access".
void
uvmevict(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0 || (*pte & PTE_V) == 0)
    return;
  *pte = (*pte & ~PTE_V) | PTE_PG;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pte = walk(pagetable, va0, 0);

    // --- OS mini-project, Module 2 (COW) ---
    // walkaddr() below happily returns a physical address for a page
    // that is present-but-read-only, which is exactly what a COW-
    // shared page looks like. The kernel writing straight into that
    // frame via memmove would corrupt memory another process (the
    // parent, or a sibling) is still sharing. So: if this page is not
    // yet mapped OR it is mapped but tagged PTE_COW, route it through
    // vmfault() first -- the same resolver a genuine user-mode store
    // page fault would hit -- to either demand-load it or materialize
    // a private writable copy, before we ever touch memory directly.
    if ((pte == 0) || ((*pte & PTE_V) == 0) || (*pte & PTE_COW)) {
      if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
        return -1;
      }
      pte = walk(pagetable, va0, 0);
    } else {
      pa0 = walkaddr(pagetable, va0);
      if (pa0 == 0)
        return -1;
    }

    // forbid copyout over genuinely read-only user pages (e.g. .text).
    if (pte == 0 || (*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

// Central page-fault resolver. Called from usertrap() (module dispatch
// point for the whole memory-management overhaul -- see the diagram in
// the project report) and from copyin/copyout/copyinstr when the
// kernel itself touches a user page that isn't resident yet.
//
// Decides which of the three modules should handle the fault:
//   1. PTE already exists but is tagged PTE_COW  -> Module 2 (COW)
//   2. PTE already exists but is tagged PTE_PG   -> Module 3 (reload an
//      evicted page)
//   3. va falls inside a demand-paged ELF segment -> Module 1 (load
//      from the executable for the first time)
//   4. otherwise, va is valid heap/stack the process has sbrk'd into
//      but never touched -> classic demand-zero allocation
// returns the physical address on success, 0 on failure (segfault or
// out of memory -- caller kills the process).
uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
  uint64 mem;
  uint64 va0 = PGROUNDDOWN(va);

  if (va >= psz)
    return 0;

  // Module 2: write fault on a copy-on-write shared page.
  pte_t *pte = walk(pagetable, va0, 0);
  if (pte != 0 && (*pte & PTE_V) && (*pte & PTE_COW)) {
    if (uvmcowcopy(pagetable, va0) < 0)
      return 0;
    return PTE2PA(*walk(pagetable, va0, 0));
  }

  // Module 3: this page was evicted by the clock algorithm and needs to
  // be reloaded from wherever it originally came from.
  if (pte != 0 && (*pte & PTE_PG)) {
    uint64 pa = filefault(myproc(), va0);
    if (pa == 0)
      return 0;
    memstat_reload();
    return pa;
  }

  if (ismapped(pagetable, va0)) {
    return 0;
  }

  // Module 1: is this address inside a demand-paged ELF segment
  // recorded by exec()? If so, load the page's content from the
  // executable rather than handing out an anonymous zero page.
  struct proc *p = myproc();
  for (int i = 0; i < p->nsegs; i++) {
    if (p->segs[i].valid && va0 >= p->segs[i].vabeg && va0 < p->segs[i].vaend) {
      return filefault(p, va0);
    }
  }

  // Otherwise: classic demand-zero page (heap grown via sbrk(), or
  // stack) that has never been touched before.
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if (mappages(pagetable, va0, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  frame_register(mem, myproc(), va0, FRAME_ANON, 0); // not evictable in
                                                     // this scoped
                                                     // implementation
                                                     // (see report)
  memstat_zerofault();
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}
