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

/*
 * Look up user and/or group names.
 *
 * This file should be modified for non-unix systems to do something
 * reasonable.
 */

#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#define TAR_NAMES
#include "tar.h"
#include "names.h"

#include <stdio.h>
#include <pwd.h>
#include <grp.h>

#ifndef TUNMLEN 
#define TUNMLEN 256
#endif
#ifndef TGNMLEN
#define TGNMLEN 256
#endif

static int saveuid = -993;
static char saveuname[TUNMLEN];
static int my_uid = -993;

static int savegid = -993;
static char savegname[TGNMLEN];
static int my_gid = -993;

#define myuid	( my_uid < 0? (my_uid = getuid()): my_uid )
#define	mygid	( my_gid < 0? (my_gid = getgid()): my_gid )


/* Make sure you link with the proper libraries if you are running the
   Yellow Peril (thanks for the good laugh, Ian J.!), or, euh... NIS.
   This code should also be modified for non-UNIX systems to do something
   reasonable.  */

static char cached_uname[NAMSIZ] = "";
static char cached_gname[NAMSIZ] = "";

static uid_t cached_uid;	/* valid only if cached_uname is not empty */
static gid_t cached_gid;	/* valid only if cached_gname is not empty */

#if 0
/* These variables are valid only if nonempty.  */
static char cached_no_such_uname[NAMSIZ] = "";
static char cached_no_such_gname[NAMSIZ] = "";
#endif

/* These variables are valid only if nonzero.  It's not worth optimizing
   the case for weird systems where 0 is not a valid uid or gid.  */
static uid_t cached_no_such_uid = 0;
static gid_t cached_no_such_gid = 0;


/*
 * Look up a user or group name from a uid/gid, maintaining a cache.
 * FIXME, for now it's a one-entry cache.
 * FIXME2, the "-993" is to reduce the chance of a hit on the first lookup.
 *
 * This is ifdef'd because on Suns, it drags in about 38K of "yellow
 * pages" code, roughly doubling the program size.  Thanks guys.
 */
void finduname (char *uname, int uid)
{
    struct passwd *pw;
#ifndef HAVE_GETPWUID
    extern struct passwd *getpwuid ();
#endif

    if (uid != saveuid) {
	saveuid = uid;
	saveuname[0] = '\0';
	pw = getpwuid (uid);
	if (pw)
	    strncpy (saveuname, pw->pw_name, TUNMLEN);
    }
    strncpy (uname, saveuname, TUNMLEN);
}

int finduid (char *uname)
{
    struct passwd *pw;
    extern struct passwd *getpwnam ();
    
    if (uname[0] != saveuname[0]/* Quick test w/o proc call */
	||0 != strncmp (uname, saveuname, TUNMLEN)) {
	strncpy (saveuname, uname, TUNMLEN);
	pw = getpwnam (uname);
	if (pw) {
	    saveuid = pw->pw_uid;
	} else {
	    saveuid = myuid;
	}
    }
    return saveuid;
}


void findgname (char *gname, int gid)
{
    struct group *gr;
#ifndef HAVE_GETGRGID
    extern struct group *getgrgid ();
#endif

    if (gid != savegid) {
	savegid = gid;
	savegname[0] = '\0';
	(void) setgrent ();
	gr = getgrgid (gid);
	if (gr)
	    strncpy (savegname, gr->gr_name, TGNMLEN);
    }
    (void) strncpy (gname, savegname, TGNMLEN);
}


int findgid (char *gname)
{
    struct group *gr;
    extern struct group *getgrnam ();
    
    if (gname[0] != savegname[0]/* Quick test w/o proc call */
	||0 != strncmp (gname, savegname, TUNMLEN)) {
	strncpy (savegname, gname, TUNMLEN);
	gr = getgrnam (gname);
	if (gr) {
	    savegid = gr->gr_gid;
	} else {
	    savegid = mygid;
	}
    }
    return savegid;
}


void
uid_to_uname (uid_t uid, char uname[NAMSIZ])
{
  struct passwd *passwd;

  if (uid != 0 && uid == cached_no_such_uid)
    {
      *uname = '\0';
      return;
    }

  if (!cached_uname[0] || uid != cached_uid)
    {
      passwd = getpwuid (uid);
      if (passwd)
	{
	  cached_uid = uid;
	  strncpy (cached_uname, passwd->pw_name, NAMSIZ);
	}
      else
	{
	  cached_no_such_uid = uid;
	  *uname = '\0';
	  return;
	}
    }
  strncpy (uname, cached_uname, NAMSIZ);
}

void
gid_to_gname (gid_t gid, char gname[NAMSIZ])
{
  struct group *group;

  if (gid != 0 && gid == cached_no_such_gid)
    {
      *gname = '\0';
      return;
    }

  if (!cached_gname[0] || gid != cached_gid)
    {
      group = getgrgid (gid);
      if (group)
	{
	  cached_gid = gid;
	  strncpy (cached_gname, group->gr_name, NAMSIZ);
	}
      else
	{
	  cached_no_such_gid = gid;
	  *gname = '\0';
	  return;
	}
    }
  strncpy (gname, cached_gname, NAMSIZ);
}
