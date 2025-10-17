#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "memstat.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// System call to get memory statistics for the calling process
// Returns 0 on success, -1 on failure
uint64
sys_memstat(void)
{
  uint64 addr;
  struct proc *p = myproc();
  
  // Get user-space pointer from argument
  argaddr(0, &addr);
  
  // Allocate kernel-side structure
  struct proc_mem_stat info;
  
  // Fill basic process information
  info.pid = p->pid;
  info.next_fifo_seq = p->next_seq;
  info.num_resident_pages = p->nresident;
  info.num_swapped_pages = p->nswapped;
  
  // Calculate total pages: from address 0 to process size
  info.num_pages_total = PGROUNDUP(p->sz) / PGSIZE;
  
  // Fill page information (up to MAX_PAGES_INFO pages)
  int page_count = 0;
  uint64 va;
  
  // Iterate through all virtual pages from 0 to sz
  for(va = 0; va < p->sz && page_count < MAX_PAGES_INFO; va += PGSIZE) {
    uint64 va_aligned = PGROUNDDOWN(va);
    struct page_stat *ps = &info.pages[page_count];
    
    ps->va = va_aligned;
    ps->state = UNMAPPED;  // Default: not mapped
    ps->is_dirty = 0;
    ps->seq = -1;
    ps->swap_slot = -1;
    
    // Check if page is in resident set (in memory)
    for(int i = 0; i < p->nresident; i++) {
      if(p->resident[i].va == va_aligned && p->resident[i].in_memory) {
        ps->state = RESIDENT;
        ps->is_dirty = p->resident[i].dirty;
        ps->seq = p->resident[i].seq;
        ps->swap_slot = -1;
        break;
      }
    }
    
    // If not resident, check if it's swapped
    if(ps->state == UNMAPPED) {
      for(int i = 0; i < MAX_SWAP_PAGES; i++) {
        if(p->swap_map[i].va == va_aligned) {
          ps->state = SWAPPED;
          ps->is_dirty = 1;  // Swapped pages are dirty
          ps->seq = -1;      // Not in memory, no sequence
          ps->swap_slot = p->swap_map[i].swap_slot;
          break;
        }
      }
    }
    
    // If still unmapped, check if page table entry exists
    if(ps->state == UNMAPPED) {
      pte_t *pte = walk(p->pagetable, va_aligned, 0);
      if(pte != 0 && (*pte & PTE_V)) {
        // Page is mapped but not tracked in resident (shouldn't happen in our design)
        ps->state = RESIDENT;
      }
    }
    
    page_count++;
  }
  
  // Copy the structure to user space
  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0) {
    return -1;
  }
  
  return 0;
}
