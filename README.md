[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/MXQf8Ibj)
  demand paging
  we shouldnt allocate physical page of RAM for a virtual address till the very last point 
  we should allocate only when the program tries to read or write from that address 
  we will have some bits like dirty , present 
  cpu hands the address to mmu now mmu will look upto the address in the page table and checks
  if present bit is cleared or not if so then process cant proceed it stops and raises an interrupt called page fault 
  now os wakes up and then handles using handlers job
    if address is invalide terminate 
    valid but wasnt in the ram 
        Is it part of the program's code/text? The OS finds the program's executable file on disk, reads the specific 4096-byte chunk corresponding to that address, allocates a fresh page of physical RAM, copies the data into it, and updates the page table to map the virtual address to this new physical page (and importantly, sets the PTE_P bit to 1).
        Is it part of the program's heap or stack? This memory doesn't come from a file. The OS simply allocates a fresh page of physical RAM, fills it with zeros, and updates the page table to map the address to it.
    once this part is done interrupt is fixed and mmu can find this address


    Modify exec: Don't allocate memory and load the whole program. Just set up the page table entries as "not present."
    Modify sbrk: When the heap grows, don't allocate memory. Just update the process's size (proc->sz)




In trap.c 
    all interrupt will be handled so pagefault number is 14 so we have to handle pagefault in it 

In vm.c 
   virtual  memory management fns 
    allocuvm(): This function is called by sbrk (via growproc) to allocate and map heap 
    memory. We'll need to change it to not allocate physical memory (kalloc) immediately.

    loaduvm(): This is called by exec to load a program segment from a file into memory. 
    We'll need to change it to set up the PTEs as not-present instead of allocating and loading.
    We will likely add our main handler function, let's call it handle_page_fault(), inside this file.


in exec.c 
    it calls vm.c to set address space so have to call those 



Hardware Register rcr2(): When a page fault occurs, the x86 CPU helpfully stores the virtual address that 
caused the fault in a special control register called CR2. xv6 provides a function rcr2() to read this value. This is the first thing our handler will do.




Create the Page Fault Handler (handle_page_fault in vm.c):

uint va = rcr2(); // Get the faulting address.

Round va down to the nearest page boundary. Let's call this page_va.

Validity Check: This is crucial. Check if page_va is a legal address for this process.

Is it a NULL pointer access (va < PGSIZE)? Invalid.

Is it in the heap region (i.e., page_va < p->sz)? Valid.

Is it the "guard page" just below the stack (page_va == p->stack_top - PGSIZE)? Valid (the stack needs to grow).

Is it part of the code/data? This is a bit trickier. exec needs to have defined the valid code/data range. The handler needs to check if page_va falls within it.

Anything else? Invalid.

If Invalid: Log the error and return a failure code. trap.c will see this and kill the process.

If Valid:

Allocate a physical page: char* mem = kalloc();. Check if it fails (this is for the next part of the assignment).

memset(mem, 0, PGSIZE); // Zero out the new page.

Determine Source:

If it was a heap or stack access, the zeroed page is all we need.

If it was a code/data access, we now need to load the content from the executable file into mem. We'll need the file's inode and the offset within the file to read from. exec must save this information somewhere accessible.

Map the Page: Call mappages() to map page_va to the physical address of mem with the correct permissions (PTE_W, PTE_U, etc.).

Log Everything: Print the required cprintf statements for the fault, allocation/load, and the new resident page with its sequence number.

Return success.


page replacement policy 

description

I have implemented the Second Chance (Clock) algorithm as the alternative page replacement policy. This is a practical enhancement over pure FIFO that gives frequently accessed pages a better chance of staying in memory.

working 
 The algorithm maintains a circular clock hand that sweeps through the resident page array
 For each page encountered, it checks a software maintained reference bit 
 If ref_bit is set page was recently accessed
    Clear the ref_bit
    Give the page a "second chance" by moving to the next page
    Continue scanning
 If ref_bit is clear page hasnt been accessed since last scan
    Select this page as the victim for eviction
    Stop scanning
 The clock hand advances circularly, ensuring fair treatment of all pages

This creates a simple approximation of LRU  without the overhead of maintaining timestamps or complex data structures.


 Much simpler than full LRU, NRU with multiple bits, or WSClock
    Uses only one reference bit per page 
    Single integer clock hand pointer
    No complex sorting or priority queues

 Better than pure FIFO
    FIFO can evict frequently used pages just because they are old
    Second Chance holds recently accessed pages from eviction
    Approximates LRU behavior at minimal cost

 Simple and portable
    Reference bit set when page is added to resident set
    Reference bit set when page is written to (dirty)
    Reference bit set when page is swapped back in
    Cleared during victim selection to give second chance

 Circular scanning prevents starvation
    All pages get equal consideration
    Clock hand ensures we don't repeatedly check same pages



Not as optimal as true LRU
In worst cases  behaves like FIFO
 May require multiple sweeps to find a victim but capped at 2 
 Software tracking has slight overhead compared to hardware maintained



 find_victim_second_chance() vm.c
    Main victim selection algorithm
    Sweeps through resident[] array circularly
    Checks and clears PTE_A bits
    Logs victim with algorithm name and clock position
   
 evict_page() 
    Adjusted to maintain clock_hand consistency
    When removing element at index idx:
    If clock_hand > idx: decrement it (array shifted left)
    If clock_hand == idx: keep same (now points to next page)
    Wrap around if needed

  USE_SECOND_CHANCE 
   Set to 1: Uses Second Chance algorithm
   Set to 0: Uses original FIFO algorithm



