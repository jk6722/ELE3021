#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  // __asm__("int $128"); // call interrupt 128
  int called_lock = 0;
  if(argv[1] != 0){
    schedulerLock(atoi(argv[1]));
    called_lock = 1;
  }
  for(unsigned long long i = 0; i < 10000000000000; i++){
    printf(1, "i: %d\n", i);
    if(i == 1000 && called_lock)
      schedulerUnlock(2019049716);
  }
  exit();
}