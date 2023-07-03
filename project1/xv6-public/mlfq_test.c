#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_CHILD 7
#define NUM_LOOP 5000000

int child_num;
int create_child(void){
  for(int i = 0; i<NUM_CHILD; i++){
    int pid = fork();
    if(pid == 0){
      // child process
      child_num = i;
      sleep(10);
      return 0;
    }
  }
  // parent process
  return 1;
}

void exit_child(int is_parent) {
	if (is_parent){
		while (wait() != -1); // wait children to terminate
  }
	exit();
}

int main()
{
  int is_parent;
  is_parent = create_child();

	if (!is_parent) {
		int cnt[4] = {0, }; //count L0, L1, L2, Lock
		for (int i = 0; i < NUM_LOOP; i++) {
      if(i % 1000000 == 0 && child_num % 2){
        schedulerLock(2019049716);
        // call schedulerLock if odd num child
      }
			cnt[getLevel()]++;
		}
		printf(1, "process %d: L0=%d, L1=%d, L2=%d, Lock=%d\n", child_num, cnt[0], cnt[1], cnt[2], cnt[3]);
	}
	exit_child(is_parent);
}
