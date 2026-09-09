#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define BSIZE 32768

// Large uninitialized global array.
// This belongs to .bss and should initially contain only zeroes.
char bss_array[BSIZE];

int
main(void)
{
  printf("BSS test starting\n");

  // Verify that the .bss region is zero-filled.
  for (int i = 0; i < BSIZE; i++) {
    if (bss_array[i] != 0) {
      printf("FAIL: bss_array[%d] = %d, expected 0\n",
             i, bss_array[i]);
      exit(1);
    }
  }

  printf("Initial .bss values are zero: PASS\n");

  // Touch the pages so that demand paging is triggered.
  for (int i = 0; i < BSIZE; i += 4096)
    bss_array[i] = 'A';

  printf("Touched .bss pages successfully\n");

 

  // Check that values remain correct.
  for (int i = 0; i < BSIZE; i += 4096) {
    if (bss_array[i] != 'A') {
      printf("FAIL: bss_array[%d] was not written correctly\n", i);
      exit(1);
    }
  }

  printf("BSS test: PASS\n");
  exit(0);
}
