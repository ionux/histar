/* Copyright (C) 1997 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#ifndef _JOS64_KERNEL_TERMIOS_H
#define _JOS64_KERNEL_TERMIOS_H 1

/* We need the definition of tcflag_t, cc_t, and speed_t.  */
#include <termios.h>

/* The following corresponds to the values from the Linux 2.1.20 kernel.  */

#define __JOS64_KERNEL_NCCS 19
struct __kernel_termios
{
    tcflag_t c_iflag;		/* input mode flags */
    tcflag_t c_oflag;		/* output mode flags */
    tcflag_t c_cflag;		/* control mode flags */
    tcflag_t c_lflag;		/* local mode flags */
    cc_t c_line;		/* line discipline */
    cc_t c_cc[__JOS64_KERNEL_NCCS];	/* control characters */
};

extern struct __kernel_termios __kernel_std_termios;


#endif /* kernel_termios.h */

