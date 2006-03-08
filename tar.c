/* GNU tar files parsing.
   Copyright (C) 1995 The Free Software Foundation
   
   Written by: 1995 Jakub Jelinek
   Rewritten by: 1998 Pavel Machek
   Modified by: 2002 Ludovic Courtes (for the Hurd tarfs)

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License
   as published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <time.h>
#include <string.h>
#include <hurd/netfs.h>
#include <hurd/store.h>

#include "tar.h"
#include "names.h"

#include "debug.h"

/* A hook which is called each time a header has been parsed. */
int (*tar_header_hook) (tar_record_t *, off_t) = NULL;


#define	isodigit(c)	( ((c) >= '0') && ((c) <= '7') )

#ifndef isspace
# define isspace(c)      ( (c) == ' ' )
#endif

/*
 * Quick and dirty octal conversion.
 *
 * Result is -1 if the field is invalid (all blank, or nonoctal).
 */
static long
from_oct (int digs, char *where)
{
  register long value;

  while (isspace (*where))
    {				/* Skip spaces */
      where++;
      if (--digs <= 0)
	return -1;		/* All blank field */
    }
  value = 0;
  while (digs > 0 && isodigit (*where))
    {				/* Scan till nonoctal */
      value = (value << 3) | (*where++ - '0');
      --digs;
    }

  if (digs > 0 && *where && !isspace (*where))
    return -1;			/* Ended on non-space/nul */

  return value;
}

/* As we open one archive at a time, it is safe to have this static */
static store_offset_t current_tar_position = 0;

static tar_record_t rec_buf;


static tar_record_t *
get_next_record (struct store *tar_file)
{
  error_t err;
  size_t n;
  void *buf;

  debug (("Reading at offset %lli", current_tar_position));
  buf = rec_buf.charptr;
  err = store_read (tar_file, current_tar_position, RECORDSIZE, &buf, &n);

  if (err)
    error (1, err, "Read error (offset=%lli)", current_tar_position);
  assert (n <= RECORDSIZE);

  if (buf != rec_buf.charptr)
    {
      memcpy (rec_buf.charptr, buf, n);
      munmap (buf, n);
    }

  if (n != RECORDSIZE)
    return NULL;		/* An error has occurred */

  current_tar_position += n;

  return &rec_buf;
}

static void
skip_n_records (struct store *tar_file, int n)
{
  current_tar_position += n * RECORDSIZE;
}

void
tar_header2stat (io_statbuf_t *st, tar_record_t *header)
{
  st->st_mode = from_oct (8, header->header.mode);

  /* Adjust st->st_mode because there are tar-files with
   * linkflag==LF_SYMLINK and S_ISLNK(mod)==0. I don't 
   * know about the other modes but I think I cause no new
   * problem when I adjust them, too. -- Norbert.
   */
  if (header->header.linkflag == LF_DIR)
    {
      st->st_mode |= S_IFDIR;
    }
  else if (header->header.linkflag == LF_SYMLINK)
    {
      st->st_mode |= S_IFLNK;
    }
  else if (header->header.linkflag == LF_CHR)
    {
      st->st_mode |= S_IFCHR;
    }
  else if (header->header.linkflag == LF_BLK)
    {
      st->st_mode |= S_IFBLK;
    }
  else if (header->header.linkflag == LF_FIFO)
    {
      st->st_mode |= S_IFIFO;
    }
  else
    st->st_mode |= S_IFREG;

  st->st_rdev = 0;
  if (!strcmp (header->header.magic, TMAGIC))
    {
      st->st_uid = *header->header.uname ? finduid (header->header.uname) :
	from_oct (8, header->header.uid);
      st->st_gid = *header->header.gname ? findgid (header->header.gname) :
	from_oct (8, header->header.gid);
      switch (header->header.linkflag)
	{
	case LF_BLK:
	case LF_CHR:
	  st->st_rdev = (from_oct (8, header->header.devmajor) << 8) |
	    from_oct (8, header->header.devminor);
	}
    }
  else
    {				/* Old Unix tar */
      st->st_uid = from_oct (8, header->header.uid);
      st->st_gid = from_oct (8, header->header.gid);
    }
  //st->st_size = hstat.st_size;
  st->st_size  = from_oct (1 + 12, header->header.size);
  st->st_mtime = from_oct (1 + 12, header->header.mtime);
  st->st_atime = from_oct (1 + 12, header->header.atime);
  st->st_ctime = from_oct (1 + 12, header->header.ctime);
}


typedef enum
{
  STATUS_BADCHECKSUM,
  STATUS_SUCCESS,
  STATUS_EOFMARK,
  STATUS_EOF,
}
ReadStatus;
/*
 * Return 1 for success, 0 if the checksum is bad, EOF on eof,
 * 2 for a record full of zeros (EOF marker).
 *
 */
static ReadStatus
read_header (struct store *tar_file)
{
  register int i;
  register long sum, signed_sum, recsum;
  register char *p;
  register tar_record_t *header;
  char *data;
  int size, written;
  static char *next_lonname = NULL, *next_lonlink = NULL;
  char *current_file_name, *current_link_name;
  struct stat hstat;		/* Stat struct corresponding */
  char arch_name[NAMSIZ + 1];
  char arch_linkname[NAMSIZ + 1];

  memcpy (arch_name, header->header.arch_name, NAMSIZ);
  arch_name [NAMSIZ] = '\0';
  memcpy (arch_linkname, header->header.arch_linkname, NAMSIZ);
  arch_linkname [NAMSIZ] = '\0';


recurse:

  header = get_next_record (tar_file);
  if (NULL == header)
    return STATUS_EOF;

  recsum = from_oct (8, header->header.chksum);

  sum = 0;
  signed_sum = 0;
  p = header->charptr;
  for (i = sizeof (*header); --i >= 0;)
    {
      /*
       * We can't use unsigned char here because of old compilers,
       * e.g. V7.
       */
      signed_sum += *p;
      sum += 0xFF & *p++;
    }

  /* Adjust checksum to count the "chksum" field as blanks. */
  for (i = sizeof (header->header.chksum); --i >= 0;)
    {
      sum -= 0xFF & header->header.chksum[i];
      signed_sum -= (char) header->header.chksum[i];
    }
  sum += ' ' * sizeof header->header.chksum;
  signed_sum += ' ' * sizeof header->header.chksum;

  /*
   * This is a zeroed record...whole record is 0's except
   * for the 8 blanks we faked for the checksum field.
   */
  if (sum == 8 * ' ')
    return STATUS_EOFMARK;

  if (sum != recsum && signed_sum != recsum)
    return STATUS_BADCHECKSUM;

  /*
   * linkflag on BSDI tar (pax) always '\000'
   */

  if (header->header.linkflag == '\000' &&
      strlen (arch_name) &&
      arch_name[strlen (arch_name) - 1] == '/')
    header->header.linkflag = LF_DIR;

  /*
   * Good record.  Decode file size and return.
   */
  if (header->header.linkflag == LF_LINK || header->header.linkflag == LF_DIR)
    hstat.st_size = 0;		/* Links 0 size on tape */
  else
    hstat.st_size = from_oct (1 + 12, header->header.size);

  if (header->header.linkflag == LF_LONGNAME
      || header->header.linkflag == LF_LONGLINK)
    {
      for (size = hstat.st_size; size > 0; size -= written)
	{
	  data = get_next_record (tar_file)->charptr;
	  if (data == NULL)
	    {
	      error (0, 0, "Unexpected EOF on archive file");
	      return STATUS_BADCHECKSUM;
	    }
	  written = RECORDSIZE;
	  if (written > size)
	    written = size;
	}
      goto recurse;
    }
  else
    {
      long data_position;
      char *p, *q;
      int len;
      int isdir = 0;

      current_file_name = (next_lonname
			   ? next_lonname
			   : strdup (arch_name));
      len = strlen (current_file_name);
      if (current_file_name[len - 1] == '/')
	{
	  current_file_name[len - 1] = 0;
	  isdir = 1;
	}


      current_link_name = (next_lonlink
			   ? next_lonlink
			   : strdup (arch_linkname));
      len = strlen (current_link_name);
      if (len && current_link_name[len - 1] == '/')
	current_link_name[len - 1] = 0;

      next_lonlink = next_lonname = NULL;

      data_position = current_tar_position;

      p = strrchr (current_file_name, '/');
      if (p == NULL)
	{
	  p = current_file_name;
	  q = current_file_name + strlen (current_file_name);	/* "" */
	}
      else
	{
	  *(p++) = 0;
	  q = current_file_name;
	}

      if (tar_header_hook)
	tar_header_hook (header, current_tar_position);

      free (current_file_name);

/*    done: */
      if (header->header.isextended)
	{
	  while (get_next_record (tar_file)->ext_hdr.isextended);
	}
      skip_n_records (tar_file, (hstat.st_size + RECORDSIZE - 1) / RECORDSIZE);
      return STATUS_SUCCESS;
    }
}

/*
 * Main loop for reading an archive.
 * Returns 0 on success, -1 on error.
 */
int
tar_open_archive (struct store *tar_file)
{
  ReadStatus status = STATUS_EOFMARK;	/* Initial status at start of archive */
  ReadStatus prev_status = STATUS_SUCCESS;

  current_tar_position = 0;

  for (;;)
    {
      prev_status = status;
      status = read_header (tar_file);


      switch (status)
	{

	case STATUS_SUCCESS:
	  continue;

	  /*
	   * Invalid header:
	   *
	   * If the previous header was good, tell them
	   * that we are skipping bad ones.
	   */
	case STATUS_BADCHECKSUM:
	  switch (prev_status)
	    {

	      /* Error on first record */
	    case STATUS_EOFMARK:
	      return -1;
	      /* FALL THRU */

	      /* Error after header rec */
	    case STATUS_SUCCESS:
	      prev_status = status;
	      { 
	        /* FIXME: Bad hack */
	        size_t size;
	        tar_record_t *hdr;
	        current_tar_position -= RECORDSIZE;
	        error (0, 0, "Skipping to next header (offset=%lli)",
	               current_tar_position);
	        hdr = get_next_record (tar_file);
	        size = from_oct (8, hdr->header.size);
	        size = size % RECORDSIZE
	               ? size / RECORDSIZE
	               : (size / RECORDSIZE) + RECORDSIZE;
	        current_tar_position += size;
	      }
	        
	      /* Error after error */

	    case STATUS_BADCHECKSUM:
	      error (1, 0, "Bad checksum (offset=%lli)", current_tar_position);
	      return -1;

	    case STATUS_EOF:
	      return 0;
	    }

	  /* Record of zeroes */
	case STATUS_EOFMARK:
	  status = prev_status;	/* If error after 0's */
	  /* FALL THRU */

	case STATUS_EOF:	/* End of archive */
	  break;
	}
      break;
    };
  return 0;
}


/* Create a tar header based on ST and NAME where NAME is a path.
   If NAME is a hard link (resp. symlink), HARDLINK (resp.
   SYMLINK) is the path of NAME's target.
   Also see GNU tar's create.c:start_header() (Ludovic).  */
void
tar_make_header (tar_record_t *header, io_statbuf_t *st, char *name,
    		 char *symlink, char *hardlink)
{
  int i;
  long sum = 0;
  char *p;

  /* NAME must be at most NAMSIZ long.  */
  assert (strlen (name) <= NAMSIZ);

  bzero (header, sizeof (* header));
  strcpy (header->header.arch_name, name);

  /* If it's a dir, add a trailing '/' */
  if (S_ISDIR (st->st_mode))
  {
    size_t s = strlen (name);
    if (s + 1 <= NAMSIZ + 1)
    {
      header->header.arch_name[s] = '/';
      header->header.arch_name[s+1] = '\0';
    }
  }

#define TO_OCT(what, where) \
  sprintf (where, "%07o", what);
#define LONG_TO_OCT(what, where) \
  sprintf (where, "%011llo", what);
#define TIME_TO_OCT(what, where) \
  sprintf (where, "%011lo", what);

  TO_OCT (st->st_mode, header->header.mode);
  TO_OCT (st->st_uid, header->header.uid);
  TO_OCT (st->st_gid, header->header.gid);
  LONG_TO_OCT (st->st_size, header->header.size);
  TIME_TO_OCT (st->st_mtime, header->header.mtime);

  /* Set the correct file type.  */
  if (S_ISREG (st->st_mode))
    header->header.linkflag = LF_NORMAL;
  else if (S_ISDIR (st->st_mode))
    header->header.linkflag = LF_DIR;
  else if (S_ISLNK (st->st_mode))
  {
    assert (symlink);
    assert (strlen (symlink) <= NAMSIZ);
    header->header.linkflag = LF_SYMLINK;
    memcpy (header->header.arch_linkname, symlink, strlen (symlink));
  }
  else if (S_ISCHR (st->st_mode))
    header->header.linkflag = LF_CHR;
  else if (S_ISBLK (st->st_mode))
    header->header.linkflag = LF_BLK;
  else if (S_ISFIFO (st->st_mode))
    header->header.linkflag = LF_FIFO;
  else if (hardlink)
  {
    assert (strlen (hardlink) <= NAMSIZ);
    header->header.linkflag = LF_LINK;
    memcpy (header->header.arch_linkname, hardlink, strlen (hardlink));
  }
  else
    header->header.linkflag = LF_NORMAL;

  strncpy (header->header.magic, TMAGIC, TMAGLEN);

  uid_to_uname (st->st_uid, header->header.uname);
  gid_to_gname (st->st_gid, header->header.gname);

  /* Compute a checksum for this header.  */
  strncpy (header->header.chksum, CHKBLANKS, sizeof (header->header.chksum));
  p = header->charptr;
  for (sum = 0, i = sizeof (*header); --i >= 0;)
      sum += 0xFF & *p++;

  sprintf (header->header.chksum, "%06lo", sum);
  header->header.chksum[6] = '\0';
}
