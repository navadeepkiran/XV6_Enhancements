#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  char *sp = (char *) r_sp();
  printf("DEBUG: stacktest original sp=%p\n", sp);
  sp -= PGSIZE * 256;  // Try to access well below the stack
  printf("DEBUG: stacktest trying to access sp=%p\n", sp);
  // this should trigger a page fault and trap
  printf("stacktest: read below stack %d\n", *sp);
  exit(0);
}