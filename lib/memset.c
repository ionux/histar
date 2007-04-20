/*	$OpenBSD: memset.c,v 1.5 2004/08/07 00:38:32 deraadt Exp $	*/
/*	$NetBSD: memset.c,v 1.6 1998/03/27 05:35:47 cgd Exp $	*/

/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Hibler and Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef JOS_KERNEL
#include <kern/lib.h>
#endif

#ifndef __LONG_MAX__
#error Need to know the size of a long
#endif

#define	wsize	sizeof(unsigned long)
#define	wmask	(wsize - 1)

void *
memset(void *dst0, int c0, size_t length)
{
	size_t t;
	unsigned long c;
	uint8_t *dst;

	dst = dst0;
	/*
	 * If not enough words, just fill bytes.  A length >= 2 words
	 * guarantees that at least one of them is `complete' after
	 * any necessary alignment.  For instance:
	 *
	 *	|-----------|-----------|-----------|
	 *	|00|01|02|03|04|05|06|07|08|09|0A|00|
	 *	          ^---------------------^
	 *		 dst		 dst+length-1
	 *
	 * but we use a minimum of 3 here since the overhead of the code
	 * to do word writes is substantial.
	 */ 
	if (length < 3 * wsize) {
		while (length != 0) {
			*dst++ = c0;
			--length;
		}
		return dst0;
	}

	if ((c = (uint8_t)c0) != 0) {	/* Fill the word. */
		c = (c << 8) | c;	/* long is 16 bits. */
#if __LONG_MAX__ > 0x7fff
		c = (c << 16) | c;	/* long is 32 bits. */
#endif
#if __LONG_MAX__ > 0x7fffffff
		c = (c << 32) | c;	/* long is 64 bits. */
#endif
	}

	/* Align destination by filling in bytes. */
	if ((t = (uintptr_t)dst & wmask) != 0) {
		t = wsize - t;
		length -= t;
		do {
			*dst++ = c0;
		} while (--t != 0);
	}

	/* Fill words.  Length was >= 2*words so we know t >= 1 here. */
	t = length / wsize;
	do {
		*(unsigned long *)dst = c;
		dst += wsize;
	} while (--t != 0);

	/* Mop up trailing bytes, if any. */
	t = length & wmask;
	if (t != 0)
		do {
			*dst++ = c0;
		} while (--t != 0);
	return dst0;
}
