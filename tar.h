/* Declarations for the tarfs.
   Copyright (C) 1995 The Free Software Foundation
   
   Written by: 1995 Jakub Jelinek

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#ifndef __TAR_H__
#define __TAR_H__

#if 0
#include "testpad.h"
#else
#define NEEDPAD
#endif

#include <sys/types.h>

/* major() and minor() macros (among other things) defined here for hpux */
#ifdef hpux
#include <sys/mknod.h>
#endif

/*
 * Header block on tape.
 *
 * I'm going to use traditional DP naming conventions here.
 * A "block" is a big chunk of stuff that we do I/O on.
 * A "record" is a piece of info that we care about.
 * Typically many "record"s fit into a "block".
 */
#define	RECORDSIZE	512
#define	NAMSIZ		100
#define	TUNMLEN		32
#define	TGNMLEN		32
#define SPARSE_EXT_HDR  21
#define SPARSE_IN_HDR	4

struct sparse {
    char offset[12];
    char numbytes[12];
};

struct sp_array {
    int offset;
    int numbytes;
};

union record {
    char charptr[RECORDSIZE];
    struct header {
	char arch_name[NAMSIZ];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char chksum[8];
	char linkflag;
	char arch_linkname[NAMSIZ];
	char magic[8];
	char uname[TUNMLEN];
	char gname[TGNMLEN];
	char devmajor[8];
	char devminor[8];
	/* these following fields were added by JF for gnu */
	/* and are NOT standard */
	char atime[12];
	char ctime[12];
	char offset[12];
	char longnames[4];
#ifdef NEEDPAD
	char pad;
#endif
	struct sparse sp[SPARSE_IN_HDR];
	char isextended;
	char realsize[12];	/* true size of the sparse file */
	/* char	ending_blanks[12];*//* number of nulls at the
	   end of the file, if any */
    } header;
    struct extended_header {
	struct sparse sp[21];
	char isextended;
    } ext_hdr;
};

/* The checksum field is filled with this while the checksum is computed. */
#define	CHKBLANKS	"        "	/* 8 blanks, no null */

/* The magic field is filled with this if uname and gname are valid. */
#define	TMAGIC		"ustar  "	/* 7 chars and a null */
#define TMAGLEN  6
#define TVERSION "00"		/* 00 and no null */
#define TVERSLEN 2

/* The linkflag defines the type of file */
#define	LF_OLDNORMAL	'\0'	/* Normal disk file, Unix compat */
#define	LF_NORMAL	'0'	/* Normal disk file */
#define	LF_LINK		'1'	/* Link to previously dumped file */
#define	LF_SYMLINK	'2'	/* Symbolic link */
#define	LF_CHR		'3'	/* Character special file */
#define	LF_BLK		'4'	/* Block special file */
#define	LF_DIR		'5'	/* Directory */
#define	LF_FIFO		'6'	/* FIFO special file */
#define	LF_CONTIG	'7'	/* Contiguous file */
/* Further link types may be defined later. */

/* Note that the standards committee allows only capital A through
   capital Z for user-defined expansion.  This means that defining something
   as, say '8' is a *bad* idea. */
#define LF_DUMPDIR	'D'	/* This is a dir entry that contains
					   the names of files that were in
					   the dir at the time the dump
					   was made */
#define LF_LONGLINK	'K'	/* Identifies the NEXT file on the tape
					   as having a long linkname */
#define LF_LONGNAME	'L'	/* Identifies the NEXT file on the tape
					   as having a long name. */
#define LF_MULTIVOL	'M'	/* This is the continuation
					   of a file that began on another
					   volume */
#define LF_NAMES	'N'	/* For storing filenames that didn't
					   fit in 100 characters */
#define LF_SPARSE	'S'	/* This is for sparse files */
#define LF_VOLHDR	'V'	/* This file is a tape/volume header */
/* Ignore it on extraction */

#define LF_TRANS        'T'	/* GNU/Hurd passive translator */

/*
 * Exit codes from the "tar" program
 */
#define	EX_SUCCESS	0	/* success! */
#define	EX_ARGSBAD	1	/* invalid args */
#define	EX_BADFILE	2	/* invalid filename */
#define	EX_BADARCH	3	/* bad archive */
#define	EX_SYSTEM	4	/* system gave unexpected error */
#define EX_BADVOL	5	/* Special error code means
				   Tape volume doesn't match the one
				   specified on the command line */

/*
 * We default to Unix Standard format rather than 4.2BSD tar format.
 * The code can actually produce all three:
 *	f_standard	ANSI standard
 *	f_oldarch	V7
 *	neither		4.2BSD
 * but we don't bother, since 4.2BSD can read ANSI standard format anyway.
 * The only advantage to the "neither" option is that we can cmp our
 * output to the output of 4.2BSD tar, for debugging.
 */
#define		f_standard		(!f_oldarch)


/* The following was added by Ludovic for the GNU Hurd's tarfs.  */
#include <hurd/store.h>
#include <hurd.h>
#include "tarfs.h"

typedef union record tar_record_t;

extern int  tar_open_archive (struct store *tar_file);
extern void tar_header2stat (io_statbuf_t *st, tar_record_t *header);

/* Create a tar header based on ST and NAME where NAME is a path.
   If NAME is a hard link (resp. symlink), HARDLINK (resp.
   SYMLINK) is the path of NAME's target.
   Also see GNU tar's create.c:start_header() (Ludovic).  */
void tar_make_header (tar_record_t *header, io_statbuf_t *st, char *name,
		      char *symlink, char *hardlink);


#endif
