/* tarfs - A GNU tar filesystem for the Hurd.
   Copyright (C) 2002, 2003  Ludovic Courtès <ludo@type-z.org>
 
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
 * Tarfs common definitions.
 */

#ifndef __TARFS_DEFS_H__
#define __TARFS_DEFS_H__

#include <hurd/netfs.h>
#include <hurd/store.h>
#include "backend.h"
#include "tar.h"
#include "cache.h"


/** Filesystem options. **/

struct tarfs_opts
{
  char* file_name;	/* archive file name. */
  int   create:1;	/* TRUE if we want to create a new file.  */
  int   readonly:1;	/* TRUE when filesystem is started readonly.  */
  int   volatil:1;	/* TRUE if we want the fs to be volatile.  */
  int   compress:3;	/* compression type (see flags below) */
  int   threaded:1;	/* tells whether archive should be parsed in
			   another thread to avoid startup timeout.  */
  int   interval;	/* Sync interval (in seconds) */
};

/* Compression types */
#define COMPRESS_NONE  0
#define COMPRESS_GZIP  1
#define COMPRESS_BZIP2 2



/** Tar lists (see tarlist.c) **/

/* Struct tar_item is used to create a list of tar items in their order of
   appearance in the original tar file. New files are inserted into this list
   in tarfs_create_node (). Finally, tarfs_sync_fs () traverses this list in
   order to sync files in the "right order".  */
struct tar_item
{
  /* File offset in the tar file or `-1' if this item is not part of the file */
  off_t offset;

  /* Original size in the tar file (tar header excluded) */
  size_t orig_size;

  /* Corresponding node (NULL if it's been unlinked) */
  struct node *node;
  
  /* Previous and next items in the tar file.  */
  struct tar_item *prev;
  struct tar_item *next;
};

/* Struct tar_list represents a list of tar items.  */
struct tar_list
{
  struct tar_item *head;
  struct mutex lock;
};

/* Unless stated otherwise, all the following functions taking a tar_list
   as an argument will first try to grab their lock.  */

/* Initialize LIST.  */
extern void tar_list_init (struct tar_list *list);

/* Make a tar item containing the given information. NEW points to the
   newly created item.  */
extern error_t tar_make_item (struct tar_item **new_item,
	                      struct node *node, size_t orig_size,
	                      off_t offset);

/* Insert tar item NEW right after PREV in LIST.  */
extern error_t tar_insert_item (struct tar_list *list,
				struct tar_item *prev,
				struct tar_item *new);

/* Remove ITEM from LIST.  */
extern void tar_unlink_item (struct tar_list *list, struct tar_item *item);

/* Same except that this one assumes that LIST is already locked.  */
extern void tar_unlink_item_safe (struct tar_list *list,
				     struct tar_item *item);

/* Attempt to find a place for TAR, an new yet unlinked tar item, into the
   tar list in an optimal way.  Returns in PREV_ITEM the item after which
   TAR should be inserted but don't actually insert it.  */
extern void tar_put_item (struct tar_item **prev_tar, struct tar_item *tar);

/* Accessor for a list's head.  */
#define tar_list_head(List)    (List)->head

/* An iterator.  */
#define tar_list_iterate(List, Item, Expr1, Expr2) \
  for ((Item) = (mutex_lock (&(List)->lock), (List)->head); \
       (Expr1); (Expr2))

/* These two macros can be used to implement critical things (e.g. traversing
   the whole list).  */
#define tar_list_lock(List)    mutex_lock (&(List)->lock);
#define tar_list_unlock(List)  mutex_unlock (&(List)->lock);


/** Node information. **/

/* Each node has such a structure.  */
struct tarfs_info
{
  struct tar_item *tar;
  struct cache    cache;

  int stat_changed;	/* TRUE when stat changed.  */
};

/* The following macros take struct node *_N as an argument. */
#define NEW_NODE_INFO(Node) \
  (Node)->nn->info = calloc (1, sizeof (struct tarfs_info));

#define NODE_INFO(Node) \
  ((struct tarfs_info*)((Node)->nn->info))


#endif
