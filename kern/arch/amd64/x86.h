#ifndef JOS_INC_X86_H
#define JOS_INC_X86_H

#include <inc/types.h>

#define X86_INST_ATTR	static __inline __attribute__((always_inline))

X86_INST_ATTR void breakpoint(void);
X86_INST_ATTR uint8_t inb(int port);
X86_INST_ATTR void insb(int port, void *addr, int cnt);
X86_INST_ATTR uint16_t inw(int port);
X86_INST_ATTR void insw(int port, void *addr, int cnt);
X86_INST_ATTR uint32_t inl(int port);
X86_INST_ATTR void insl(int port, void *addr, int cnt);
X86_INST_ATTR void outb(int port, uint8_t data);
X86_INST_ATTR void outsb(int port, const void *addr, int cnt);
X86_INST_ATTR void outw(int port, uint16_t data);
X86_INST_ATTR void outsw(int port, const void *addr, int cnt);
X86_INST_ATTR void outsl(int port, const void *addr, int cnt);
X86_INST_ATTR void outl(int port, uint32_t data);
X86_INST_ATTR void invlpg(const void *addr);
X86_INST_ATTR void lidt(void *p);
X86_INST_ATTR void lldt(uint16_t sel);
X86_INST_ATTR void ltr(uint16_t sel);
X86_INST_ATTR void lcr0(uint32_t val);
X86_INST_ATTR uint32_t rcr0(void);
X86_INST_ATTR uint64_t rcr2(void);
X86_INST_ATTR void lcr3(uint64_t val);
X86_INST_ATTR uint64_t rcr3(void);
X86_INST_ATTR void lcr4(uint32_t val);
X86_INST_ATTR uint32_t rcr4(void);
X86_INST_ATTR void tlbflush(void);
X86_INST_ATTR uint32_t read_eflags(void);
X86_INST_ATTR void write_eflags(uint32_t eflags);
X86_INST_ATTR void halt(void);
X86_INST_ATTR void sti(void);
X86_INST_ATTR void cli(void);
X86_INST_ATTR uint64_t read_rbp(void);
X86_INST_ATTR uint64_t read_rsp(void);
X86_INST_ATTR void cpuid(uint32_t info, uint32_t *eaxp, uint32_t *ebxp,
			 uint32_t *ecxp, uint32_t *edxp);
X86_INST_ATTR uint64_t read_tsc(void);

X86_INST_ATTR void
breakpoint(void)
{
	__asm __volatile("int3");
}

X86_INST_ATTR uint8_t
inb(int port)
{
	uint8_t data;
	__asm __volatile("inb %w1,%0" : "=a" (data) : "d" (port));
	return data;
}

X86_INST_ATTR void
insb(int port, void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\tinsb"			:
			 "=D" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "memory", "cc");
}

X86_INST_ATTR uint16_t
inw(int port)
{
	uint16_t data;
	__asm __volatile("inw %w1,%0" : "=a" (data) : "d" (port));
	return data;
}

X86_INST_ATTR void
insw(int port, void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\tinsw"			:
			 "=D" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "memory", "cc");
}

X86_INST_ATTR uint32_t
inl(int port)
{
	uint32_t data;
	__asm __volatile("inl %w1,%0" : "=a" (data) : "d" (port));
	return data;
}

X86_INST_ATTR void
insl(int port, void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\tinsl"			:
			 "=D" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "memory", "cc");
}

X86_INST_ATTR void
outb(int port, uint8_t data)
{
	__asm __volatile("outb %0,%w1" : : "a" (data), "d" (port));
}

X86_INST_ATTR void
outsb(int port, const void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\toutsb"		:
			 "=S" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "cc");
}

X86_INST_ATTR void
outw(int port, uint16_t data)
{
	__asm __volatile("outw %0,%w1" : : "a" (data), "d" (port));
}

X86_INST_ATTR void
outsw(int port, const void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\toutsw"		:
			 "=S" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "cc");
}

X86_INST_ATTR void
outsl(int port, const void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\toutsl"		:
			 "=S" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "cc");
}

X86_INST_ATTR void
outl(int port, uint32_t data)
{
	__asm __volatile("outl %0,%w1" : : "a" (data), "d" (port));
}

X86_INST_ATTR void 
invlpg(const void *addr)
{ 
	__asm __volatile("invlpg (%0)" : : "r" (addr) : "memory");
}  

X86_INST_ATTR void
lidt(void *p)
{
	__asm __volatile("lidt (%0)" : : "r" (p));
}

X86_INST_ATTR void
lldt(uint16_t sel)
{
	__asm __volatile("lldt %0" : : "r" (sel));
}

X86_INST_ATTR void
ltr(uint16_t sel)
{
	__asm __volatile("ltr %0" : : "r" (sel));
}

X86_INST_ATTR void
lcr0(uint32_t val)
{
	__asm __volatile("movl %0,%%cr0" : : "r" (val));
}

X86_INST_ATTR uint32_t
rcr0(void)
{
	uint32_t val;
	__asm __volatile("movl %%cr0,%0" : "=r" (val));
	return val;
}

X86_INST_ATTR uint64_t
rcr2(void)
{
	uint64_t val;
	__asm __volatile("movq %%cr2,%0" : "=r" (val));
	return val;
}

X86_INST_ATTR void
lcr3(uint64_t val)
{
	__asm __volatile("movq %0,%%cr3" : : "r" (val));
}

X86_INST_ATTR uint64_t
rcr3(void)
{
	uint64_t val;
	__asm __volatile("movq %%cr3,%0" : "=r" (val));
	return val;
}

X86_INST_ATTR void
lcr4(uint32_t val)
{
	__asm __volatile("movl %0,%%cr4" : : "r" (val));
}

X86_INST_ATTR uint32_t
rcr4(void)
{
	uint32_t cr4;
	__asm __volatile("movl %%cr4,%0" : "=r" (cr4));
	return cr4;
}

X86_INST_ATTR void
tlbflush(void)
{
	uint32_t cr3;
	__asm __volatile("movl %%cr3,%0" : "=r" (cr3));
	__asm __volatile("movl %0,%%cr3" : : "r" (cr3));
}

X86_INST_ATTR uint32_t
read_eflags(void)
{
        uint32_t eflags;
        __asm __volatile("pushfl; popl %0" : "=r" (eflags));
        return eflags;
}

X86_INST_ATTR void
write_eflags(uint32_t eflags)
{
        __asm __volatile("pushl %0; popfl" : : "r" (eflags));
}

X86_INST_ATTR void 
halt(void) 
{
        __asm __volatile("sti; hlt" ::: "memory"); //memory -- DMA?
}

X86_INST_ATTR void 
sti(void) 
{
        __asm __volatile("sti");
}

X86_INST_ATTR void 
cli(void) 
{
        __asm __volatile("sti");
}

X86_INST_ATTR uint64_t
read_rbp(void)
{
        uint64_t rbp;
        __asm __volatile("movq %%rbp,%0" : "=r" (rbp));
        return rbp;
}

X86_INST_ATTR uint64_t
read_rsp(void)
{
        uint64_t rsp;
        __asm __volatile("movq %%rsp,%0" : "=r" (rsp));
        return rsp;
}

X86_INST_ATTR void
cpuid(uint32_t info, uint32_t *eaxp, uint32_t *ebxp,
      uint32_t *ecxp, uint32_t *edxp)
{
	uint32_t eax, ebx, ecx, edx;
	__asm volatile("cpuid" 
		: "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
		: "a" (info));
	if (eaxp)
		*eaxp = eax;
	if (ebxp)
		*ebxp = ebx;
	if (ecxp)
		*ecxp = ecx;
	if (edxp)
		*edxp = edx;
}

X86_INST_ATTR uint64_t
read_tsc(void)
{
	uint32_t a, d;
	__asm __volatile("rdtsc" : "=a" (a), "=d" (d));
	return ((uint64_t) a) | (((uint64_t) d) << 32);
}


// X86 byteswap operations

X86_INST_ATTR unsigned long
__byte_swap_long (unsigned long in) 
{
  unsigned long out;
  __asm volatile ("bswap %0" 
		: "=r" (out) 
		: "0" (in));
  return out;
}

/* XXX!!!!  Linux no longer uses an explicit xchgb instruction; it says GCC
   can figure out how to do that itself.  Need to verify Linux's compilation
   flags. */
X86_INST_ATTR uint16_t
__byte_swap_word(uint16_t in)
{
	uint16_t out;
	__asm ("xchgb %h1, %b1" 
	       : "=q" (out) 
	       : "0" (in));
	return out;
}

#define ntohl	__byte_swap_long
#define ntohs	__byte_swap_word
#define htonl	__byte_swap_long
#define htons	__byte_swap_word

#endif /* !JOS_INC_X86_H */
