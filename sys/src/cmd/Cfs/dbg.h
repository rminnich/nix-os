/*
 * 'd': general debug
 * 'D': disk
 */
#define dDprint	if(!dbg['D']){}else print
extern char dbg[];
