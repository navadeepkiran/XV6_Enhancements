// Memory test program to trigger page replacement
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

int
main(int argc, char *argv[])
{
  int i;
  char *pages[50];  // Try to allocate 50 pages (200KB)
  
  printf("memtest: starting memory allocation test\n");
  
  // Allocate and touch many pages
  for(i = 0; i < 50; i++) {
    // Use sbrklazy() to trigger demand paging and page faults
    pages[i] = sbrklazy(PGSIZE);
    if(pages[i] == (char*)-1) {
      printf("memtest: sbrk failed at page %d\n", i);
      break;
    }
    
    // Touch the page by writing to it (this will trigger page fault)
    pages[i][0] = 'A' + (i % 26);
    pages[i][PGSIZE-1] = 'Z' - (i % 26);
    
    if(i % 10 == 0) {
      printf("memtest: allocated page %d at %p\n", i, pages[i]);
    }
  }
  
  printf("memtest: allocated %d pages\n", i);
  
  // Now access all pages again to verify they're still there
  printf("memtest: verifying pages...\n");
  for(int j = 0; j < i; j++) {
    char first = pages[j][0];
    char last = pages[j][PGSIZE-1];
    
    if(first != 'A' + (j % 26) || last != 'Z' - (j % 26)) {
      printf("memtest: ERROR: page %d corrupted! (expected %c/%c, got %c/%c)\n", 
             j, 'A' + (j % 26), 'Z' - (j % 26), first, last);
    } else if(j % 10 == 0) {
      printf("memtest: page %d OK (%c/%c)\n", j, first, last);
    }
  }
  
  printf("memtest: test complete\n");
  exit(0);
}
