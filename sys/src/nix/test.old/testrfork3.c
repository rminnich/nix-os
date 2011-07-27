#include <u.h>
#include <libc.h>

/* 
	rm -f testrfork3.6 
	bind  /sys/src/9kron/include/libc.h /sys/include/libc.h
	bind -c /sys/src/9kron/libc /sys/src/libc
	6c -FVTw testrfork3.c
	6l -o 6.testrfork3 testrfork3.6
*/

void
usage(void)
{
	fprint(2, "Usage: testrfork nchilds\n");
	exits("usage");
}


char x[200];

void
main(int argc, char *argv[])
{
	int i;
	int c;
	int core;
	int type;
	Waitmsg *msg;

	ARGBEGIN {
	default:
		usage();
	} 
	ARGEND

	if(argc != 1)
		usage();

	c = atoi(argv[0]);
	if(c <= 0)
		usage();

	core = getcore(&type);
	print("parent: before RFORK my core no is %ux,"
		" my coretype is %ux\n", core, type);

	while(c-- > 0){
		switch(rfork(RFPROC|RFCORE)){
		case -1:
			fprint(2, "rfork failed: %r\n");
			break; /* remove to break it */
		case 0:
			x[32] = 0xab;

			for(i=0; i < 10; i++){
				core = getcore(&type);
				print("child %d  my core no is %d,"
					" my coretype is %d\n", c, core, type);
			}
			if(c == 0)
				exits("calm down, child #0 was supposed to fail");
			exits(nil);
		}
	}

	while((msg = wait()) != nil){
		if(msg->msg[0] == 0)
			print("parent: child pid:%d exited ok\n", msg->pid);
		else 
			print("parent: child pid:%d failed: %s\n", msg->pid, msg->msg);
		free(msg);
	}
	print("parent: done\n");	
	exits(nil);
}

