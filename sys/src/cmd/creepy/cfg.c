#include <u.h>
#include <libc.h>
#include <thread.h>
#include <bio.h>
#include <fcall.h>
#include <error.h>

#include "conf.h"
#include "dbg.h"
#include "dk.h"
#include "ix.h"
#include "net.h"
#include "fns.h"

/*
 * CAUTION: We keep the format used in fossil, but,
 * creepy rules are simpler:
 * names are ignored. uids are names:
 * 	Nemo is nemo and nobody else is nemo,
 *	new users should pick a different name. that one is taken.
 * there is no leadership.
 *	members may chown files to the group, may chmod files in the group,
 *	and may chgrp files to the group.
 *	Plus, to donate a file, you must be the owner.
 *
 * sorry. 9 rules were too complex to remember.
 */

typedef struct Usr Usr;
typedef struct Member Member;

struct Member
{
	Member *next;
	char *uid;
	Usr *u;
};

struct Usr
{
	Usr *next;
	char *uid;
	char *lead;
	Member *members;
};

char *defaultusers = 
	"adm:adm:adm:sys\n"
	"elf:elf:elf:sys\n"
	"none:none::\n"
	"noworld:noworld::\n"
	"sys:sys::glenda\n"
	"glenda:glenda:glenda:\n";

static Usr *usrs[Uhashsz];

static uint
uhash(char* s)
{
	uchar *p;
	u32int hash;

	hash = 0;
	for(p = (uchar*)s; *p != '\0'; p++)
		hash = hash*7 + *p;

	return hash % Uhashsz;
}

static Usr*
findusr(char *uid)
{
	Usr *u;

	for(u = usrs[uhash(uid)]; u != nil; u = u->next)
		if(strcmp(u->uid, uid) == 0)
			return u;
	return nil;
}

int
member(char *uid, char *member)
{
	Usr *u;
	Member *m;

	if(strcmp(uid, member) == 0)
		return 1;
	u = findusr(uid);
	if(u == nil)
		return 0;
	for(m = u->members; m != nil; m = m->next)
		if(strcmp(member, m->uid) == 0)
			return 1;
	return 0;
}

static Usr*
mkusr(char *uid, char *lead)
{
	Usr *u;
	uint h;

	h = uhash(uid);
	for(u = usrs[h]; u != nil; u = u->next)
		if(strcmp(u->uid, uid) == 0)
			error("dup uid %s", uid);

	u = mallocz(sizeof *u, 1);
	u->uid = strdup(uid);
	if(lead != 0)
		u->lead = strdup(lead);
	u->next = usrs[h];
	usrs[h] = u;
	return u;
}

static void
addmember(Usr *u, char *n)
{
	Member *m;

	for(m = u->members; m != nil; m = m->next)
		if(strcmp(m->uid, n) == 0)
			error("%s already a member of %s", n, u->uid);
	m = mallocz(sizeof *m, 1);
	m->uid = strdup(n);
	m->next = u->members;
	u->members = m;
}

static void
freemember(Member *m)
{
	if(m == nil)
		return;
	free(m->uid);
	free(m);
}

static void
freeusr(Usr *u)
{
	Member *m;

	if(u == nil)
		return;
	while(u->members != nil){
		m = u->members;
		u->members = m->next;
		freemember(m);
	}
	free(u->uid);
	free(u->lead);
	free(u);
}

void
clearusers(void)
{
	Usr *u;
	int i;

	for(i = 0; i < nelem(usrs); i++)
		while(usrs[i] != nil){
			u = usrs[i];
			usrs[i] = u->next;
			freeusr(u);
		}
}

static void
checkmembers(Usr *u)
{
	Member *m;

	d9print("checkmembers %s\n", u->uid);
	for(m = u->members; m != nil; m = m->next)
		if((m->u = findusr(m->uid)) == nil){
			fprint(2, "no user '%s'\n", m->uid);
			consprint("no user '%s'\n", m->uid);
		}
}

void
parseusers(char *u)
{
	char *c, *nc, *p, *np, *args[5];
	int nargs, i;
	Usr *usr;

	u = strdup(u);
	if(catcherror()){
		free(u);
		error(nil);
	}
	p = u;
	do{
		np = utfrune(p, '\n');
		if(np != nil)
			*np++ = 0;
		c = utfrune(p, '#');
		if(c != nil)
			*c = 0;
		if(catcherror()){
			fprint(2, "users: %r\n");
			consprint("users: %r\n");
			continue;
		}
		if(*p == 0)
			continue;
		nargs = getfields(p, args, nelem(args), 0, ":");
		if(nargs != 4)
			error("wrong number of fields %s", args[0]);
		if(*args[0] == 0 || *args[1] == 0)
			error("null uid or name");
		usr = mkusr(args[0], args[2]);
		for(c = args[3]; c != nil; c = nc){
			if(*c == 0)
				break;
			nc = utfrune(c, ',');
			if(nc != nil)
				*nc++ = 0;
			addmember(usr, c);
		}
		noerror();
	}while((p = np) != nil);
	for(i = 0; i < nelem(usrs); i++)
		for(usr = usrs[i]; usr != nil; usr = usr->next)
			checkmembers(usr);
	noerror();
	free(u);
}

/*
 * TODO: register multiple fids for the cons file by keeping a list
 * of console channels.
 * consread will have to read from its per-fid channel.
 * conprint will have to bcast to all channels.
 *
 * With that, multiple users can share the same console.
 * Although perhaps it would be easier to use C in that case.
 */

void
consprint(char *fmt, ...)
{
	va_list	arg;
	char *s, *x;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	/* consume some message if the channel is full */
	while(nbsendp(fs->consc, s) == 0)
		if((x = nbrecvp(fs->consc)) != nil)
			free(x);
}

long
consread(char *buf, long count)
{
	char *s;
	int tot, nr;

	if(count <= 0)		/* shouldn't happen */
		return 0;
	s = recvp(fs->consc);
	tot = 0;
	do{
		nr = strlen(s);
		if(tot + nr > count)
			nr = count - tot;
		memmove(buf+tot, s, nr);
		tot += nr;
		free(s);
	}while((s = nbrecvp(fs->consc)) != nil && tot + 80 < count);
	/*
	 * +80 to try to guarantee that we have enough room in the user
	 * buffer for the next received string, or we'd drop part of it.
	 * Most of the times each string is a rune typed by the user.
	 * Other times, it's the result of a consprint() call.
	 */
	return tot;
}

static void
cdump(int, char *argv[])
{
	fsdump(strcmp(argv[0], "dumpall") == 0);
}

static void
csync(int, char**)
{
	fssync();
	consprint("synced\n");
}

static void
chalt(int, char**)
{
	fssync();
	threadexitsall(nil);
}

static void
cusers(int argc, char *argv[])
{
	int i;
	Usr *u;

	switch(argc){
	case 1:
		for(i = 0; i < nelem(usrs); i++)
			for(u = usrs[i]; u != nil; u = u->next)
				consprint("%s\n", u->uid);
		break;
	case 2:
		if(strcmp(argv[1], "-r") == 0)
			consprint("-r not implemented\n");
		else if(strcmp(argv[1], "-w") == 0)
			consprint("-w not implemented\n");
		else
			consprint("usage: users [-r|-w]\n");
		break;
	default:
		consprint("usage: users [-r|-w]\n");
	}
}

static void chelp(int, char**);

static Cmd cmds[] =
{
	{"dump",	cdump, 1, "dump"},
	{"dumpall",	cdump, 1, "dumpall"},
	{"sync",	csync, 1, "sync"},
	{"halt",	chalt, 1, "halt"},
	{"users",	cusers, 0, "users [-r|-w]"},
	{"?",		chelp, 1, "?"},
};

static void
chelp(int, char**)
{
	int i;

	consprint("commands:\n");
	for(i = 0; i < nelem(cmds); i++)
		if(strcmp(cmds[i].name, "?") != 0)
			consprint("> %s\n", cmds[i].usage);
}

void
consinit(void)
{
	consprint("creepy> ");
}


long
conswrite(char *ubuf, long count)
{
	char *c, *p, *np, *args[5];
	int nargs, i, nr;
	Rune r;
	static char buf[80];
	static char *s, *e;

	if(count <= 0)
		return 0;
	if(s == nil){
		s = buf;
		e = buf + sizeof buf;
	}
	for(i = 0; i < count && s < e-UTFmax-1; i += nr){
		nr = chartorune(&r, ubuf+i);
		memmove(s, ubuf+i, nr);
		s += nr;
		consprint("%C", r);
	}
	*s = 0;
	if(s == e-1){
		s = buf;
		*s = 0;
		error("command is too large");
	}
	if(utfrune(buf, '\n') == 0)
		return count;
	p = buf;
	do{
		np = utfrune(p, '\n');
		if(np != nil)
			*np++ = 0;
		c = utfrune(p, '#');
		if(c != nil)
			*c = 0;
		nargs = tokenize(p, args, nelem(args));
		if(nargs < 1)
			continue;
		for(i = 0; i < nelem(cmds); i++){
			if(strcmp(args[0], cmds[i].name) != 0)
				continue;
			if(cmds[i].nargs != 0 && cmds[i].nargs != nargs)
				consprint("usage: %s\n", cmds[i].usage);
			else
				cmds[i].f(nargs, args);
			break;
		}
		if(i == nelem(cmds))
			consprint("'%s'?\n", args[0]);
	}while((p = np) != nil);
	s = buf;
	*s = 0;
	consprint("creepy> ");
	return count;
}

