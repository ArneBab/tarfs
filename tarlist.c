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

/* Tar list management functions.  This is used as a model representing
   the contents of a tar file, i.e. the items in the order in which they
   appear (or should appear) in the tar file.  */

#include <hurd/netfs.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <error.h>

#include "tarfs.h"
#include "fs.h"
#include "debug.h"


/* Initialize LIST.  */
void
tar_list_init (struct tar_list *list)
{
  list->head = NULL;
  mutex_init (&list->lock);
}

/* Make a tar item containing the given information. NEW points to the
   newly created item.  */
error_t
tar_make_item (struct tar_item **new_item,
	       struct node *node, size_t orig_size, off_t offset)
{
  struct tar_item *new;

  new = calloc (1, sizeof (struct tar_item));
  if (! new)
    return ENOMEM;

  assert (node != NULL);
  new->orig_size = orig_size;
  new->offset = offset;
  new->node   = node;
  NODE_INFO(node)->tar = new;

  *new_item = new;

  return 0;
}

/* Insert tar item NEW right after PREV in LIST.  */
error_t
tar_insert_item (struct tar_list *list,
		 struct tar_item *prev, struct tar_item *new)
{
  struct tar_item **head, *next;

  assert (prev != new);

  mutex_lock (&list->lock);
  head = &list->head;

  if (! prev)
    if (! *head)
    {
      *head = new;
      next  = NULL;
    }
    else
    {
      prev = *head;
      next = (*head)->next;
      (*head)->next = new;
    }
  else
  {
    next = prev->next;
    prev->next = new;
  }

  new->prev = prev;
  new->next = next;
  if (next)
    next->prev = new;

  mutex_unlock (&list->lock);

  return 0;
}

/* Remove ITEM from LIST.  */
void
tar_unlink_item_safe (struct tar_list *list, struct tar_item *item)
{
  struct tar_item **head;

  /* The corresponding node should have been destroyed first.  */
  assert (item->node == NULL);

  head = &list->head;

  /* Make sure LIST is not already empty */
  assert (*head != NULL);

  if (! item->prev)
  {
    *head = item->next;
    if (*head)
      (*head)->prev = NULL;
  }
  else
  {
    item->prev->next = item->next;
    if (item->next)
      item->next->prev = item->prev;
  }

  /* Free ITEM.  */
  free (item);
}

void
tar_unlink_item (struct tar_list *list, struct tar_item *tar)
{
  mutex_lock (&list->lock);
  tar_unlink_item_safe (list, tar);
  mutex_unlock (&list->lock);
}


/* Attempt to find a place for TAR, an new yet unlinked tar item, into the
   tar list in an optimal way.  Returns in PREV_ITEM the item after which
   TAR should be inserted but don't actually insert it.  */
void
tar_put_item (struct tar_item **prev_tar, struct tar_item *tar)
{
  struct node *node = tar->node;
  struct node *dir, *last_entry;

  /* TAR has to be linked to the filesystem.  */
  assert (tar->node);

  dir = node->nn->dir;

  /* Try to insert the node inside the tar list in such a way that it
     appears:
     - after its parent directory;
     - after all the entries of its parent dir;
     - after the last entry of its parent dir's last entry.

     The 1st thing is needed to have a consistent tar archive, and the two
     latter requirements are needed for better performance at sync time
     (it avoids having to sync everything before NEWNODE).

     So that we have, for instance:
       0: file
       1: dir/
       2: dir/file1
       3: dir/file2
       4: NEWNODE.  */

  last_entry = dir->nn->entries;

  if (last_entry)
  {
    /* Get a reference to DIR's last entry.  */
    struct node *next;

    for ( ;
	 (next = last_entry->next) != NULL;
	 last_entry = next)
    {
      if (next == node)
      {
        /* If LAST_ENTRY is TAR's node...  */
        if (node->next)
          /* ... skip it */
          next = node->next;
        else
          /* ... or keep the next-to-last entry */
          break;
      }
    }

    if (last_entry == node)
      last_entry = NULL;

    /* Jump to the last node of LAST_ENTRY's deepest subdir */
    while (last_entry)
    {
      /* If it's a directory, get its last entry.  */
      if ( (S_ISDIR (last_entry->nn_stat.st_mode))
	   && (last_entry->nn->entries) )
      {
	for (last_entry = last_entry->nn->entries;
	     last_entry->next;
	     last_entry = last_entry->next);
      }
      else
        break;
    }
  }

  if ((last_entry) && (last_entry != node))
  {
    assert (NODE_INFO(last_entry)->tar);
    *prev_tar = NODE_INFO(last_entry)->tar;
  }
  else
  {
    if (dir == netfs_root_node)
      *prev_tar = NULL;
    else
      *prev_tar = NODE_INFO(dir)->tar;
  }

  return;
}
