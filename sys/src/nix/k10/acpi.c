#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#define l16get(p)	(((p)[1]<<8)|(p)[0])
#define l32get(p)	(((u32int)l16get(p+2)<<16)|l16get(p))

static u64int
l64get(u8int* p)
{
	/*
	 * Doing this as a define
	 * #define l64get(p)	(((u64int)l32get(p+4)<<32)|l32get(p))
	 * causes 8c to abort with "out of fixed registers" in
	 * rsdlink() below.
	 */
	return (((u64int)l32get(p+4)<<32)|l32get(p));
}

/*
 * A System Descriptor Table starts with a header of 4 bytes of signature
 * followed by 4 bytes of total table length then 28 bytes of ID information
 * (including the table checksum).
 * Only the signature and length are of interest. A byte array is used
 * rather than an embedded structure to avoid structure alignment
 * problems.
 */
typedef struct {				/* Differentiated System DT */
	u8int	sdthdr[36];			/* "DSDT" + length[4] + [28] */
	u8int	db[];				/* Definition Block */
} _DSDT_;

typedef struct {				/* Fixed ACPI DT */
	u8int	sdthdr[36];			/* "FACP" + length[4] + [28] */
	u8int	faddr[4];			/* Firmware Control Address */
	u8int	dsdt[4];			/* DSDT Address */
	u8int	_44_[200];			/* total table is 244 */
} _FACP_;

static void
acpigasdump(char* s, u8int* a)
{
	DBG("%s %#2.2ux %d %d %d %#16.16llux\n",
		s,
		a[0], a[1], a[2], a[3],
		l64get(a+4));
}

static void
acpifacpdump(_FACP_* facp)
{
	u8int *p;
	u32int r;

	if(!DBGFLG){
		SET(r); USED(r);
		return;
	}

	p = facp->_44_;
	DBG("Preferred PM Profile: %d\n", p[1]);
	DBG("SCI Int %d\n", l16get(p+2));
	DBG("SMI CMD %#8.8ux\n", l32get(p+4));
	DBG("ACPI Enable %#2.2ux\n", p[8]);
	DBG("ACPI Disable %#2.2ux\n", p[9]);
	DBG("S4BIOS REQ %#2.2ux\n", p[10]);
	DBG("PSTATE CNT %#2.2ux\n", p[11]);
	DBG("PM1a EVT BLK %#8.8ux\n", l32get(p+12));
	DBG("PM1b EVT BLK %#8.8ux\n", l32get(p+156));
	DBG("PM1a CNT BLK %#8.8ux\n", l32get(p+20));
	DBG("PM1b CNT BLK %#8.8ux\n", l32get(p+24));
	DBG("PM2 CNT BLK %#8.8ux\n", l32get(p+28));
	DBG("PM TMR BLK %#8.8ux\n", l32get(p+32));
	DBG("GPE0 BLK %#8.8ux\n", l32get(p+36));
	DBG("GPE1 BLK %#8.8ux\n", l32get(p+40));
	DBG("PM1 EVT LEN %d\n", p[44]);
	DBG("PM1 CNT LEN %d\n", p[45]);
	DBG("PM2 CNT LEN %d\n", p[46]);
	DBG("PM TMR LEN %d\n", p[47]);
	DBG("GPE0 BLK LEN %d\n", p[48]);
	DBG("GPE1 BLK LEN %d\n", p[49]);
	DBG("GPE1 BASE %d\n", p[50]);
	DBG("CST CNT %#2.2ux\n", p[51]);
	DBG("P LVL2 LAT %d\n", l16get(p+52));
	DBG("P LVL3 LAT %d\n", l16get(p+54));
	DBG("FLUSH SIZE %d\n", l16get(p+56));
	DBG("FLUSH STRIDE %d\n", l16get(p+58));
	DBG("DUTY OFFSET %d\n", p[60]);
	DBG("DUTY WIDTH %d\n", p[61]);
	DBG("DAY ALARM %d\n", p[62]);
	DBG("MON ALARM %d\n", p[63]);
	DBG("CENTURY %d\n", p[64]);
	DBG("IAPC BOOT ARCH %#4.4ux\n", l16get(p+65));
	DBG("Flags %#8.8ux\n", l32get(p+68));
	acpigasdump("RESET REG", p+72);
	DBG("RESET VALUE %#2.2ux\n", p[84]);
	DBG("X FIRMWARE CTRL %#16.16llux\n", l64get(p+88));
	DBG("X DSDT %#16.16llux\n", l64get(p+96));
	acpigasdump("X PM1a EVT BLK", p+104);
	acpigasdump("X PM1b EVT BLK", p+116);
	acpigasdump("X PM1a CNT BLK", p+128);
	acpigasdump("X PM1b CNT BLK", p+140);
	acpigasdump("X PM2 CNT BLK", p+152);
	acpigasdump("X PM TMR BLK", p+164);
	acpigasdump("X GPE0 BLK", p+176);
	acpigasdump("X GPE1 BLK", p+188);
}

static void
acpifacp(void* _facp_)
{
	u8int *p;
	int i, l, n;
	_FACP_ *facp;

	facp = _facp_;
	DBG("acpi: faddr @ %#8.8ux dsdt @ %#8.8ux\n",
		l32get(facp->faddr), l32get(facp->dsdt));

	if(DBGFLG > 1){
		//for(i = 0; i < 200; i++){
		//	DBG(" %2.2ux", facp->_44_[i]);
		//	if((i % 16) == 15)
		//		DBG("\n");
		//}
		//DBG("\n");
		acpifacpdump(facp);
	}

	if((p = vmap(l32get(facp->dsdt), 8)) == nil)
		return;
	if(memcmp(p, "DSDT", 4) != 0){
		vunmap(p, 8);
		return;
	}
	l = l32get(p+4);
	vunmap(p, 8);
	DBG("acpi: DSDT length %ud\n", l);

	if(DBGFLG > 1){
		if((p = vmap(l32get(facp->dsdt), l)) == nil)
			return;
		if(DBGFLG > 2)
			n = l;
		else
			n = 256;
		for(i = 0; i < n; i++){
			DBG(" %2.2ux", p[36+i]);
			if((i % 16) == 15)
				DBG("\n");
		}
		vunmap(p, l);
	}
	else
		USED(l);
}

typedef struct {				/* Multiple APIC DT */
	u8int	sdthdr[36];			/* "MADT" + length[4] + [28] */
	u8int	addr[4];			/* Local APIC Address */
	u8int	flags[4];
	u8int	structures[];
} _MADT_;

static void
acpiapic(void* _madt_)
{
	u8int *p;
	int i, l, n;
	_MADT_ *madt;

	madt = _madt_;
	DBG("acpi: Local APIC Address %#8.8ux, flags %#8.8ux\n",
		l32get(madt->addr), l32get(madt->flags));

	n = l32get(&madt->sdthdr[4]);
	p = madt->structures;
	for(i = offsetof(_MADT_, structures[0]); i < n; i += l){
		DBG("acpi: type %d, length %d\n", p[0], p[1]);
		switch(p[0]){
		default:
			break;
		case 0:				/* Processor Local APIC */
			DBG("acpi: ACPI Processor ID %d\n", p[2]);
			DBG("acpi: APIC ID %d\n", p[3]);
			DBG("acpi: Flags %#8.8ux\n", l32get(p+4));
			break;
		case 1:				/* I/O APIC */
			DBG("acpi: I/O APIC ID %d\n", p[2]);
			DBG("acpi: I/O APIC Address %#8.8ux\n", l32get(p+4));
			DBG("acpi: Global System Interrupt Base %#8.8ux\n",
				l32get(p+8));
			break;
		case 2:				/* Interrupt Source Override */
			DBG("acpi: IRQ %d\n", p[3]);
			DBG("acpi: Global System Interrupt %#8.8ux\n",
				l32get(p+4));
			DBG("acpi: Flags %#4.4ux\n", l16get(p+8));
			break;
		case 3:				/* NMI Source */
		case 4:				/* Local APIC NMI Structure */
		case 5:				/* Local APIC Address Override */
		case 6:				/* I/O SAPIC */
		case 7:				/* Local SAPIC */
		case 8:				/* Platform Interrupt Sources */
			break;
		}
		l = p[1];
		p += l;
	}
}

typedef struct {				/* _MCFG_ Descriptor */
	u8int	addr[8];			/* base address */
	u8int	segno[2];			/* segment group number */
	u8int	sbno;				/* start bus number */
	u8int	ebno;				/* end bus number */
	u8int	_12_[4];			/* reserved */
} _MCFGD_;

typedef struct {				/* PCI Memory Mapped Config */
	u8int	sdthdr[36];			/* "MCFG" + length[4] + [28] */
	u8int	_36_[8];			/* reserved */
	_MCFGD_	mcfgd[];			/* descriptors */
} _MCFG_;

static void
acpimcfg(void* _mcfg_)
{
	int i, n;
	_MCFG_ *mcfg;
	_MCFGD_ *mcfgd;

	mcfg = _mcfg_;
	n = l32get(&mcfg->sdthdr[4]);
	mcfgd = mcfg->mcfgd;
	for(i = offsetof(_MCFG_, mcfgd[0]); i < n; i += sizeof(_MCFGD_)){
		DBG("acpi: addr %#16.16llux segno %d sbno %d ebno %d\n",
			l64get(mcfgd->addr), l16get(mcfgd->segno),
			mcfgd->sbno, mcfgd->ebno);
		mcfgd++;
	}
}

typedef struct {				/* HPET DT */
	u8int	sdthdr[36];			/* "FACP" + length[4] + [28] */
	u8int	id[4];				/* Event Timer Bock ID */
	u8int	addr[12];			/* ACPI Format Address */
	u8int	seqno;				/* Sequence Number */
	u8int	minticks[2];			/* Minimum Clock Tick */
	u8int	attr;				/* Page Protection */
} _HPET_;

static void
acpihpet(void* _hpet_)
{
	_HPET_ *hpet;
	uintptr addr;
	int minticks;
	extern void hpetinit(int, uintptr, int);

	hpet = _hpet_;
	addr = l64get(&hpet->addr[4]);
	minticks = l16get(hpet->minticks);

	DBG("acpi: id %#ux addr %d %d %d %#p seqno %d ticks %d attr %#ux\n",
		l32get(hpet->id), hpet->addr[0], hpet->addr[1], hpet->addr[2],
		addr, hpet->seqno, minticks, hpet->attr);

	hpetinit(hpet->seqno, addr, minticks);
}

typedef struct {				/* Root System Description * */
	u8int	signature[8];			/* "RSD PTR " */
	u8int	rchecksum;			/* */
	u8int	oemid[6];
	u8int	revision;
	u8int	raddr[4];			/* RSDT */
	u8int	length[4];
	u8int	xaddr[8];			/* XSDT */
	u8int	xchecksum;			/* XSDT */
	u8int	_33_[3];			/* reserved */
} _RSD_;

static void*
rsdchecksum(void* addr, int length)
{
	u8int *p, sum;

	sum = 0;
	for(p = addr; length-- > 0; p++)
		sum += *p;
	if(sum == 0)
		return addr;

	return nil;
}

static void*
rsdscan(u8int* addr, int len, char* signature)
{
	int sl;
	u8int *e, *p;

	e = addr+len;
	sl = strlen(signature);
	for(p = addr; p+sl < e; p += 16){
		if(memcmp(p, signature, sl))
			continue;
		return p;
	}

	return nil;
}

static void*
rsdsearch(char* signature)
{
	uintptr p;
	u8int *bda;
	_RSD_ *rsd;

	/*
	 * Search for the data structure signature:
	 * 1) in the first KB of the EBDA;
	 * 2) in the BIOS ROM between 0xE0000 and 0xFFFFF.
	 */
	if(strncmp((char*)KADDR(0xFFFD9), "EISA", 4) == 0){
		bda = BIOSSEG(0x40);
		if((p = (bda[0x0F]<<8)|bda[0x0E])){
			if(rsd = rsdscan(KADDR(p), 1024, signature))
				return rsd;
		}
	}
	return rsdscan(BIOSSEG(0xE000), 0x20000, signature);
}

static void
rsdlink(void)
{
	_RSD_ *rsd;
	uchar *p, *sdt;
	int asize, i, l, n;
	uintptr dhpa, sdtpa;

	if((rsd = rsdsearch("RSD PTR ")) == nil)
		return;
	DBG("acpi: RSD PTR @ %#p, physaddr %#ux length %ud %#llux rev %d\n",
		rsd, l32get(rsd->raddr), l32get(rsd->length),
		l64get(rsd->xaddr), rsd->revision);

	if(rsd->revision == 2){
		if(rsdchecksum(rsd, 36) == nil)
			return;
		sdtpa = l64get(rsd->xaddr);
		asize = 8;
	}
	else{
		if(rsdchecksum(rsd, 20) == nil)
			return;
		sdtpa = l32get(rsd->raddr);
		asize = 4;
	}
	if((sdt = vmap(sdtpa, 8)) == nil)
		return;
	if((sdt[0] != 'R' && sdt[0] != 'X') || memcmp(sdt+1, "SDT", 3) != 0){
		vunmap(sdt, 8);
		return;
	}

	n = l32get(sdt+4);
	vunmap(sdt, 8);
	if((sdt = vmap(sdtpa, n)) == nil)
		return;
	if(rsdchecksum(sdt, n) == nil){
		vunmap(sdt, n);
		return;
	}
	for(i = 36; i < n; i += asize){
		if(asize == 8)
			dhpa = l64get(sdt+i);
		else
			dhpa = l32get(sdt+i);
		if((p = vmap(dhpa, 8)) == nil)
			continue;
		l = l32get(p+4);
		vunmap(p, 8);
		if((p = vmap(dhpa, l)) == nil)
			continue;
		if(rsdchecksum(p, l) != nil){
			DBG("acpi: %c%c%c%c\n", p[0], p[1], p[2], p[3]);
			if(memcmp(p, "FACP", 4) == 0)
				acpifacp(p);
			else if(memcmp(p, "APIC", 4) == 0)
				acpiapic(p);
			else if(memcmp(p, "MCFG", 4) == 0)
				acpimcfg(p);
			else if(memcmp(p, "HPET", 4) == 0)
				acpihpet(p);
		}
		vunmap(p, l);
	}
	vunmap(sdt, n);
}

void
acpilink(void)
{
	rsdlink();
}
