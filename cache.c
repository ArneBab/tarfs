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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

#include <hurd/netfs.h>

#include "tarfs.h"
#include "cache.h"
#include "debug.h"

/* Locking/unlocking a node's cache */
#define LOCK(Node)     mutex_lock (&CACHE_INFO ((Node), lock))
#define UNLOCK(Node)   mutex_unlock (&CACHE_INFO ((Node), lock));

/* Tar file callback (in tarfs.c).  */
static error_t (* read_file) (struct node *node,
			      off_t offset, size_t howmuch,
			      size_t *actually_read, void *data) = NULL;

/* BLOCK_NUMBER gives the number in which Offset can be found
   (equivalent to Offset/CACHE_BLOCK_SIZE).  */
#define BLOCK_NUMBER(Offset) \
  ((Offset) >> CACHE_BLOCK_SIZE_LOG2)

/* BLOCK_RELATIVE_OFFSET gives the relative offset inside a block
   (equivalent to AbsoluteOffset%CACHE_BLOCK_SIZE).  */
#define BLOCK_RELATIVE_OFFSET(AbsoluteOffset) \
  ((AbsoluteOffset) & (CACHE_BLOCK_SIZE - 1))

 
/* Initializes the cache backend.  READ is the method that will be called
   when data needs to be read from a node.  */
void
cache_init (error_t (* read) (struct node *node, off_t offset, size_t howmuch,
			      size_t *actually_read, void *data))
{
  read_file = read;
}

/* Create a cache for node NODE.  */
error_t
cache_create (struct node *node)
{
  size_t size, blocks;

  size   = node->nn_stat.st_size;
  blocks = size ? BLOCK_NUMBER (size - 1) + 1 : 1;

  CACHE_INFO (node, blocks) = calloc (blocks, sizeof (char *));
  if (!CACHE_INFO (node, blocks))
    return ENOMEM;

  CACHE_INFO (node, size) = blocks;
  debug (("Node %s: Initial block vector size: %u", node->nn->name, blocks));

  mutex_init (&CACHE_INFO (node, lock));

  return 0;
}

/* Free NODE's cache.  */
error_t
cache_free (struct node *node)
{
  size_t i, size;
  char **p;

  LOCK (node);
  p    = CACHE_INFO (node, blocks);
  size = CACHE_INFO (node, size);
  debug (("Node %s: Freeing blocks (size = %u)", node->nn->name, size));

  if (p)
  {
    for (i=0; i < CACHE_INFO (node, size); i++)
      if (p[i])
      {
	free (p[i]);
	p[i] = NULL;
      }

    /* Finish it.  */
    free (p);
    CACHE_INFO (node, blocks) = NULL;
    CACHE_INFO (node, size) = 0;
  }
  else
    assert (CACHE_INFO (node, size) == 0);

  UNLOCK (node);
  return 0;
}

/* Same as cache_synced () (assuming NODE's cache is locked).  */
static inline int
__cache_synced (struct node *node)
{
  int i, ret = 1;
  char **blocks;
  
  blocks = CACHE_INFO (node, blocks);

  for (i = 0; i < CACHE_INFO (node, size); i++)
    if (blocks[i])
    {
      ret = 0;
      break;
    }

  return ret;
}

/* Returns non-zero if NODE is synchronized (ie. not cached).  */
int
cache_synced (struct node *node)
{
  int ret;

  LOCK (node);
  ret = __cache_synced (node);
  UNLOCK (node);

  return ret;
}


/* A canonical way to allocate cache blocks (assumes that cache is locked
   and that BLOCKS is at least BLOCK+1 long).  */
static inline error_t
alloc_block (struct node *node, size_t block)
{        
  char *b;
  char **blocks = CACHE_INFO (node, blocks);

  assert (CACHE_INFO(node, size) > block);
  assert (!blocks[block]);

  /* Allocate a new block */
  //debug (("Node %s: Allocating block %u", node->nn->name, block));
  b = calloc (CACHE_BLOCK_SIZE, sizeof (char));
  if (!b)
    return ENOMEM;
  
  blocks[block] = b;

  return 0;
}

/* Fetches block number BLOCK of NODE.  This assumes that NODE's cache
   is already locked.  */
static inline error_t
fetch_block (struct node *node, int block)
{
  error_t err   = 0;
  char **blocks = CACHE_INFO(node, blocks);	/* cache blocks array */
  
  size_t read;
  size_t size = NODE_INFO (node)->tar->orig_size;
  size_t actually_read = 0;

  assert (read_file);

  /* Don't try to go beyond the boundaries.  */
  assert (block <= BLOCK_NUMBER (size - 1));

  /* Allocate a new block.  */
  assert (!blocks[block]);
  err = alloc_block (node, block);
  if (err)
    return err;

  /* If this is the last block, then we may have less to read.  */
  if (block == BLOCK_NUMBER (size - 1))
    read = size % CACHE_BLOCK_SIZE;
  else
    read = CACHE_BLOCK_SIZE;

  err = read_file (node, (block << CACHE_BLOCK_SIZE_LOG2),
		   read, &actually_read,
		   blocks[block]);

  if (err)
    return err;

  /* We should have read everything.  */
  assert (actually_read == read);

  return err;
}


/* Read at most AMOUNT bytes from NODE at OFFSET into BUF.
   Returns the amount of data actually read in LEN.  */
error_t
cache_read (struct node *node, off_t offset, size_t amount,
            void *buf, size_t *len)
{
  error_t err   = 0;
  void   *datap = buf; /* current pointer */
  off_t   start = NODE_INFO(node)->tar->offset;
  size_t  size  = node->nn_stat.st_size;
  char  **blocks;
  size_t  blocks_size;
  size_t  block  = BLOCK_NUMBER (offset);	/* 1st block to read.  */

  /* If NODE is a link then redirect the call.  */
  if (node->nn->hardlink)
    return cache_read (node->nn->hardlink, offset, amount, buf, len);

  /* Symlinks should be handled in tarfs_read_node () or so.  */
  assert (! node->nn->symlink);
  assert (read_file);

  /* Check file boundaries.  */
  if (offset >= size)
  {
    *len = 0;
    return 0;
  }

  /* Lock the node */
  LOCK (node);
  blocks = CACHE_INFO(node, blocks);
  blocks_size = CACHE_INFO(node, size);

  /* Adjust SIZE and LEN to the maximum that can be read.  */
  size -= offset;
  size = (size > amount) ? amount : size;
  *len = size;

  /* Set OFFSET to be the relative offset inside cache block num. BLOCK.  */
  offset = BLOCK_RELATIVE_OFFSET (offset);

  while (size > 0)
  {
    size_t read = (size > CACHE_BLOCK_SIZE)
                  ? (CACHE_BLOCK_SIZE - offset)
		  : (size);

    /* Read a block either from cache or directly.  */
    if ((block < blocks_size) && (blocks[block]))
      memcpy (datap, &blocks[block][offset], read);
    else
    {
      /* If NODE is available on disk, then fetch its contents.  */
      if (start != -1)
      {
	size_t actually_read;
	err = read_file (node, (block << CACHE_BLOCK_SIZE_LOG2) + offset,
			 read, &actually_read, datap);
        if (err)
	  break;

	/* We should have read everything.  */
	assert (actually_read == read);
      }
      else
        /* If NODE is not cached nor on disk, then zero the user's buffer.  */
        bzero (datap, read);
    }

    /* Go ahead with next block.  */
    block++;
    size  -= read;
    offset = 0;
    datap  = datap + read;
  }

  UNLOCK (node);

  return err;
}

/* Set the cache size (assuming NODE's cache is locked).  */
static inline error_t
__cache_set_size (struct node *node, size_t size)
{
  error_t err = 0;
  size_t  *blocks_size;
  char ***blocks;

  /* New size of BLOCKS */
  size_t newsize = size ? BLOCK_NUMBER (size - 1) + 1 : 0;

  blocks_size = &(CACHE_INFO (node, size));
  blocks      = &(CACHE_INFO (node, blocks));

  if (size > node->nn_stat.st_size)
  {
    /* Grow the cache.  */
    if (newsize > *blocks_size)
    {
      /* Enlarge the block vector */
      char **newblocks;

      newblocks = realloc (*blocks, newsize * sizeof (char *));
      if (newblocks)
      {
	/* Zero the new blocks without actually allocating them */
	bzero (&newblocks[*blocks_size],
	       (newsize - *blocks_size) * sizeof (char *));

	*blocks = newblocks;
	*blocks_size = newsize;

	debug (("Node %s: grown to %u blocks", node->nn->name, newsize));
      }
      else
        err = ENOMEM;
    }
  }
  else
  {
    int i;

    /* Free unused cache blocks */
    for (i = newsize; i < *blocks_size; i++)
      free ((*blocks)[i]);

    /* Reduce cache vector */
    *blocks = realloc (*blocks, newsize * sizeof (char *));
    *blocks_size = newsize;
  }

  if (!err)
    node->nn_stat.st_size = size;

  return err;
}

/* Sets the size of NODE and reduce/grow its cache.  */
error_t
cache_set_size (struct node *node, size_t size)
{
  error_t err;

  LOCK (node);
  err = __cache_set_size (node, size);
  UNLOCK (node);

  return err;
}

/* Writes at most LEN bytes from NODE at OFFSET into BUF.
   Returns the amount of data actually written in AMOUNT.  */
error_t
cache_write (struct node *node, off_t offset, void *data,
	     size_t len, size_t *amount)
{
  error_t err  = 0;
  size_t  size = len;
  void  *datap = data;			/* current pointer */
  size_t block = BLOCK_NUMBER (offset);	/* 1st block to read */
  size_t last_block;			/* Last block avail on disk */
  char **blocks;			/* cache blocks array */
  int  ondisk;

  /* Links should be handled by tarfs_write_node ()).  */
  assert (!node->nn->hardlink);
  assert (!node->nn->symlink);

  LOCK (node);

  {
    /* Check whether we need to create/grow NODE's cache */
    size_t newsize = offset + len;

    if (__cache_synced (node) || (newsize > node->nn_stat.st_size))
      err = __cache_set_size (node, newsize);
  }

  blocks = CACHE_INFO (node, blocks);
  ondisk = (NODE_INFO (node)->tar->offset >= 0);
  last_block = BLOCK_NUMBER (NODE_INFO (node)->tar->orig_size - 1);

  /* Set OFFSET to be the relative offset inside cache block num. BLOCK.  */
  offset = BLOCK_RELATIVE_OFFSET (offset);

  while ((!err) && (size > 0))
  {
    size_t write = (size + offset > CACHE_BLOCK_SIZE)
                   ? (CACHE_BLOCK_SIZE - offset)
		   : (size);

    /* Allocate and fetch this block if not here yet (copy-on-write).  */
    if (!blocks[block])
    {
      if (ondisk && (block <= last_block))
	/* Fetch this block */
	err = fetch_block (node, block);
      else
        /* Allocate a new block */
        err = alloc_block (node, block);

      if (err)
	break;
    }

    /* Copy the new data into cache.  */
    memcpy (&blocks[block][offset], datap, write);

    /* Go ahead with next block.  */
    block++;
    size  -= write;
    offset = 0;
    datap  = datap + write;
  }

  UNLOCK (node);

  *amount = len - size;
  assert (*amount <= len);

  return err;
}

/* Cache AMOUNT bytes of NODE.  */
error_t
cache_cache (struct node *node, size_t amount)
{
  error_t err   = 0;
  int block = BLOCK_NUMBER (amount - 1) + 1;
  char **blocks;
  int b;

  assert (amount <= node->nn_stat.st_size);

  LOCK (node);

  if (block >= CACHE_INFO (node, size))
    /* Allocate a large enough cache */
    err = __cache_set_size (node, amount);

  blocks = CACHE_INFO (node, blocks);

  for (b = 0; b < block; b++)
    if (!blocks[b])
    {
      err = fetch_block (node, b);
      if (err)
	break;
    }

  UNLOCK (node);

  return err;
}
