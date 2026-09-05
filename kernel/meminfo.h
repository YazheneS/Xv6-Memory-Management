// --- OS mini-project, Module 4: instrumentation ---
// Shared between the kernel (kalloc.c/sysproc.c) and userspace demo
// programs (memtop.c, stresstest.c), the same way kernel/stat.h is.
struct meminfo {
  uint total_frames;     // total physical frames managed by kalloc
  uint free_frames;      // frames currently on the free list
  uint allocated_frames; // frames currently handed out
  uint frame_cap;        // artificial cap used to simulate memory pressure
  uint n_zerofaults;     // demand-zero (heap/stack) faults
  uint n_filefaults;     // demand-load-from-executable faults
  uint n_cowfaults;      // copy-on-write faults
  uint n_evictions;      // pages reclaimed by the clock algorithm
  uint n_reloads;        // evicted pages that were subsequently reloaded
};
