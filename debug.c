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

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <cthreads.h>


static struct mutex debug_lock;
static char *debug_function = NULL;
static FILE *debug_file = NULL;

/* Sets the debugging output file.  */
void
debug_set_file (const char *name)
{
  if (!strcmp (name, "-"))
    debug_file = stderr;
  else
  {
    debug_file = fopen (name, "w+");
    if (!debug_file)
      error (0, errno, name);
  }
}


void
__debug_start (const char *function)
{
  if (!debug_file)
    return;

  mutex_lock (&debug_lock);
  debug_function = strdup (function);
}

void
__debug (const char *fmt, ...)
{
  va_list ap;

  if (!debug_file)
    return;

  fprintf (debug_file, "%s: ", debug_function);

  va_start (ap, fmt);
  vfprintf (debug_file, fmt, ap);
  va_end (ap);

  fprintf (debug_file, "\n");
}

void
__debug_end ()
{
  if (!debug_file)
    return;

  free (debug_function);
  debug_function = NULL;
  fflush (debug_file);
  mutex_unlock (&debug_lock);
}
