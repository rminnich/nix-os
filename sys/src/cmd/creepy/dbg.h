/*
 * '9': 9p
 * 'D': disk
 * 'F': slices, indirects, dirnth
 * 'M': mblk/dblk gets puts
 * 'R': block read
 * 'W': block write
 * 'd': general debug
 * 'P': procs
 * 'x': ix
 */
#define d9print	if(!dbg['9']){}else print
#define dDprint	if(!dbg['D']){}else print
#define dFprint	if(!dbg['F']){}else print
#define dMprint	if(!dbg['M']){}else print
#define dRprint	if(!dbg['R']){}else print
#define dWprint	if(!dbg['W']){}else print
#define dxprint	if(!dbg['x']){}else print
#define dPprint	if(!dbg['P']){}else print
extern char dbg[];
