// cowtest.c -- OS mini-project, Module 2 demo.
//
// Allocates a large array, touches all of it (so it's actually
// resident), then forks several children. With COW fork, this should
// complete almost instantly and the meminfo counters should show far
// fewer zero-faults than a naive implementation that duplicated every
// page eagerly at fork time -- and each child then privately modifies
// its own copy of the array to trigger the actual copy-on-write faults,
// which memtop's "cow=" counter will reflect.
//
#include "kernel/types.h"
#include "kernel/meminfo.h"
#include "user/user.h"

#define NPAGES 200 // ~800KB shared region
#define PGSIZE 4096
#define NCHILD 4

char *big;

int
main(void)
{
  struct meminfo before, after;

  big = sbrk(NPAGES * PGSIZE);
  if (big == SBRK_ERROR) {
    fprintf(2, "cowtest: sbrk failed\n");
    exit(1);
  }

  // Touch every page once so it's actually resident before we fork --
  // this is exactly the case where naive fork() would eagerly
  // duplicate everything.
  for (int i = 0; i < NPAGES; i++)
    big[i * PGSIZE] = 'A';

  meminfo(&before);
  printf("cowtest: %d pages resident, forking %d children...\n", NPAGES,
         NCHILD);

  for (int c = 0; c < NCHILD; c++) {
    int pid = fork();
    if (pid < 0) {
      fprintf(2, "cowtest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      // Child: modify only a handful of pages -- this should trigger
      // exactly that many COW copies, not NPAGES of them.
      for (int i = 0; i < 5; i++)
        big[i * PGSIZE] = 'B' + c;
      exit(0);
    }
  }
  for (int c = 0; c < NCHILD; c++)
    wait(0);

  meminfo(&after);

  printf("cowtest: done.\n");
  printf("cowtest: cow faults triggered = %d (expected around %d, i.e. "
         "NCHILD*5, not NCHILD*%d)\n",
         after.n_cowfaults - before.n_cowfaults, NCHILD * 5, NPAGES);
  printf("cowtest: run 'memtop' before/after this program to see the "
         "full picture.\n");
  exit(0);
}
