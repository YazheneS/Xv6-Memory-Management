// memtop.c -- OS mini-project, Module 4 demo.
//
// Prints a live-updating snapshot of the kernel's memory-management
// counters (from kalloc.c) so you can literally watch demand paging,
// copy-on-write, and page replacement happen while another program
// runs. Usage:
//   memtop            -- print one snapshot and exit
//   memtop N          -- print a snapshot every N ticks, forever
//
#include "kernel/types.h"
#include "kernel/meminfo.h"
#include "user/user.h"

void
printsnapshot(struct meminfo *mi)
{
  printf("== memtop ==\n");
  printf("frames   : total=%d free=%d allocated=%d cap=%d\n", mi->total_frames,
         mi->free_frames, mi->allocated_frames, mi->frame_cap);
  printf("faults   : zero=%d file=%d cow=%d\n", mi->n_zerofaults,
         mi->n_filefaults, mi->n_cowfaults);
  printf("replace  : evictions=%d reloads=%d\n", mi->n_evictions,
         mi->n_reloads);
  printf("\n");
}

int
main(int argc, char *argv[])
{
  struct meminfo mi;

  if (argc <= 1) {
    if (meminfo(&mi) < 0) {
      fprintf(2, "memtop: meminfo syscall failed\n");
      exit(1);
    }
    printsnapshot(&mi);
    exit(0);
  }

  int interval = atoi(argv[1]);
  if (interval <= 0)
    interval = 10;

  for (;;) {
    if (meminfo(&mi) < 0) {
      fprintf(2, "memtop: meminfo syscall failed\n");
      exit(1);
    }
    printsnapshot(&mi);
    pause(interval);
  }
}
