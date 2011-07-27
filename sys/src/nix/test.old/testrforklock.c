#include <u.h>
#include <libc.h>

/* 
	rm -f testrforklock.6 
	bind  /sys/src/9kron/include/libc.h /sys/include/libc.h
	bind -c /sys/src/9kron/libc /sys/src/libc
	6c testrforklock.c 
	6l -o 6.testrforklock testrforklock.6 
*/

void
usage(void)
{
	fprint(2, "Usage: testrfork nchilds\n");
	exits("usage");
}

QLock l;

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
	print("parent: my core no is %ux,"
		" my coretype is %ux\n", core, type);

	while(c-- > 0){
		switch(rfork(RFPROC|RFMEM|RFCORE)){
		case -1:
			sysfatal("rfork failed: %r");
		case 0:	
			core = getcore(&type);
			for(i=0; i < 100; i++){
				qlock(&l);
				print("child %d  my core no is %d,"
					" my coretype is %d\n", c, core, type);
				qunlock(&l);
			}
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

