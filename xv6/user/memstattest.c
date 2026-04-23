// Test program for memstat system call
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/memstat.h"

int
main(int argc, char *argv[])
{
  struct proc_mem_stat info;
  char *pages[20];
  int i;
  
  printf("memstattest: starting\n");
  
  // Allocate some pages
  printf("\n=== Allocating 12 pages ===\n");
  for(i = 0; i < 12; i++) {
    pages[i] = sbrk(4096);
    if(pages[i] == (char*)-1) {
      printf("memstattest: sbrk failed\n");
      exit(1);
    }
    // Write to the page to ensure it's allocated
    pages[i][0] = 'A' + i;
    pages[i][4095] = 'Z' - i;
  }
  printf("memstattest: allocated 12 pages\n");
  
  // Call memstat to inspect memory state
  printf("\n=== Calling memstat ===\n");
  if(memstat(&info) < 0) {
    printf("memstattest: memstat failed\n");
    exit(1);
  }
  
  // Print the results
  printf("\nProcess Memory Statistics:\n");
  printf("  PID: %d\n", info.pid);
  printf("  Total pages: %d\n", info.num_pages_total);
  printf("  Resident pages: %d\n", info.num_resident_pages);
  printf("  Swapped pages: %d\n", info.num_swapped_pages);
  printf("  Next FIFO seq: %d\n", info.next_fifo_seq);
  
  printf("\nPer-Page Information (showing first 20 pages):\n");
  printf("  VA        State      Dirty  Seq   SwapSlot\n");
  printf("  --------  ---------  -----  ----  --------\n");
  
  for(i = 0; i < 20 && i < info.num_pages_total; i++) {
    char *state_str;
    if(info.pages[i].state == UNMAPPED)
      state_str = "UNMAPPED ";
    else if(info.pages[i].state == RESIDENT)
      state_str = "RESIDENT ";
    else if(info.pages[i].state == SWAPPED)
      state_str = "SWAPPED  ";
    else
      state_str = "UNKNOWN  ";
    
    printf("  0x%lx  %s  %d      ", 
           info.pages[i].va, state_str, info.pages[i].is_dirty);
    
    if(info.pages[i].seq >= 0)
      printf("%d  ", info.pages[i].seq);
    else
      printf("N/A   ");
    
    if(info.pages[i].swap_slot >= 0)
      printf("%d\n", info.pages[i].swap_slot);
    else
      printf("N/A\n");
  }
  
  // Verify data integrity
  printf("\n=== Verifying data integrity ===\n");
  int errors = 0;
  for(i = 0; i < 12; i++) {
    if(pages[i][0] != 'A' + i || pages[i][4095] != 'Z' - i) {
      printf("memstattest: ERROR: page %d corrupted!\n", i);
      errors++;
    }
  }
  
  if(errors == 0) {
    printf("memstattest: All data verified successfully!\n");
  } else {
    printf("memstattest: %d pages corrupted!\n", errors);
  }
  
  // Allocate more pages to force more swapping
  printf("\n=== Allocating 8 more pages ===\n");
  for(i = 12; i < 20; i++) {
    pages[i] = sbrk(4096);
    if(pages[i] == (char*)-1) {
      printf("memstattest: sbrk failed\n");
      exit(1);
    }
    pages[i][0] = 'A' + i;
    pages[i][4095] = 'Z' - i;
  }
  printf("memstattest: allocated 8 more pages (total 20)\n");
  
  // Call memstat again
  printf("\n=== Calling memstat again ===\n");
  if(memstat(&info) < 0) {
    printf("memstattest: memstat failed\n");
    exit(1);
  }
  
  printf("\nUpdated Statistics:\n");
  printf("  Total pages: %d\n", info.num_pages_total);
  printf("  Resident pages: %d\n", info.num_resident_pages);
  printf("  Swapped pages: %d\n", info.num_swapped_pages);
  printf("  Next FIFO seq: %d\n", info.next_fifo_seq);
  
  // Count pages in each state
  int unmapped = 0, resident = 0, swapped = 0;
  for(i = 0; i < info.num_pages_total && i < MAX_PAGES_INFO; i++) {
    if(info.pages[i].state == UNMAPPED)
      unmapped++;
    else if(info.pages[i].state == RESIDENT)
      resident++;
    else if(info.pages[i].state == SWAPPED)
      swapped++;
  }
  
  printf("\nPage state summary (from pages array):\n");
  printf("  UNMAPPED: %d\n", unmapped);
  printf("  RESIDENT: %d\n", resident);
  printf("  SWAPPED: %d\n", swapped);
  
  // Final verification
  printf("\n=== Final data verification ===\n");
  errors = 0;
  for(i = 0; i < 20; i++) {
    if(pages[i][0] != 'A' + i || pages[i][4095] != 'Z' - i) {
      printf("memstattest: ERROR: page %d corrupted!\n", i);
      errors++;
    }
  }
  
  if(errors == 0) {
    printf("memstattest: All 20 pages verified successfully!\n");
    printf("\nmemstattest: TEST PASSED!\n");
  } else {
    printf("memstattest: %d pages corrupted!\n", errors);
    printf("\nmemstattest: TEST FAILED!\n");
  }
  
  exit(0);
}
