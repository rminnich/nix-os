
#include <u.h>
#include <libc.h>
#include <thread.h>	/* chancreate */
#include <fcall.h>
#include "conf.h"
#include "msg.h"
#include "mpool.h"
#include "dbg.h"

static Mbuf *bufs;
static Lock bufslck;
static int nbufs;
static int nmaxbufs;

void 
freepoolmsg(Msg *m)
{
	int i;
	Mbuf *mb;
	Mpool *mp;

	mb = m->aux;
	assert(&mb->m == m);

	dPprint("freepoolmsg %#p pc %#ulx\n", mb, getcallerpc(&m));
	mp = mb->mp;
	mb->m.hdr = nil;
	mb->m.io[0].bp = mb->buf;
	mb->m.io[0].rp = mb->buf;
	mb->m.io[0].wp = mb->buf;
	mb->m.io[0].ep = mb->m.io[0].bp + mp->msize;
	for(i = 1; i < nelem(mb->m.io); i++){
		mb->m.io[i].bp = nil;
		mb->m.io[i].rp = nil;
		mb->m.io[i].wp = nil;
		mb->m.io[i].ep = nil;
	}
	sendp(mp->bc, mb);
	lock(mp);
	mp->nfree++;
	mp->nfrees++;
	unlock(mp);
}

Mpool* 
newpool(ulong msize, int nmsg)
{
	Mpool *mp;
	Mbuf *mb;
	int i;

	if(nmsg == 0)
		sysfatal("newpool: called for 0 messages");
	mp = mallocz(sizeof *mp, 1);
	mp->msize = msize;
	mp->bc = echancreate(sizeof(Mbuf*), nmsg);
	for(i = 0; i < nmsg; i++){
		mb = nil;
		if(bufs != nil){
			lock(&bufslck);
			if(bufs != nil){
				mb = bufs;
				bufs = bufs->next;
				nbufs--;
			}
			unlock(&bufslck);
		}
		if(mb == nil)
			mb = malloc(sizeof(Mbuf) + msize);
		if(mb == nil)
			break;
		memset(mb, 0, sizeof *mb);
		mb->mp = mp;
		mb->m.io[0].bp = mb->buf;
		mb->m.io[0].rp = mb->buf;
		mb->m.io[0].wp = mb->buf;
		mb->m.io[0].ep = mb->buf + msize;
		mb->m.aux = mb;
		mb->m.free = freepoolmsg;
		mp->nmsg++;
		mp->nfree++;
		sendp(mp->bc, mb);
	}
	if(i == 0){
		chanfree(mp->bc);
		free(mp);
		return nil;
	}
	return mp;
}

void
poolstats(Mpool *mp)
{
	dprint("pool %p: nmsg %d nfree %d nminfree %d nallocs %uld nfrees %uld\n",
		mp, mp->nmsg, mp->nfree, mp->nminfree, mp->nallocs, mp->nfrees);
	dprint("nbufs %d nmaxbufs %d\n", nbufs, nmaxbufs);
}

/*
 * To free a pool we must collect all messages allocated
 * from it.
 * Note that the caller might have given a message to other process
 * which might free it after freepool is called.
 * e.g., a client may terminate a session and release its pool, but
 * the session read process may be still terminating and holding
 * its last message buffer.
 */
void 
freepool(Mpool *mp)
{
	Mbuf *mb;
	int i;

	if(mp == nil)
		return;
	if(mp->freepool != nil){
		mp->freepool(mp);
		return;
	}
	for(i = 0; i < mp->nmsg; i++){
		mb = recvp(mp->bc);
		lock(&bufslck);
		mb->next = bufs;
		mb->mp = nil;
		mb->m.free = nil;
		bufs = mb;
		nbufs++;
		if(nmaxbufs < nbufs)
			nmaxbufs = nbufs;
		unlock(&bufslck);
	}
	poolstats(mp);
	chanfree(mp->bc);
	memset(mp, 9, sizeof *mp);
	free(mp);
}

Msg* 
newmsg(Mpool *mp)
{
	Mbuf *mb;

	if(mp == nil)
		sysfatal("newmsg: nil pool");

	mb = recvp(mp->bc);
	setmalloctag(mb, getcallerpc(&mp));
	dPprint("newmsg %#p pc %#ulx\n", mb, getcallerpc(&mp));
	lock(mp);
	mp->nfree--;
	mp->nallocs++;
	if(mp->nfree < mp->nminfree)
		mp->nminfree = mp->nfree;
	unlock(mp);
	return &mb->m;
}

