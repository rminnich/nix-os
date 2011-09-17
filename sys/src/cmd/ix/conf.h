enum
{
	KiB = 1024,
	Stack = 8*KiB,	/* stack size for threads */

	Hbufsz = 8,	/* room for message headers */
	Msgsz = 8*KiB,	/* message size */
	Smsgsz = 64,	/* small message size */
	Nmsgs = 16,	/* # of messages per session */

	Maxid = 255,	/* # of channels per session */
	Nses = 512,	/* max # of sessions */

	Nchs = 64,	/* max # of chans in ix */

	Nels = 64,	/* max # of elems in path */

	Maxwritewin = 10,	/* max # of outstanding writes */
	Incr = 16,
};
