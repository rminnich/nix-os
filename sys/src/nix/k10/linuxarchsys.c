#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "../port/error.h"
#include <tos.h>
#include "ureg.h"
#include	"amd64.h"

/* from linux */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

void
arch_prctl(Ar0*ar0, Ureg *ureg, va_list list)
{
	uintptr va;
	int code;
	code = va_arg(list, int);
	va = va_arg(list, uintptr);
	if (up->linux & 128) print("%d:arch_prctl code %x va %p: ", up->pid, code, va);
	/* always make sure it's a valid address, no matter what the command */
	validaddr((void *)va, 8, code > ARCH_SET_FS);
	switch(code) {
	case ARCH_SET_GS:
	case ARCH_GET_GS:
		error("not yet");
		break;
	case ARCH_SET_FS:
		memmove(&ureg->r11, &va, 4);
		ar0->i = 0;
		break;
	case ARCH_GET_FS:
		memmove((void *)va, &ureg->r11, 4);
		ar0->i = 0;
		break;
	default: 
		error("Bad code");
		break;
	}
}

