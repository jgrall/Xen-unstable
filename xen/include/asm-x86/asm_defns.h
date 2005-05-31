
#ifndef __X86_ASM_DEFNS_H__
#define __X86_ASM_DEFNS_H__

/* NB. Auto-generated from arch/.../asm-offsets.c */
#include <asm/asm-offsets.h>
#include <asm/processor.h>

#ifndef STR
#define __STR(x) #x
#define STR(x) __STR(x)
#endif

#ifdef __x86_64__
#include <asm/x86_64/asm_defns.h>
#else
#include <asm/x86_32/asm_defns.h>
#endif

#endif /* __X86_ASM_DEFNS_H__ */
