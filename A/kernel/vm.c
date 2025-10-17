#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "stat.h"
#include "fcntl.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Debug root pagetable used to map child page-table pages back to virtual ranges
// for diagnostic printing when freewalk finds an unexpected leaf.
static pagetable_t debug_root_pagetable = 0;

// Set the debug root pagetable (called from proc_freepagetable)
void
vm_set_debug_root(pagetable_t root)
{
  debug_root_pagetable = root;
}

// Recursive search helper: walk a pagetable and try to find a PTE that
// points to child_pa. When found, print the VA range covered by that entry.
// level = 2..0 (Sv39), va_base = base VA of this pagetable at this level.
// static int
// find_child_in_root(pagetable_t pt, uint64 child_pa, int level, uint64 va_base)
// {
//   for(int i = 0; i < 512; i++){
//     pte_t pte = pt[i];
//     if((pte & PTE_V) == 0)
//       continue;
//     if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
//       // points to next-level page table
//       uint64 pa = PTE2PA(pte);
//       if(pa == child_pa){
//         // compute VA range depending on level
//         int shift = 12 + 9 * level; // level 2 -> 30, level1 ->21, level0->12
//         uint64 range = (1ULL << shift);
//         uint64 va_start = va_base + ((uint64)i << shift);
//         uint64 va_end = va_start + range - 1;
//         printf("freewalk: child page-table found at level=%d index=%d va-range=0x%lx-0x%lx\n",
//                level, i, va_start, va_end);
//         return 1;
//       }
//       // recurse into child page table
//       pagetable_t child = (pagetable_t)PTE2PA(pte);
//       uint64 next_va_base = va_base + ((uint64)i << (12 + 9 * level));
//       if(find_child_in_root(child, child_pa, level-1, next_va_base))
//         return 1;
//     }
//   }
//   return 0;
// }

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// Recursively free page-table pages.

void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
    } else if(pte & PTE_V){
      // this is a leaf page mapping.
      // free the physical page.
      uint64 pa = PTE2PA(pte);
      kfree((void*)pa);
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  //   if(sz > 0)
  //   uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  // freewalk(pagetable, 2);
  if (pagetable) {
    freewalk(pagetable);
  }
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// Handle page faults for demand paging.
// Returns physical address if successful, 0 on failure.
// This function handles:
// 1. Text/Data segments - load from executable file
// 2. Heap (sbrk) - allocate zero-filled page
// 3. Stack - allocate zero-filled page if within stack bounds
uint64
vmfault(pagetable_t pagetable, uint64 va, int write)
{
  uint64 mem;
  struct proc *p = myproc();
  uint64 va_aligned = PGROUNDDOWN(va);
  int i, n;
  uint64 offset;
  int perm;
  
  // Check for invalid addresses (e.g., kernel memory, beyond MAXVA)
  if(va_aligned >= MAXVA) {
    // printf("PAGEFAULT pid=%d va=0x%lx %s INVALID: addr out of range\n", 
    //         p->pid, va_aligned, write ? "WRITE" : "READ");
    return 0;
  }
  
  // Check if page is already mapped
  if(ismapped(pagetable, va_aligned)) {
    // Page is already mapped but we got a page fault - this is a protection violation
    // This is an invalid access that should terminate the process
    // printf("PAGEFAULT pid=%d va=0x%lx %s INVALID: protection violation\n",
    //         p->pid, va_aligned, write ? "WRITE" : "READ");
    return 0;  // Return failure to kill the process
  }
  
  // SWAP CHECK: Before allocating a new page, check if this page is in swap
  // Search the swap_map table for a matching VA
  int swap_slot = -1;
  for(i = 0; i < MAX_SWAP_PAGES; i++) {
    if(p->swap_map[i].va == va_aligned) {
      // Found it! This page is in swap
      swap_slot = p->swap_map[i].swap_slot;
      break;
    }
  }
  
  if(swap_slot >= 0) {
    // This page is in swap! Load it back
    printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, va_aligned, swap_slot);
    
    // Swap in the page
    if(swap_in(va_aligned, swap_slot) < 0) {
      return 0;
    }
    
    // Add back to resident set as a newly accessed page
    add_resident_page(va_aligned, 1);  // Writable since it was swapped out
    
    return va_aligned;  // Success
  }
  
  // Determine the type of page fault and handle accordingly
  
  // Case 1: Check if va is in text/data segments (executable)
  for(i = 0; i < p->nsegments; i++) {
    struct seginfo *seg = &p->segments[i];
    uint64 seg_start = seg->vaddr;
    uint64 seg_end = seg->vaddr + seg->memsz;
    
    if(va_aligned >= seg_start && va_aligned < seg_end) {
      // This is a fault in a text/data segment - load from executable
      
      // Check if we have the executable file
      if(p->execfile == 0) {
        // printf("PAGEFAULT pid=%d va=0x%lx FATAL: no executable file\n", p->pid, va_aligned);
        // return 0;
      }
      
      // Allocate physical page
      mem = (uint64)kalloc();
      if(mem == 0) {
        // Out of physical memory - need to evict a page using FIFO
        printf("MEMFULL pid=%d\n", p->pid);
        
        int victim_idx = find_victim_fifo();
        if(victim_idx < 0) {
          // No pages to evict - process must terminate
         // printf("PAGEFAULT pid=%d va=0x%lx FATAL: out of memory, no pages to evict\n", p->pid, va_aligned);
          return 0;
        }
        
        // Evict the victim page
        if(evict_page(victim_idx) < 0) {
         // printf("PAGEFAULT pid=%d va=0x%lx FATAL: failed to evict page\n", p->pid, va_aligned);
          return 0;
        }
        
        // Try to allocate again after eviction
        mem = (uint64)kalloc();
        if(mem == 0) {
        //  printf("PAGEFAULT pid=%d va=0x%lx FATAL: still out of memory after eviction\n", p->pid, va_aligned);
          return 0;
        }
      }
      memset((void*)mem, 0, PGSIZE);
      
      // Determine how much to load from file
      uint64 page_offset_in_seg = va_aligned - seg->vaddr;
      offset = seg->offset + page_offset_in_seg;
      
      if(page_offset_in_seg < seg->filesz) {
        // Load from file
        n = PGSIZE;
        if(seg->filesz - page_offset_in_seg < PGSIZE)
          n = seg->filesz - page_offset_in_seg;
        
        // Read from executable file
        ilock(p->execfile);
        if(readi(p->execfile, 0, mem, offset, n) != n) {
          iunlock(p->execfile);
          kfree((void*)mem);
        //  printf("PAGEFAULT pid=%d va=0x%lx FATAL: cannot read executable\n", p->pid, va_aligned);
          return 0;
        }
        iunlock(p->execfile);
        
        // Log the page load from executable
        printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n", 
                p->pid, va_aligned, write ? "write" : "read");
        printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va_aligned);
      } else {
        // BSS section (zero-filled part of segment beyond file size)
        printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n", 
                p->pid, va_aligned, write ? "write" : "read");
        printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va_aligned);
      }
      
      // Map the page with appropriate permissions
      perm = PTE_U | PTE_R;
      if(seg->flags & 0x2)  // Writable
        perm |= PTE_W;
      if(seg->flags & 0x1)  // Executable
        perm |= PTE_X;
      
      if(mappages(pagetable, va_aligned, PGSIZE, mem, perm) != 0) {
        kfree((void*)mem);
       // printf("PAGEFAULT pid=%d va=0x%lx FATAL: cannot map page\n", p->pid, va_aligned);
        return 0;
      }
      
      // Add page to resident set for FIFO tracking
      add_resident_page(va_aligned, (perm & PTE_W) != 0);
      
      // If page is writable, mark as dirty (we can't track subsequent writes without hardware support)
      if(perm & PTE_W) {
        mark_page_dirty(va_aligned);
      }
      
      return mem;
    }
  }
  
  // Case: Stack region handling - check stack guard and allow stack growth
  // before deciding this might be a heap access. This ensures accesses
  // below the current stack pointer are treated as invalid (trap) and
  // pages near the stack are handled by stack growth logic.
  uint64 sp = p->trapframe->sp;
  uint64 sp_aligned = PGROUNDDOWN(sp);

  // If the faulting address is below the current stack pointer, it's a
  // stack guard violation and should kill the process.
  if(va < sp) {
  //  printf("PAGEFAULT pid=%d va=0x%lx %s INVALID: stack guard violation (sp=0x%lx)\n",
    //        p->pid, va_aligned, write ? "WRITE" : "READ", sp);
    return 0;
  }

  // If the faulting page is at or above the current stack pointer and within
  // the reserved process size, treat it as stack growth and allocate a page.
  if(va_aligned >= sp_aligned && va_aligned < p->sz) {
    // Stack page - allocate zero-filled
    mem = (uint64)kalloc();
    if(mem == 0) {
      // Out of physical memory - need to evict a page using FIFO
      printf("MEMFULL pid=%d\n", p->pid);
      
      int victim_idx = find_victim_fifo();
      if(victim_idx < 0) {
       // printf("PAGEFAULT pid=%d va=0x%lx FATAL: out of memory, no pages to evict\n", p->pid, va_aligned);
        return 0;
      }
      
      if(evict_page(victim_idx) < 0) {
       // printf("PAGEFAULT pid=%d va=0x%lx FATAL: failed to evict page\n", p->pid, va_aligned);
        return 0;
      }
      
      mem = (uint64)kalloc();
      if(mem == 0) {
       // printf("PAGEFAULT pid=%d va=0x%lx FATAL: still out of memory after eviction\n", p->pid, va_aligned);
        return 0;
      }
    }
    memset((void*)mem, 0, PGSIZE);
    
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=stack\n", 
            p->pid, va_aligned, write ? "write" : "read");
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va_aligned);
    
    if(mappages(pagetable, va_aligned, PGSIZE, mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree((void*)mem);
     // printf("PAGEFAULT pid=%d va=0x%lx FATAL: cannot map stack page\n", p->pid, va_aligned);
      return 0;
    }
    
    // Add page to resident set for FIFO tracking (stack is writable, so mark as dirty)
    add_resident_page(va_aligned, 1);
    // Stack pages are writable, so mark as dirty immediately since we can't track writes
    mark_page_dirty(va_aligned);
    
    return mem;
  }

  // Compute stack base: the lowest virtual address of the reserved stack region.
  // p->sz includes the reserved stack region (set in exec), so stackbase
  // is p->sz - USERSTACK*PGSIZE.
  uint64 stackbase = 0;
  if(p->sz >= (uint64)USERSTACK * PGSIZE)
    stackbase = p->sz - (uint64)USERSTACK * PGSIZE;

  // Case 2: Check if va is in heap (between data_end and stackbase)
  if(stackbase != 0 && va_aligned >= p->data_end && va_aligned < stackbase) {
    // First check if this page is in swap
    int swap_slot = -1;
    int resident_idx = -1;
    for(int i = 0; i < MAX_RESIDENT_PAGES; i++) {
      if(p->resident[i].va == va_aligned) {
        // Found the page in resident set
        if(p->resident[i].swap_offset >= 0 && !p->resident[i].in_memory) {
          // Page is swapped out
          swap_slot = p->resident[i].swap_offset;
          resident_idx = i;
          break;
        } else if(p->resident[i].in_memory) {
          // Page is already in memory (shouldn't fault on it)
          printf("WARNING: Page fault on in-memory page va=0x%lx\n", va_aligned);
          pte_t *pte = walk(pagetable, va_aligned, 0);
          if(pte && (*pte & PTE_V)) {
            return PTE2PA(*pte);
          }
        }
        break;
      }
    }
    
    if(swap_slot >= 0) {
      // Page is in swap - load it
      printf("SWAPIN pid=%d va=0x%lx slot=%d\n", p->pid, va_aligned, swap_slot);
      
      if(swap_in(va_aligned, swap_slot) == 0) {
        // Swap-in successful, update resident entry
        p->resident[resident_idx].in_memory = 1;
        p->resident[resident_idx].seq = p->next_seq++;
        p->resident[resident_idx].dirty = write ? 1 : 0;
        p->resident[resident_idx].swap_offset = -1;  // Clear swap offset since it's back in memory
        
        // Return the physical address
        pte_t *pte = walk(pagetable, va_aligned, 0);
        if(pte && (*pte & PTE_V)) {
          return PTE2PA(*pte);
        }
      }
    //  printf("PAGEFAULT pid=%d va=0x%lx FATAL: swap-in failed\n", p->pid, va_aligned);
      return 0;
    }
    
  // Heap page not in swap - allocate zero-filled
    mem = (uint64)kalloc();
    if(mem == 0) {
      // Out of physical memory - need to evict a page using FIFO
      printf("MEMFULL pid=%d\n", p->pid);
      
      int victim_idx = find_victim_fifo();
      if(victim_idx < 0) {
        //printf("PAGEFAULT pid=%d va=0x%lx FATAL: out of memory, no pages to evict\n", p->pid, va_aligned);
        return 0;
      }
      
      if(evict_page(victim_idx) < 0) {
      //  printf("PAGEFAULT pid=%d va=0x%lx FATAL: failed to evict page\n", p->pid, va_aligned);
        return 0;
      }
      
      mem = (uint64)kalloc();
      if(mem == 0) {
      //  printf("PAGEFAULT pid=%d va=0x%lx FATAL: still out of memory after eviction\n", p->pid, va_aligned);
        return 0;
      }
    }
    memset((void*)mem, 0, PGSIZE);
    
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=heap\n", 
            p->pid, va_aligned, write ? "write" : "read");
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va_aligned);
    
    if(mappages(pagetable, va_aligned, PGSIZE, mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree((void*)mem);
      //printf("PAGEFAULT pid=%d va=0x%lx FATAL: cannot map heap page\n", p->pid, va_aligned);
      return 0;
    }
    
    // Add page to resident set for FIFO tracking (heap is writable, so mark as dirty)
    add_resident_page(va_aligned, 1);
    // Heap pages are writable, so mark as dirty immediately since we can't track writes
    mark_page_dirty(va_aligned);
    
    return mem;
  }
  
  
  
  // Case 4: Invalid access - outside all valid regions
 // printf("PAGEFAULT pid=%d va=0x%lx %s INVALID: addr out of range\n", 
     //     p->pid, va_aligned, write ? "WRITE" : "READ");
  return 0;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// Add a page to the resident set with FIFO tracking
// va: virtual address (page-aligned)
// writable: 1 if page is writable (for dirty tracking), 0 otherwise
void
add_resident_page(uint64 va, int writable)
{
  struct proc *p = myproc();
  
  // Check if page is already in resident set
  for(int i = 0; i < p->nresident; i++) {
    if(p->resident[i].va == va) {
      // Already tracked, nothing to do
      return;
    }
  }
  
  // Add new page to resident set
  if(p->nresident < MAX_RESIDENT_PAGES) {
    // Room available, just add it
    p->resident[p->nresident].va = va;
    p->resident[p->nresident].seq = p->next_seq;  // Assign FIFO sequence
    p->resident[p->nresident].dirty = 0;  // Initially clean
    p->resident[p->nresident].swap_offset = -1;  // Not in swap
    p->resident[p->nresident].in_memory = 1;  // In physical memory
    p->nresident++;
    
    // Log page becoming resident
    printf("[pid %d] RESIDENT va=0x%lx seq=%ld\n", p->pid, va, p->next_seq);
    p->next_seq++;
  } else {
    // Resident set is full - need to evict a page before adding new one
    printf("[pid %d] MEMFULL\n", p->pid);
    
    int victim_idx = find_victim_fifo();
    if(victim_idx < 0) {
      // No victim available - can't make room. Kill the process instead of panicking.
      printf("[pid %d] add_resident_page: no victim found, killing process\n", p->pid);
      kkill(p->pid);
      return;
    }

    if(evict_page(victim_idx) < 0) {
      // Eviction or swap-out failed (e.g., swap full). Kill the process instead of panicking.
      printf("[pid %d] add_resident_page: failed to evict page, killing process\n", p->pid);
      kkill(p->pid);
      return;
    }
    
    // After eviction, there's always room (evict_page removes entry from resident array)
    p->resident[p->nresident].va = va;
    p->resident[p->nresident].seq = p->next_seq;
    p->resident[p->nresident].dirty = 0;
    p->resident[p->nresident].swap_offset = -1;
    p->resident[p->nresident].in_memory = 1;
    p->nresident++;
    
    // Log page becoming resident
    printf("[pid %d] RESIDENT va=0x%lx seq=%ld\n", p->pid, va, p->next_seq);
    p->next_seq++;
  }
}

// Mark a page as dirty when written to
// Called on write page faults
void
mark_page_dirty(uint64 va)
{
  struct proc *p = myproc();
  uint64 va_aligned = PGROUNDDOWN(va);
  
  for(int i = 0; i < p->nresident; i++) {
    if(p->resident[i].va == va_aligned) {
      p->resident[i].dirty = 1;
      return;
    }
  }
}

// Find victim page using FIFO policy
// Returns index in resident[] array, or -1 if no victim found
int
find_victim_fifo(void)
{
  struct proc *p = myproc();
  
  if(p->nresident == 0) {
    return -1;  // No resident pages to evict
  }
  
  // Find page with lowest sequence number (oldest) that's in memory
  int victim_idx = -1;
  uint64 min_seq = 0xFFFFFFFFFFFFFFFF;  // Max uint64
  
  for(int i = 0; i < p->nresident; i++) {
    // Only consider pages that are currently in physical memory
    if(p->resident[i].in_memory && p->resident[i].seq < min_seq) {
      min_seq = p->resident[i].seq;
      victim_idx = i;
    }
  }
  
  if(victim_idx < 0) {
    return -1;  // No in-memory pages to evict
  }
  
  // Log the victim selection (Part 4 format)
  printf("[pid %d] VICTIM va=0x%lx seq=%ld algo=FIFO\n", 
          p->pid, p->resident[victim_idx].va, p->resident[victim_idx].seq);
  
  return victim_idx;
}

// Evict a page at the given resident set index
// Returns 0 on success, -1 on failure
int
evict_page(int idx)
{
  struct proc *p = myproc();
  
  if(idx < 0 || idx >= p->nresident) {
    return -1;
  }
  
  uint64 va = p->resident[idx].va;
  int dirty = p->resident[idx].dirty;
  
  // Get the PTE for this page
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0) {
    // Page not mapped, just remove from resident set
    goto remove_from_resident;
  }
  
  uint64 pa = PTE2PA(*pte);
  
  // Handle dirty vs clean pages
  if(dirty) {
    // Dirty page must be written to swap
    printf("[pid %d] EVICT  va=0x%lx state=dirty\n", p->pid, va);
    
    // Write page to swap file
    if(swap_out(va, idx) < 0) {
      // Swap failed - either no swap space or I/O error
      printf("[pid %d] SWAPFULL\n", p->pid);
      printf("[pid %d] KILL swap-exhausted\n", p->pid);
      return -1;
    }
  } else {
    // Clean page can be discarded (it has a valid copy elsewhere)
    printf("[pid %d] EVICT  va=0x%lx state=clean\n", p->pid, va);
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, va);
  }
  
  // Free the physical page
  kfree((void*)pa);
  
  // Unmap the page (clear PTE)
  *pte = 0;
  
  // Remove from resident set completely (swap info is tracked in swap_map now)
  // Shift remaining entries down to fill the gap
remove_from_resident:
  for(int i = idx; i < p->nresident - 1; i++) {
    p->resident[i] = p->resident[i + 1];
  }
  p->nresident--;
  
  return 0;
}

// ============================================================================
// SWAP FILE FUNCTIONS (Part 3)
// ============================================================================

// Create per-process swap file
// Returns 0 on success, -1 on failure
int
create_swapfile(void)
{
  struct proc *p = myproc();
  
  // Generate unique swap file name using PID
  // Format: "pgswpXXXXX" where XXXXX is the 5-digit PID
  // Manually construct the filename since xv6 doesn't have snprintf
  p->swapname[0] = 'p';
  p->swapname[1] = 'g';
  p->swapname[2] = 's';
  p->swapname[3] = 'w';
  p->swapname[4] = 'p';
  // Convert PID to 5-digit string
  int pid = p->pid;
  for(int i = 9; i >= 5; i--) {
    p->swapname[i] = '0' + (pid % 10);
    pid /= 10;
  }
  p->swapname[10] = '\0';
  
  // Create the swap file in the root directory
  begin_op();
  struct inode *ip = create(p->swapname, T_FILE, 0, 0);
  if(ip == 0) {
    end_op();
    return -1;
  }
  
  // Keep a persistent reference to this inode
  // idup() before unlocking to ensure it stays in cache
  struct inode *swap_ip = idup(ip);  // Increment ref to 2
  iunlock(ip);  // Unlock the inode
  end_op();     // End the transaction
  
  // Store the inode pointer (ref=2, won't be evicted)
  p->swapfile = swap_ip;
  
  // Initialize swap tracking
  p->nswapped = 0;
  for(int i = 0; i < 32; i++) {
    p->swap_bitmap[i] = 0;  // All slots free initially
  }
  
  // Initialize all resident pages' swap_offset to -1 (not in swap)
  for(int i = 0; i < MAX_RESIDENT_PAGES; i++) {
    p->resident[i].swap_offset = -1;
  }
  
  return 0;
}

// Close and delete the swap file
void
close_swapfile(void)
{
  struct proc *p = myproc();
  
  if(p->swapfile == 0)
    return;
  
  // Log cleanup
  printf("SWAPCLEANUP pid=%d slots=%d\n", p->pid, p->nswapped);
  
  // Delete the swap file and release the inode
  begin_op();
  ilock(p->swapfile);
  p->swapfile->nlink = 0;  // Mark for deletion
  iupdate(p->swapfile);
  iunlockput(p->swapfile);
  end_op();
  
  p->swapfile = 0;
  p->nswapped = 0;
}

// Allocate a free swap slot
// Returns slot number (0-1023) on success, -1 if no free slots
int
alloc_swap_slot(void)
{
  struct proc *p = myproc();
  
  // Search for a free slot in the bitmap
  for(int i = 0; i < MAX_SWAP_PAGES; i++) {
    int word_idx = i / 32;        // Which uint32 in bitmap
    int bit_idx = i % 32;         // Which bit in that uint32
    uint32 mask = 1 << bit_idx;
    
    if((p->swap_bitmap[word_idx] & mask) == 0) {
      // Found a free slot, mark it as used
      p->swap_bitmap[word_idx] |= mask;
      return i;
    }
  }
  
  // No free slots available
  return -1;
}

// Free a swap slot
void
free_swap_slot(int slot)
{
  struct proc *p = myproc();
  
  if(slot < 0 || slot >= MAX_SWAP_PAGES)
    return;
  
  int word_idx = slot / 32;
  int bit_idx = slot % 32;
  uint32 mask = 1 << bit_idx;
  
  // Clear the bit to mark slot as free
  p->swap_bitmap[word_idx] &= ~mask;
}

// Write a page to swap file (called during eviction)
// va: virtual address of the page to swap out
// resident_idx: index in the resident set
// Returns 0 on success, -1 on failure
int
swap_out(uint64 va, int resident_idx)
{
  struct proc *p = myproc();
  
  // Check if swap file exists
  if(p->swapfile == 0) {
    // Create swap file on first use
    if(create_swapfile() < 0) {
      printf("swap_out: failed to create swap file\n");
      return -1;
    }
  }
  
  // Allocate a swap slot
  int slot = alloc_swap_slot();
  if(slot < 0) {
    // No free slots, terminate process
    printf("SWAPFULL pid=%d\n", p->pid);
    return -1;
  }
  
  // Get the PTE to find physical address
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0) {
    free_swap_slot(slot);
    return -1;
  }
  
  uint64 pa = PTE2PA(*pte);
  
  // Safety check: ensure swap file is valid
  if(p->swapfile == 0) {
    free_swap_slot(slot);
    return -1;
  }
  
  // Write the page to swap file at the slot's offset
  // Each slot is PGSIZE bytes, so offset = slot * PGSIZE
  begin_op();
  ilock(p->swapfile);
  int written = writei(p->swapfile, 0, pa, slot * PGSIZE, PGSIZE);
  iunlock(p->swapfile);
  end_op();
  
  if(written != PGSIZE) {
    free_swap_slot(slot);
    return -1;
  }
  
  // Update resident page entry with swap information
  p->resident[resident_idx].swap_offset = slot;
  p->nswapped++;
  
  // ADD MAPPING: Store VA-to-slot mapping in swap_map table
  p->swap_map[slot].va = va;
  p->swap_map[slot].swap_slot = slot;
  
  // Log the swap out (Part 4 format)
  printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, va, slot);
  
  return 0;
}

// Read a page from swap file (called during page fault)
// va: virtual address where the page should be loaded
// slot: swap slot number where the page is stored
// Returns 0 on success, -1 on failure
int
swap_in(uint64 va, int slot)
{
  struct proc *p = myproc();
  
  if(p->swapfile == 0 || slot < 0 || slot >= MAX_SWAP_PAGES)
    return -1;
  
  // Allocate a physical page
  char *mem = kalloc();
  if(mem == 0) {
    // Try to evict a page to make room
    int victim_idx = find_victim_fifo();
    if(victim_idx < 0 || evict_page(victim_idx) < 0) {
      return -1;
    }
    // Try allocating again
    mem = kalloc();
    if(mem == 0)
      return -1;
  }
  
  // Read the page from swap file
  begin_op();
  ilock(p->swapfile);
  int read_bytes = readi(p->swapfile, 0, (uint64)mem, slot * PGSIZE, PGSIZE);
  iunlock(p->swapfile);
  end_op();
  
  if(read_bytes != PGSIZE) {
    kfree(mem);
    return -1;
  }
  
  // Map the page into the page table
  uint64 va_aligned = PGROUNDDOWN(va);
  int perm = PTE_U | PTE_R | PTE_W;  // Writable since it was swapped out
  if(mappages(p->pagetable, va_aligned, PGSIZE, (uint64)mem, perm) != 0) {
    kfree(mem);
    return -1;
  }
  
  // Free the swap slot (it's now back in memory)
  free_swap_slot(slot);
  p->nswapped--;
  
  // CLEAR MAPPING: Remove from swap_map table
  p->swap_map[slot].va = (uint64)-1;
  p->swap_map[slot].swap_slot = -1;
  
  return 0;
}
