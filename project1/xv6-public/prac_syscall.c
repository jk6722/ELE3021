#include "types.h"
#include "defs.h"

//System Call

int myfunction(char* str) {
	cprintf("%s\n", str); // print on console
	return 0xABCDABCD;
}

int sys_myfunction(void) { // wrapper function
	char* str;

	if(argstr(0, &str) < 0)
		return -1;
	return myfunction(str);
}
