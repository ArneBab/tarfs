/* Look up user and/or group names.
   Copyright (C) 1988, 1992 Free Software Foundation

   From GNU Tar.
   
   GNU Tar is free software; you can redistribute it and/or modify it
   under the terms of the GNU Library General Public License as published
   by the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GNU Tar is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Library General Public License for more details.

   Namespace: finduname, finduid, findgname, findgid,
              uid_to_uname, gid_to_gname.
 */

#ifndef __TAR_NAMES__
#define __TAR_NAMES__

#include "tar.h"

extern int  finduid (char *name);
extern void finduname (char *name, int uid);
extern int  findgid (char *name);
extern void findgname (char *name, int gid);

extern void uid_to_uname (uid_t uid, char uname[NAMSIZ]);
extern void gid_to_gname (gid_t gid, char gname[NAMSIZ]);

#endif
