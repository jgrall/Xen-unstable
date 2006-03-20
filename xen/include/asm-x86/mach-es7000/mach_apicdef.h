#ifndef __ASM_MACH_APICDEF_H
#define __ASM_MACH_APICDEF_H

static inline unsigned get_apic_id(unsigned long x) 
{ 
	return (((x)>>24)&0xFF);
} 

#define		GET_APIC_ID(x)	get_apic_id(x)

#endif
