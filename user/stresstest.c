// stresstest.c -- OS mini-project, Modules 1 & 3 demo.
//
// Usage: stresstest <cap-in-frames> <concurrent-children>
//
// IMPORTANT DESIGN NOTE (read this before you run it, and definitely
// before you explain it in the viva): in this project's scoped
// implementation of page replacement, only *clean, file-backed* pages
// -- i.e. a program's read-only .text, loaded lazily by filefault() in
// exec.c -- are ever evicted. Anonymous heap/stack pages are
// deliberately NOT evictable, because reclaiming a page the program has
// written to would require writing it out to a swap area on disk, which
// this project intentionally leaves as future work (see the report).
//
// So: to actually exercise the clock algorithm, this program doesn't
// grow the heap -- it creates *contention for code pages* instead, by
// caping the simulated physical memory very low and then running many
// short-lived child processes concurrently, each independently
// exec()'ing a small program. Every exec() demand-loads a fresh, private
// set of text-page frames (they are not shared across unrelated
// processes), so with enough of them alive at once, the small memcap
// forces the clock algorithm to evict someone's code pages -- which
// then have to be reloaded (filefault() again) if that process is still
// running and touches them again.
//
#include "kernel/types.h"
#include "kernel/meminfo.h"
#include "user/user.h"

#define ROUNDS 15 // how many times each worker re-execs

void
worker(void)
{
  char *args[] = {"echo", "hi", 0};
  for (int i = 0; i < ROUNDS; i++) {
    int pid = fork();
    if (pid < 0)
      exit(1);
    if (pid == 0) {
      close(1); // don't spam the console with ROUNDS*children lines
      exec("echo", args);
      exit(1); // exec only returns on failure
    }
    wait(0);
  }
  exit(0);
}

int
main(int argc, char *argv[])
{
  if (argc != 3) {
    fprintf(2, "usage: stresstest <cap-in-frames> <concurrent-children>\n");
    exit(1);
  }
  int cap = atoi(argv[1]);
  int nchild = atoi(argv[2]);

  struct meminfo before, after;
  meminfo(&before);

  printf("stresstest: capping simulated physical memory to %d frames\n", cap);
  setmemcap(cap);

  printf("stresstest: launching %d concurrent workers, %d exec rounds "
         "each...\n",
         nchild, ROUNDS);

  for (int c = 0; c < nchild; c++) {
    int pid = fork();
    if (pid < 0) {
      fprintf(2, "stresstest: fork failed\n");
      break;
    }
    if (pid == 0)
      worker();
  }
  for (int c = 0; c < nchild; c++)
    wait(0);

  setmemcap(0); // remove the artificial cap before we hand control back
  meminfo(&after);

  printf("stresstest: done. delta since start:\n");
  printf("  file-faults (demand-loaded code pages) = %d\n",
         after.n_filefaults - before.n_filefaults);
  printf("  evictions                              = %d\n",
         after.n_evictions - before.n_evictions);
  printf("  reloads (evicted page touched again)   = %d\n",
         after.n_reloads - before.n_reloads);

  if (after.n_evictions - before.n_evictions > 0)
    printf("stresstest: eviction occurred -- this is Module 3 (page "
           "replacement) triggering under memory pressure.\n");
  else
    printf("stresstest: no evictions yet -- lower the cap or raise "
           "concurrent-children and try again.\n");
  exit(0);
}
