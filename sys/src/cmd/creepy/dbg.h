/*
 * 'd': general debug
 * 'D': disk
 * 'W': block write
 * 'R': block read
 * '9': 9p
 */
#define dDprint	if(!dbg['D']){}else print
#define dRprint	if(!dbg['R']){}else print
#define dWprint	if(!dbg['W']){}else print
#define d9print	if(!dbg['9']){}else print
extern char dbg[];
