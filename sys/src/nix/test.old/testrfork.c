#include <u.h>
#include <libc.h>

/* 
	rm -f testrfork.6 
	bind  /sys/src/9kron/include/libc.h /sys/include/libc.h
	bind -c /sys/src/9kron/libc /sys/src/libc
	6c testrfork.c 
	6l -o 6.testrfork testrfork.6 
*/

void
usage(void)
{
	fprint(2, "Usage: testbuf.c\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int i;
	int rounds = 2;
	int core;
	int type;

	ARGBEGIN {
	default:
		usage();
	} 
	ARGEND

	core = getcore(&type);
	print("before RFORK my core no is %ux,"
		" my coretype is %ux\n", core, type);

	while(rounds-- > 0){
		if(rfork(RFCORE) < 0){
			fprint(2, "rfork failed: %r\n");
			exits("rfork failed");	
		}
	
		for(i=0; i < 10; i++){
			core = getcore(&type);
			print("after RFORK my core no is %ux,"
				" my coretype is %ux\n", core, type);
		}

		if(rfork(RFCCORE) < 0){
			fprint(2, "rfork failed: %r\n");
			exits("rfork failed");	
		}

		for(i=0; i < 10; i++){
			core = getcore(&type);
			print("now I am at TC my core no is %ux,"
				" my coretype is %ux\n", core, type);
		}
	}

	exits(nil);
}

