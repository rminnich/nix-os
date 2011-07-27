/*
 !6c -FVTw nc.c && 6l -o 6.nc nc.6
 */


#include <u.h>
#include <libc.h>

static int dosyscall;
extern int sysr1(void);

void
thr(int , void *[])
{
	int i, n;

	for(i = 0; i < 500; i++){
		if(dosyscall)
			n = sysr1();
		else
			n = tsyscall(gettid(), gettid());
		USED(n);
		if(0)print("t%d: rc %d\n", gettid(), n);
	}
}

void
thrmain(int argc, void *argv[])
{
	int i, id;

	for(i = 0; i < 50; i++){
		if(0)print("t%d: arg[%d] = %s\n", gettid(), i, argv[i]);
		id = newthr("thr", thr, argc, argv);
		USED(id);
		if(0)print("t%d: newthr %d\n", gettid(), id);
	}
}

void
main(int argc, char *argv[])
{
	ARGBEGIN{
	case 's':
		dosyscall = 1;
		break;
	case 'n':
		dosyscall = 0;
		break;
	}ARGEND;

	nixsyscall();
	newthr("thrmain", thrmain, argc, argv);
	sysfatal("newthr returns");
}

