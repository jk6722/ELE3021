#include "types.h"
#include "stat.h"
#include "user.h"

void* 
func(void* arg) 
{

  int ret = (int) arg;
  char* temp;
  int addr;
  sleep(10);
  printf(1, "Hello Im func addr: %d\n", &addr);
  sleep(5);
  temp = malloc(3000 * 2);
  printf(1, "thread%d malloc %d\n", ret,temp);
  thread_exit((void*)ret);
  return 0;
}

int
main(int argc, char *argv[]) 
{
  thread_t th[3];
  void* retval[3];
  int addr;
  char* temp;
  printf(1, "HELLO, Im main addr: %d\n", &addr);

  thread_create(&th[0], func, (void*) 0);
  thread_join(th[0], &retval[0]);
  
  temp = malloc(3000 * 2);
  printf(1, "main0 malloc %d\n", temp);

  thread_create(&th[0], func, (void*) 0);
  thread_create(&th[1], func, (void*) 1);
  thread_join(th[0], &retval[0]);
  thread_join(th[1], &retval[1]);
  temp = malloc(3000 * 2);
  printf(1, "main1 malloc %d\n", temp);

  thread_create(&th[0], func, (void*) 0);
  thread_create(&th[1], func, (void*) 1);
  thread_create(&th[2], func, (void*) 2);
  thread_join(th[0], &retval[0]);
  thread_join(th[1], &retval[1]);
  thread_join(th[2], &retval[2]);
  temp = malloc(3000 * 2);
  printf(1, "main2 malloc %d\n", temp);
  exit();
}