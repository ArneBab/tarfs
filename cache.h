/* tarfs - A GNU tar filesystem for the Hurd.
   Copyright (C) 2002, Ludovic Courtès <ludo@type-z.org>
 
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
 * Nodes contents cache management.
 */

#ifndef __CACHE_H__
#define __CACHE_H__

#include <stdlib.h>
#include <error.h>
#include <hurd/netfs.h>
#include <hurd/store.h>

/* Size of a cache block.  */
#define CACHE_BLOCK_SIZE_LOG2  10
#define CACHE_BLOCK_SIZE  (1 << CACHE_BLOCK_SIZE_LOG2)

/* Cache data accessor */
#define CACHE_INFO(Node, Field) \
  (NODE_INFO(Node)->cache. Field)

/* Nodes contents cache */
struct cache
{
  /* Vector of cache blocks */
  char **blocks;

  /* Size of BLOCKS */
  size_t size;

  /* Lock of this cache */
  struct mutex lock;
};

/* Initializes the cache backend.  READ is the method that will be called
   when data needs to be read from a node.  */
extern void cache_init (error_t (* read) (struct node *node, off_t offset,
					  size_t howmuch,
					  size_t *actually_read, void *data));

/* Create a cache for node NODE.  */
extern error_t cache_create (struct node *node);

/* Free NODE's cache.  */
extern error_t cache_free (struct node *node);

/* Read at most AMOUNT bytes from NODE at OFFSET into BUF.
   Returns the amount of data actually read in LEN.  */
extern error_t cache_read (struct node *node, off_t offset,
			   size_t amount, void *buf, size_t *len);

/* Writes at most LEN bytes from NODE at OFFSET into BUF.
   Returns the amount of data actually written in AMOUNT.  */
extern error_t cache_write (struct node *node, off_t offset,
			    void *data, size_t len, size_t *amount);

/* Sets the size of NODE and reduce/grow its cache.  */
extern error_t cache_set_size (struct node *node, size_t size);

/* Cache AMOUNT bytes of NODE.  */
extern error_t cache_cache (struct node *node, size_t amount);

/* Returns non-zero if NODE is synchronized (ie. not cached).  */
extern int cache_synced (struct node *node);

#endif /* cache.h */
