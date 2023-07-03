#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
	if(argv[1] == 0 || strlen(argv[1]) < 1){
		printf(1, "invalid argument!\n");
		exit();
	}
	int ret_val;
	ret_val = myfunction(argv[1]);
	printf(1, "Return value: 0x%x\n", ret_val);
	exit();
}
