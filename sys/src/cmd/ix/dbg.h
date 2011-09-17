/*
 * 'P': mpools
 * 'd': general debug
 * 'f': fs
 * 'm': messages
 * 'p': processes
 * 's': session debug
 * 't': test client
 * 'n': nsfile
 * 'c': cache
 */
#define dPprint	if(!dbg['P']){}else print
#define dcprint	if(!dbg['c']){}else print
#define dfprint if(!dbg['f']){}else print
#define dmprint	if(!dbg['m']){}else print
#define dnprint	if(!dbg['n']){}else print
#define dpprint	if(!dbg['p']){}else print
#define dprint	if(!dbg['d']){}else print
#define dsprint	if(!dbg['s']){}else print
#define dtprint	if(!dbg['t']){}else print
extern char dbg[];
