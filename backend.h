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
 * A general filesystem backend.
 */

#ifndef __FS_BACKEND__
#define __FS_BACKEND__

#include <hurd/netfs.h>
#include <dirent.h>
#include <argp.h>
#include <assert.h>


/* Generic (fs independent) netnode structure.  */
struct netnode
{
  char *name;		/* node name */
  char *symlink;	/* link's target path (in the case of a symlink) */
  struct node *hardlink;/* hard link's target or zero */
  struct node *entries;	/* directory entries (when applies) */
  struct node *dir;	/* parent directory */

  void *info;		/* fs defined data (node related info) */
};

/* Substitute for '/' and chars lower than 32 in node names.
   If SUBST_LOWER isn't defined, then lower chars won't be filtered. */
#define SUBST_SLASH '|'
//#define SUBST_LOWER '.'


/* Each filesystem backend should define a struct fs_backend variable with
   the appropriate functions.  */
struct fs_backend
{
  /* Initialize filesystem (create root node *N, etc.) */
  error_t (* init)(struct node **n, struct iouser *user);

  /* Get filesystem's struct argp. */
  void (* get_argp)(struct argp *s);

  /* Get arguments (see netfs_append_args()). */
  error_t (*get_args)(char **argz, unsigned *argz_len);

  /* Set options (see netfs_set_options()). */
  error_t (*set_options)(char *argz, size_t argz_len);


  /* 
   * Directory scan functions (used in netfs_get_dirents ()).
   */

  /* Set current directory. */
  int (* set_curr_dir)(struct node *dir);

  /* Skip N entries in current directory, returns non-zero if
     no more entries are available.  */
  int (* skip_entries)(int n);
  
  /* Returns a newly-allocated entry in ENTRY. Returns non-zero when
     no more entries are available.  */
  int (* get_next_entry)(struct dirent **entry);

  /* Reading a node */
  error_t (* lookup_node)(struct node **np, struct node* dir, const char* name);
  error_t (* read_node)  (struct node *np, off_t offset,
			  size_t *len, void* data);


  /* Changing a node */
  error_t (* write_node) (struct node *np, off_t offset,
  			  size_t *len, void* data);

  /* Change NP's stats.  */
  error_t (* change_stat)(struct node *np, const io_statbuf_t *new_stat);

  /* Creates a node named NAME in DIR which is locked.  */
  error_t (* create_node) (struct node **new, struct node *dir,
  			   char *name, mode_t m);

  /* Unlinks NODE.  NODE can be a directory in which case it is empty.  */
  error_t (* unlink_node) (struct node *node);

  /* Tries to create a hard link named NAME in DIR to file NODE.  */
  error_t (* link_node) (struct node *dir, struct node *target,
  			 char *name, int excl); /* Same as netfs semantics */

  /* Makes NODE a symlink to TARGET.  */
  error_t (* symlink_node) (struct node *node, const char *target);

  /* Tries to turn NODE into a device of type TYPE (either S_IFBLK
     or S_IFCHR).  */
  error_t (* mkdev_node)   (struct node *node, mode_t type, dev_t indexes);

  /* Free all resources associated to NODE.  */
  void (* free_node) (struct node *node);

  /* Synchronize filesystem.  */
  error_t (* sync_fs) (int wait);

  /* Filesystem destructor */
  error_t (* go_away) ();
};

#endif
