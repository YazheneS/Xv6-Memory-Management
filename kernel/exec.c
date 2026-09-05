#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

// --- OS mini-project, Module 1: Demand Paging (lazy loading) ---
//
// Stock xv6 reads every byte of every PT_LOAD segment into physical
// memory at exec() time, whether or not the process ever touches it.
// Here, exec() only *records* where each segment lives on disk (see
// struct segdesc in proc.h) and reserves the corresponding range of
// virtual address space -- no physical pages are allocated and no
// disk I/O happens for the segments at all. The very first time the
// process actually executes an instruction or touches data in a given
// page, that access faults (page not present); vmfault() in vm.c
// recognises the fault as falling inside a demand-paged segment and
// calls filefault() below to load just that one page from disk.
//
// This is also what makes Module 3 (page replacement) possible for
// code/data pages: since we already know how to load any given page of
// a segment on demand, the clock algorithm can evict a clean page
// (uvmevict() just clears PTE_V and sets PTE_PG) knowing a later fault
// will transparently bring it back via this same filefault() path.

// map ELF permissions to PTE permission bits.
int
flags2perm(int flags)
{
  int perm = 0;
  if (flags & 0x1)
    perm = PTE_X;
  if (flags & 0x2)
    perm |= PTE_W;
  return perm;
}

// Release every inode this process is holding onto for demand-paging
// purposes. Called when a process exits, and when exec() commits to a
// brand new image (the old segment table is no longer needed).
// Must be called inside a begin_op()/end_op() transaction, because
// iput() can trigger a disk write if this was the last reference.
void
segs_free(struct proc *p)
{
  for (int i = 0; i < NSEGS; i++) {
    if (p->segs[i].valid && p->segs[i].ip) {
      iput(p->segs[i].ip);
      p->segs[i].ip = 0;
      p->segs[i].valid = 0;
    }
  }
  p->nsegs = 0;
}

// fork() support: the child needs its own duped references to every
// inode the parent is demand-paging from, because although uvmcopy()
// shares *already-resident* pages between parent and child via COW,
// any page neither of them has touched yet is not copied at all -- the
// child must be able to independently fault it in later using its own
// segment table.
int
segs_copy(struct proc *parent, struct proc *child)
{
  for (int i = 0; i < NSEGS; i++) {
    child->segs[i] = parent->segs[i];
    if (parent->segs[i].valid && parent->segs[i].ip) {
      child->segs[i].ip = idup(parent->segs[i].ip);
    }
  }
  child->nsegs = parent->nsegs;
  return 0;
}

// --- Module 1 + Module 3, shared fault handler ---
// Load (or re-load, after an eviction) a single page for process p at
// page-aligned address va, which must fall inside one of p->segs[].
// Handles the "partial last page" case correctly: a segment's memsz
// can exceed its filesz (that's .bss), so bytes beyond filesz within
// the segment are zero-filled instead of read from disk.
// Returns the physical address on success, 0 on failure.
uint64
filefault(struct proc *p, uint64 va)
{
  struct segdesc *seg = 0;

  for (int i = 0; i < p->nsegs; i++) {
    if (p->segs[i].valid && va >= p->segs[i].vabeg && va < p->segs[i].vaend) {
      seg = &p->segs[i];
      break;
    }
  }
  if (seg == 0)
    return 0; // not actually inside any demand-paged segment

  uint64 mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);

  // How much of this page actually has file content behind it?
  uint64 seg_off = va - seg->vabeg; // offset within the segment
  uint n = 0;
  if (seg_off < seg->filesz) {
    uint64 remaining = seg->filesz - seg_off;
    n = (remaining < PGSIZE) ? (uint)remaining : PGSIZE;
  }

  if (n > 0) {
    ilock(seg->ip);
    int ok = readi(seg->ip, 0, mem, seg->fileoff + seg_off, n);
    iunlock(seg->ip);
    if (ok != n) {
      kfree((void *)mem);
      return 0;
    }
  }
  // any remaining bytes in the page (n..PGSIZE) are already zero from
  // the memset above -- this is exactly how .bss is supposed to behave.

  // If a mapping already exists here (PTE_PG from a previous eviction),
  // clear it first so mappages() doesn't panic on "remap".
  pte_t *pte = walk(p->pagetable, va, 0);
  if (pte && (*pte & PTE_PG))
    *pte = 0;

  if (mappages(p->pagetable, va, PGSIZE, mem, seg->perm | PTE_R | PTE_U) != 0) {
    kfree((void *)mem);
    return 0;
  }

  // Only pages that can never be written are safe to reclaim in this
  // scoped implementation of page replacement (see report / README):
  // reclaiming a dirty writable page would require swapping it out to
  // disk, which this project intentionally scopes out. Read-only
  // (PTE_X and/or PTE_R without PTE_W) pages -- i.e. .text -- are
  // always safe, since re-running filefault() reproduces them exactly.
  int evictable = (seg->perm & PTE_W) == 0;
  frame_register(mem, p, va, FRAME_FILE, evictable);
  memstat_filefault();

  return mem;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  struct segdesc newsegs[NSEGS];
  int nnewsegs = 0;

  memset(newsegs, 0, sizeof(newsegs));

  begin_op();

  // Open the executable file.
  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Reserve address space for each PT_LOAD segment, but do NOT read its
  // bytes from disk and do NOT allocate physical pages yet (Module 1).
  // We just grow `sz` and record a segdesc; filefault() does the real
  // work later, one page at a time, on demand.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (nnewsegs >= NSEGS)
      goto bad; // program has more LOAD segments than we support

    uint64 segend = PGROUNDUP(ph.vaddr + ph.memsz);
    if (segend > sz)
      sz = segend;

    newsegs[nnewsegs].valid = 1;
    newsegs[nnewsegs].vabeg = PGROUNDDOWN(ph.vaddr);
    newsegs[nnewsegs].vaend = segend;
    newsegs[nnewsegs].ip = idup(ip); // keep the executable open for the
                                     // lifetime of this process image
    newsegs[nnewsegs].fileoff = ph.off - (ph.vaddr - newsegs[nnewsegs].vabeg);
    newsegs[nnewsegs].filesz = ph.filesz + (ph.vaddr - newsegs[nnewsegs].vabeg);
    newsegs[nnewsegs].perm = flags2perm(ph.flags);
    nnewsegs++;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack. (The stack is small and always
  // touched immediately, so it is allocated eagerly, same as stock
  // xv6 -- only the program image itself is demand-paged.)
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) ==
      0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK * PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for (argc = 0; argv[argc]; argc++) {
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sz, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sz, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) <
      0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image: drop the segment table (and its inode
  // references) belonging to whatever image the process used to be
  // running, and install the new one.
  begin_op();
  segs_free(p);
  end_op();
  memmove(p->segs, newsegs, sizeof(newsegs));
  p->nsegs = nnewsegs;

  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry; // initial program counter = ulib.c:start()
  p->trapframe->sp = sp;         // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (pagetable)
    proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  // release any inodes we duped into newsegs[] before the failure
  begin_op();
  for (i = 0; i < nnewsegs; i++) {
    if (newsegs[i].valid && newsegs[i].ip)
      iput(newsegs[i].ip);
  }
  end_op();
  return -1;
}
