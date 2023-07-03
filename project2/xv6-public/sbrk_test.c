#include "types.h"
#include "user.h"

#define NTHREAD 10


void* sbrkmain(void* arg){
  sbrk(4096 * (int)arg);
  thread_exit(0);
  return 0;
}

int main(void) {
  thread_t thread[10];
  void* retval;
  for(int i = 0; i < NTHREAD; i++){
    thread[i] = i;
    if(thread_create(&thread[i], sbrkmain, (void*)i) != 0){
      printf(1, "panic: sbrk\n");
      exit();
    }
  }
  for (int i = 0; i < NTHREAD; i++){
    if (thread_join(thread[i], &retval) != 0){
      printf(1, "panic:  thread_join\n");
      exit();
    }
  }
  while(1);
}