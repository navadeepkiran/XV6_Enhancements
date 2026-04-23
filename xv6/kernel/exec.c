#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

//static int loadseg(pde_t *, uint64, struct inode *, uint, uint);
static int setup_lazy_segment(pagetable_t, uint64, uint64);


int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  struct inode *new_execfile = 0;  // Will hold reference to new executable file

  begin_op();

  // Open the executable file.
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  //  Keep a reference to the new executable file for demand loading
  // store this temporarily and only commit it if exec succeeds
  new_execfile = idup(ip);
  if(new_execfile == 0)
    goto bad;

  //  Dont load program into memory immediately.
  // instead, just record segment information for lazy loading on page faults.
  p->nsegments = 0;
  p->text_start = 0;
  p->text_end = 0;
  
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    
    // Record segment information for demand paging
    if(p->nsegments >= MAX_SEGMENTS)
      goto bad;
    p->segments[p->nsegments].vaddr = ph.vaddr;
    p->segments[p->nsegments].filesz = ph.filesz;
    p->segments[p->nsegments].memsz = ph.memsz;
    p->segments[p->nsegments].offset = ph.off;
    p->segments[p->nsegments].flags = ph.flags;
    
    p->nsegments++;
    
    // Just set up page table entries as invalid (not present)
    // Pages will be allocated and loaded on demand when accessed
    if(setup_lazy_segment(pagetable, sz, ph.vaddr + ph.memsz) < 0)
      goto bad;
    
    // Track text and data boundaries
    if(ph.flags & 0x1) { // executable segment (text)
      p->text_start = ph.vaddr;
      p->text_end = ph.vaddr + ph.memsz;
    }
    
    uint64 sz1 = ph.vaddr + ph.memsz;
    if(sz1 > sz)
      sz = sz1;
  }
  
  p->data_end = sz;  // End of data segments, beginning of heap
  
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  //  Allocate stack lazily.
  // Set up virtual address space for stack, but don't allocate physical pages yet.
  // Pages will be allocated on first access (page fault).
  sz = PGROUNDUP(sz);
  
  // Allocate first stack page eagerly to hold arguments
  // (We need this to copy argv strings before the process starts)
  char *mem = kalloc();
  if(mem == 0)
    goto bad;
  memset(mem, 0, PGSIZE);
  
  // Map the first user stack page
  sp = sz + (USERSTACK+1)*PGSIZE;  // Top of stack
  stackbase = sp - USERSTACK*PGSIZE;
  if(mappages(pagetable, sp - PGSIZE, PGSIZE, (uint64)mem, PTE_R|PTE_W|PTE_U) != 0){
    kfree(mem);
    goto bad;
  }
  
  // Set up guard page (invalid/unmapped)
  // Note: We don't map the guard page, so it will cause a fault if accessed
  
  // Update sz to include the full stack region
  sz = sz + (USERSTACK+1)*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  
  // Replace old executable file reference with new one
  struct inode *old_execfile = p->execfile;
  p->execfile = new_execfile;  // Set new execfile
  
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);
  
  // Now release the old execfile after freeing the old page table
  if(old_execfile) {
    iput(old_execfile);
  }

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  // Clean up on error
  // Release the new_execfile reference we acquired
  if(new_execfile) {
    iput(new_execfile);
  }
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
// static int
// loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
// {
//   uint i, n;
//   uint64 pa;

//   for(i = 0; i < sz; i += PGSIZE){
//     pa = walkaddr(pagetable, va + i);
//     if(pa == 0)
//       panic("loadseg: address should exist");
//     if(sz - i < PGSIZE)
//       n = sz - i;
//     else
//       n = PGSIZE;
//     if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
//       return -1;
//   }
  
//  return 0;
//}

// Setup lazy segment: Reserve virtual address space but don't allocate physical pages.
// For demand paging, we don't create valid PTEs here. Pages will be allocated
// and loaded on first access (page fault).
// Returns 0 on success, -1 on failure.
static int
setup_lazy_segment(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  
  // just ensure the page table structure exists 
  // but leave leaf PTEs invalid. This allows the virtual address space to be
 
  
  // In xv6, we can simply return success - the walk() function with alloc=1
  // will create intermediate page tables when needed during page fault handling.
  
  // Just validate the sizes
  if(newsz < oldsz)
    return -1;
    
  return 0;
}
