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
 * General filesystem node management facilities.
 */

#ifndef __FS_H__
#define __FS_H__

#include <hurd.h>
#include <hurd/netfs.h>
#include <fcntl.h>
#include "backend.h"

/* Initialization.  */
extern int fs_init ();

/* The point of the following two accessors is to have something generic.
   For instance, the first entry could be either DIR->NN->ENTRIES or
   it could be the last element of the list starting at DIR->NN->ENTRIES.  */

/* Returns the first entry of directory DIR (i.e. the first entry which was
   added to DIR).  */
extern error_t fs_dir_first_entry (struct node *dir, struct node **first);

/* Returns the directory entry next to NODE (i.e. the entry which was added
   right after NODE).  This has to be consistent with _make_node ()/  */
#define fs_dir_next_entry(Node)   ((Node)->next)

/* Return DIR's last entry.  */
extern error_t fs_dir_last_entry (struct node *dir, struct node **last);

/* Returns either NULL or a pointer to a node if found.  */
extern struct node*
fs_find_node (struct node *dir, char *name);

/* Looks for a node located at PATH, starting at directory N.
   When looking for "/foo/bar":
    - if "/foo/bar" exists, a reference to it is returned in N and
      the remaining parameters are set to NULL;
    - if "/foo" doesn't exist, a reference to "/" is returned and RETRY_NAME
      is set to "foo/bar" and NOTFOUND is set to "foo"; N points to "/foo".
    - if "/foo" exists but "/foo/bar" doesn't, then RETRY is NULL but NOTFOUND
      is equal to "bar".  */
extern error_t
fs_find_node_path (struct node **n, char **retry_name, char **notfound,
		   const char *path);

/* Creates a new node in directory DIR, with name NAME (actually
   a copy of NAME) and mode M. If not NULL, *N points to the newly
   created node.
   Checks whether there already exists such a node.  */
extern error_t fs_make_node (struct node **n, struct node *dir,
			     char* name, mode_t m);

/* Tries to create a node located at PATH, starting at directory N.
   When creating "/foo/bar":
    - if "/foo" exists and is a directory, "/foo/bar" is created and
      a reference to it is returned; RETRY_NAME is NULL; N points to "/foo/bar".
    - if "/foo" doesn't exist, a reference to "/" is returned and RETRY_NAME
      is set to "foo/bar" and NOTFOUND is set to "foo"; N points to "/foo".  */
extern error_t
fs_make_node_path (struct node **n, char **retry_name, char **notfound,
		   const char *path, const mode_t m);

/* Used to add a sub-directory to DIR. If SUBDIRNAME already exists in DIR,
   returns the number of entries in it; otherwise creates it and returns
   zero. NEWDIR points to DIR/SUBDIRNAME.
   It also checks whether SUBDIRNAME already exists.
   SUBDIRNAME is *not* duplicated!  */
extern unsigned long fs_make_subdir (struct node **newdir,
				     struct node *dir, char *subdirname);

/* Makes NODE a symlink to TARGET, relatively to root directory ROOTDIR.  */
extern error_t fs_link_node (struct node *node, struct node *target);

/* Turn NODE into a symbolic link to TARGET.  */
extern error_t fs_link_node_path (struct node *node, const char *target);

/* Creates a new node NODE, in directory DIR, with name NAME and mode
   M, hard linked to TARGET.  */
extern error_t
fs_hard_link_node (struct node **node, struct node *dir, char* name,
		   const mode_t m, struct node *target);

/* Returns the path of a given node (relatively to the given root node).  */
extern char* fs_get_path_from_root (struct node *root, struct node *node);

/* Returns the relavive path to the given root node.  */
extern char* fs_get_path_to_root (struct node *root, struct node *node);

/* Gets the first common directory.  */
extern struct node* get_common_root (struct node *node1, struct node *node2);

/* Filters a node name, that is, remove '/' and chars lower than 32.
   Returns NAME is no change has been made, or a pointer to a newly
   malloced buffer otherwise.  */
extern char* filter_node_name (char* name);

/* Unlink NODE *without* freeing its resources.  */
extern error_t fs_unlink_node (struct node *node);

/* Frees all memory associated to NODE (which is assumed to be already
   unlinked) except its 'nn->info' field.  */
extern void fs_free_node (struct node *node);

#endif
