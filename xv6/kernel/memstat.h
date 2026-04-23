// In kernel/memstat.h

#ifndef _MEMSTAT_H_
#define _MEMSTAT_H_

#include "types.h"

#define MAX_PAGES_INFO 128 // Max pages to report per syscall

// Page states
#define UNMAPPED 0 
#define RESIDENT 1 
#define SWAPPED  2

// Structure to report the status of a single page
struct page_stat {
  uint64 va;      // Virtual address of the page
  int state;      // UNMAPPED, RESIDENT, or SWAPPED
  int is_dirty;   // 1 if dirty, 0 otherwise
  int seq;        // FIFO sequence number (if resident)
  int swap_slot;  // Swap slot ID (if swapped)
};

// Structure to report the overall memory status of a process
struct proc_mem_stat {
  int pid;
  int num_pages_total;     // Total virtual pages
  int num_resident_pages;  // Pages in physical memory
  int num_swapped_pages;   // Pages in the swap file
  int next_fifo_seq;       // The next sequence number to be assigned
  struct page_stat pages[MAX_PAGES_INFO]; // Info for each page
};

#endif // _MEMSTAT_H_