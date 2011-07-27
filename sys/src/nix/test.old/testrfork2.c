#include <u.h>
#include <libc.h>

/* 
	rm -f testrfork2.6 
	bind  /sys/src/9kron/include/libc.h /sys/include/libc.h
	bind -c /sys/src/9kron/libc /sys/src/libc
	6c testrfork2.c
	6l -o 6.testrfork2 testrfork2.6
*/

void
usage(void)
{
	fprint(2, "Usage: testrfork\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int i;
	int rounds = 2;
	int core;
	int type;
	Waitmsg *msg;

	ARGBEGIN {
	default:
		usage();
	} 
	ARGEND

	core = getcore(&type);
	print("before RFORK my core no is %ux,"
		" my coretype is %ux\n", core, type);

	switch(rfork(RFPROC|RFCORE)){
	case -1:
		fprint(2, "rfork failed: %r\n");
		exits("rfork failed");	
	case 0:	
		for(i=0; i < 10; i++){
			core = getcore(&type);
			print("child my core no is %ux,"
				" my coretype is %ux\n", core, type);
		}

		if(rfork(RFCCORE) < 0){
			fprint(2, "rfork failed: %r\n");
			exits("rfork failed");	
		}

		for(i=0; i < 10; i++){
			core = getcore(&type);
			print("child my core no is %ux,"
				" my coretype is %ux\n", core, type);
		}

		if(rfork(RFCORE) < 0){
			fprint(2, "rfork failed: %r\n");
			exits("rfork failed");	
		}

		for(i=0; i < 10; i++){
			core = getcore(&type);
			print("child my core no is %ux,"
				" my coretype is %ux\n", core, type);
		}

		exits(nil);
	}
	for(i=0; i < 10; i++){
		core = getcore(&type);
		print("parent my core no is %ux,"
			" my coretype is %ux\n", core, type);
	}

	if(rfork(RFCORE) < 0){
		fprint(2, "rfork failed: %r\n");
		exits("rfork failed");	
	}

	for(i=0; i < 10; i++){
		core = getcore(&type);
		print("parent my core no is %ux,"
			" my coretype is %ux\n", core, type);
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

