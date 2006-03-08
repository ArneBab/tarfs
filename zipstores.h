/* Gzip/Bzip2 store backends.

   Copyright (C) 1995,96,97,99,2000,01, 02 Free Software Foundation, Inc.
   Written by Ludovic Courtes <ludo@chbouib.org>
   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111, USA. */

#ifndef __ZIPSTORES_H__
#define __ZIPSTORES_H__

/* Open an existing store.  */
extern error_t store_gzip_open (const char *name,
				int flags, struct store **store);

extern error_t store_bzip2_open (const char *name,
				 int flags, struct store **store);

extern const struct store_class store_gzip_class;
extern const struct store_class store_bzip2_class;

#endif
