/* tarfs - A GNU tar filesystem for the Hurd.
   Copyright (C) 2002, Ludovic Courtès <ludo@chbouib.org>
 
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or * (at your option) any later version.
 
   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
 
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
   USA */

/*
 * General debugging output tools.
 */

#ifndef __DEBUG_H__
#define __DEBUG_H__


/* Format string for off_t objects.  */

#ifdef _FILE_OFFSET_BITS
# if _FILE_OFFSET_BITS == 64
#  define OFF_FMT  "%lli"
# else /* assume 32 bit */
#  define OFF_FMT  "%li"
# endif
#else /* assume 32 bit */
# define OFF_FMT  "%li"
#endif


#ifdef DEBUG

/* Sets the debugging output file.  */
extern void debug_set_file (const char *name);

extern void __debug_start (const char *function);
extern void __debug (const char *fmt, ...);
extern void __debug_end ();

#define debug(Args) \
  __debug_start (__FUNCTION__), __debug Args, __debug_end ();

#else

static inline void
debug_set_file (const char *name) { }

static inline void
__debug (const char *fmt, ...) { }

#define debug(Args)  __debug Args;

#endif /* DEBUG */

#endif
