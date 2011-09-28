#pragma	lib	"liberror.a"
#pragma src "/sys/src/liberror"

typedef struct Channel Channel;

#pragma incomplete Channel;

#pragma varargck		argpos	esmprint 1

char*	estrdup(char*);
void*	emalloc(int);
void*	emallocz(int, int);
void*	erealloc(void*,int);
char*	esmprint(char *fmt, ...);
Channel*echancreate(int, int);
