/* tarfs - A GNU tar filesystem for the Hurd.
   Copyright (C) 2002, Ludovic Courtès <ludo@chbouib.org>
 
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

#include <hurd.h>
#include <hurd/netfs.h>
#include <stdio.h>
#include <unistd.h>
#include <maptime.h>
#include <fcntl.h>
#include "backend.h"
#include "fs.h" /* to make sure that the header is up-to-date */
#include "debug.h"


/* General info */
static pid_t pid;
static uid_t uid;
static gid_t gid;
static volatile struct mapped_time_value *curr_time;

/* Initialization.  */
int
fs_init ()
{
  error_t err;

  /* General stuff. */
  pid = getpid ();
  uid = getuid ();
  gid = getgid ();
  err = maptime_map (0, 0, &curr_time);
 
  return err;
}

/* Returns the first entry of directory DIR (i.e. the first entry which was
   added to DIR).  */
error_t
fs_dir_first_entry (struct node *dir, struct node **first)
{
  if ((!dir->nn->entries) ||
      (!S_ISDIR (dir->nn_stat.st_mode)))
    return ENOTDIR;

  /* This has to be consistent with _make_node () */
  *first = dir->nn->entries;

  if (!*first)
    return ENOENT;

  return 0;
}

/* Return DIR's last entry.  */
error_t
fs_dir_last_entry (struct node *dir, struct node **last)
{
  struct node *node = dir->nn->entries;

  if ((!dir->nn->entries) ||
      (!S_ISDIR (dir->nn_stat.st_mode)))
    return ENOTDIR;

  if (!node)
  {
    *last = NULL;
    return ENOENT;
  }

  for ( ; node->next; node = node->next);

  *last = node;

  return 0;
}


/* Filters a node name, that is, remove '/' and chars lower than 32.
   Returns NAME is no change has been made, or a pointer to a newly
   malloced buffer otherwise.  */
char*
filter_node_name (char* name)
{
  char* newname;
  int modified = 0;
  char *s, *ns;
  
  if (!name)
    return NULL;
  if (! (newname = calloc (strlen (name) + 1, sizeof (char))))
    return name;

  for (s = name, ns = newname;
       *s != '\0';
       s++, ns++)
  {
    if (*s == '/')
      *ns = SUBST_SLASH, modified = 1;
#ifdef SUBST_LOWER
    else if (*s < 32)
      *ns = SUBST_LOWER, modified = 1;
#endif
    else
      *ns = *s;
  }
  *ns = *s;

  if (!modified)
  {
    free (newname);
    newname = name;
  }

  return newname;
}

/* Returns either NULL or a pointer to a node if found.  */
static inline struct node*
_find_node (struct node *dir, char *name)
{
  struct node *node = NULL;

  if (name)
  {
    /* Looking for '.' or '..'? */
    if (name[0] == '.')
    {
      switch (name[1])
      {
        case '\0':
          node = dir;
          break;
        case '.':
          if (name[2] == '\0')
          {
            node = dir->nn->dir;
            break;
          }
      }
    }

    if (!node)
    {
      /* Look for a "regular" node */
      for (node = dir->nn->entries;
	   node != NULL;
	   node = node->next)
      {
	if (node->nn->name)
	  if (! strcmp (node->nn->name, name))
	    break;
      }
    }
  }

  return node;
}

struct node *
fs_find_node (struct node *dir, char *name)
{
  return _find_node (dir, name);
}

/* Inserts a new node in directory DIR, with name NAME and mode M. If not NULL,
   *N points to the newly created node.
   NAME is *not* duplicated!  */
static inline error_t
_make_node (struct node **n, struct node *dir, char* name, mode_t m)
{
  static ino_t id = 1;
  io_statbuf_t   st;
  struct netnode *nn;
  struct node*   newnode = NULL;

  /* Alloctes a new netnode */
  nn = (struct netnode*) calloc (1, sizeof (struct netnode));
  if (!nn)
    return ENOMEM;
  newnode = netfs_make_node (nn);
  if (!newnode)
    return ENOMEM;

  /* General stat */
  st.st_fstype  = FSTYPE_TAR;
  st.st_fsid    = pid;
  st.st_dev     = st.st_rdev = pid;	/* unique device id */
  st.st_uid     = st.st_author = uid;
  st.st_gid     = gid;
  st.st_mode    = m;
  st.st_ino     = id++;	/* unique inode number for each fs node */
  st.st_nlink   = 1;	/* number of subdir plus two, one otherwise. */
  st.st_size    = 0;
  st.st_blksize = 1024;	/* optimal block size for reading */
  st.st_blocks  = 1;	/* XXX */
  st.st_gen     = 0;

  if (S_ISDIR (m))
    /* Set st_nlink to the number of subdirs plus 2 */
    st.st_nlink = 2;
  
  newnode->nn->name = filter_node_name (name);
  newnode->nn->entries = NULL;	/* ptr to the first entry of this node */
  newnode->nn_stat = st;
  newnode->nn_translated = m;

  newnode->next = NULL;
  newnode->prevp = NULL;

  if (dir)
  {
    struct node *p;

    /* Add a reference to DIR */
    netfs_nref (dir);

    /* Insert the new node *at the end* of the linked list of DIR entries. */
    if (dir->nn->entries)
    {
      for (p = dir->nn->entries;
	   p->next;
	   p = p->next);
      newnode->prevp = &p->next;
      p->next = newnode;
    }
    else
    {
      newnode->prevp = &dir->nn->entries;
      dir->nn->entries = newnode;
    }

#if 0
    /* Insert the new node *at the beginning* of the linked list
       of DIR entries. */
    newnode->next  = dir->nn->entries;
    newnode->prevp = &dir->nn->entries;
    dir->nn->entries = newnode;
    if (newnode->next)
      newnode->next->prevp = &newnode->next;
#endif

    newnode->nn->dir = dir;

    /* Make sure that DIR is a directory. */
    dir->nn_stat.st_mode |= S_IFDIR;

    if (S_ISDIR (m))
      /* Update DIR's hardlinks count */
      dir->nn_stat.st_nlink++;
  }

  fshelp_touch (&newnode->nn_stat,
		TOUCH_ATIME | TOUCH_CTIME | TOUCH_MTIME,
		curr_time);

  *n = newnode;

  return 0;
}

/* Creates a new node in directory DIR, with name NAME (actually
   a copy of NAME) and mode M. If not NULL, *N points to the newly
   created node.
   Checks whether there already exists such a node.  */
error_t
fs_make_node (struct node **n, struct node *dir,
              char* name, mode_t m)
{
  struct node*  newnode = NULL;
  error_t err = 0;

  /* DIR == NULL means that we are creating NETFS_ROOT_NODE. */
  if (dir)
    newnode = _find_node (dir, name);

  /* Creates a new one if not found. */
  if (!newnode)
  {
    /* Make sure the filetype bits are set */
    m = (m & S_IFMT) ? m : (m | S_IFREG);
    name = name ? strdup (name) : NULL;
    err = _make_node (&newnode, dir, name, m);
  }
  else
    err = EEXIST;

  /* Return a pointer to the newly created node. */
  if (n)
    *n = newnode;

  return err;
}

/* Looks for a node located at PATH, starting at directory N.
   When looking for "/foo/bar":
    - if "/foo/bar" exists, a reference to it is returned in N and
      the remaining parameters are set to NULL;
    - if "/foo" doesn't exist, a reference to "/" is returned and RETRY_NAME
      is set to "foo/bar" and NOTFOUND is set to "foo"; N points to "/foo";
    - if "/foo" exists but "/foo/bar" doesn't, then RETRY is NULL but NOTFOUND
      is equal to "bar".  */
error_t
fs_find_node_path (struct node **n, char **retry_name, char **notfound,
		   const char *path)
{
  struct node *node = NULL;
  char *str, *pathstr;
  char *name = NULL;

  pathstr = str = strdup (path);

  /* Lookup nodes. */
  if (! *n)
    *n = netfs_root_node;
  node = *n;
  name = strtok_r (pathstr, "/", &str);

  while (node && name)
  {
    /* Lookup base node. */
    node = _find_node (*n, name);
    if (node)
    {
      name = strtok_r (NULL, "/", &str);
      *n   = node;
    }
  }

  /* Did we parse the whole string? */
  if (*str == '\0')
  {
    if (!node)
    {
      /* Yes, but we didn't find the very last node. */
      assert (name != NULL);
      assert (strlen (name) != 0);

      *notfound   = strdup (name);
      *retry_name = NULL;
    }
    else
      /* Yes, and we did find it. */
      *notfound = *retry_name = NULL;
  }
  else
  {
    /* No, we stopped before the end of the string. */
    assert (name != NULL);
    assert (strlen (name) != 0);
    *notfound   = strdup (name);
    *retry_name = strdup (str);
  }

  free (pathstr);

  return 0;
}

/* Tries to create a node located at PATH, starting at directory N.
   When creating "/foo/bar":
    - if "/foo" exists and is a directory, "/foo/bar" is created and
      a reference to it is returned; RETRY_NAME is NULL; N points to "/foo/bar".
    - if "/foo" doesn't exist, a reference to "/" is returned and RETRY_NAME
      is set to "foo/bar" and NOTFOUND is set to "foo"; N points to "/foo".  */
error_t
fs_make_node_path (struct node **n, char **retry_name, char **notfound,
		   const char *path, const mode_t m)
{
  struct node *updir = *n;

  fs_find_node_path (&updir, retry_name, notfound, path);
  
  /* If all parent dirs have been found, then create the new node. */
  if (!*retry_name)
  {
    assert (*notfound != NULL);
#ifdef DEBUG_FS
    fprintf (stderr, "%s: Creating %s\n", __FUNCTION__, *notfound);
#endif
    fs_make_node (n, updir, *notfound, m);
    /* Do *not* free *NOTFOUND. */

    free (*notfound);
    *notfound = NULL;
  }

  return 0;
}

/* Used to add a sub-directory to DIR. If SUBDIRNAME already exists in DIR,
   returns the number of entries in it; otherwise creates it and returns
   zero. NEWDIR points to DIR/SUBDIRNAME.
   It also checks whether SUBDIRNAME already exists.  */
unsigned long
fs_make_subdir (struct node **newdir, struct node *dir, char *subdirname)
{
  unsigned long nodenum = 0;
  struct node *n, *p;

  /* Look for an existing dir */
  n = _find_node (dir, subdirname);

  if (!n)
    /* Create a new sub-directory. */
    fs_make_node (&n, dir, subdirname, S_IFDIR|0555);
  else
    /* Compute the node number. */
    for (p = n->nn->entries; p; p = p->next)
      if (!S_ISDIR (p->nn_stat.st_mode))
	nodenum++;

  *newdir = n;
  return nodenum;
}


/* Returns the path of a given node (relatively to the given root node).
   This is a very funny function (see macro below). ;-) */
char*
fs_get_path_from_root (struct node *root, struct node *node)
{
#define REVERSE_COPY(dst, src) \
	{ int i; \
	  for (i=0; i < strlen ((src)); i++) \
	    (dst)[i] = src[strlen ((src)) - 1 - i]; \
	  (dst)[strlen ((src))] = '\0'; \
	}

  struct node *n;
  size_t len = 256;
  char *path;
  char *ptr;

  path = (char*)calloc(len, sizeof(char));
  ptr  = path;

  for (n = node;
       (n != root) && (n != NULL);
       n = n->nn->dir)
  {
    /* Reallocate if necessary. */
    if (strlen (path) + strlen (n->nn->name) + 1 + 1 > len)
    {
      char* new;
      len *= 2;
      new = realloc (path, len);
      ptr = new + (ptr - path);
      path = new;
    }
    REVERSE_COPY (ptr, n->nn->name);
    ptr[strlen (n->nn->name)] = '/';
    ptr += strlen (n->nn->name) + 1;
  }

  /* Remove trailing slash. */
  if (strlen (path))
    path[strlen (path) - 1] = '\0';

  /* Reverse-copy the final result. */
  ptr = (char*)calloc (strlen (path) + 1, sizeof (char));
  REVERSE_COPY (ptr, path);
  free (path);

  return ptr;
}

/* Returns the relavive path to the given root node.  */
char*
fs_get_path_to_root (struct node *root, struct node *node)
{
  struct node *n;
  size_t len = 256;
  char *path;
  char *ptr;

  /* Go to the parent dir if NODE is not a directory. */
  if (! (node->nn_stat.st_mode & S_IFDIR))
    node = node->nn->dir;

  path = (char*)calloc(len, sizeof(char));
  ptr  = path;

  for (n = node;
       (n != root) && (n != NULL);
       n = n->nn->dir)
  {
    /* Reallocate if necessary. */
    if (strlen (path) + 3 + 1 > len)
    {
      char* new;
      len *= 2;
      new = realloc (path, len);
      ptr = new + (ptr - path);
      path = new;
    }
    strncpy (ptr, "../", 3);
    ptr += 3;
  }

  /* Remove last slash. */
  assert (strlen (path) > 0);
  path[strlen (path) - 1] = '\0';

  /* Reverse-copy the final result. */
  ptr = (char*)calloc (strlen (path) + 1, sizeof (char));
  strcpy (ptr, path);
  free (path);

  return ptr;
}

/* Gets the first common directory.  */
struct node*
get_common_root (struct node *node1, struct node *node2)
{
#define MAX_PATH_DEPTH 256
  struct node *n1 = node1, *n2 = node2;
  struct node *path1[MAX_PATH_DEPTH];
  struct node *path2[MAX_PATH_DEPTH];
  int i1 = 0, i2 = 0;

  if (n1 == n2)
    return n1;

  /* Save pathes to NETFS_ROOT_NODE in a stack. */
  do
  {
    assert (i1 < MAX_PATH_DEPTH);
    path1[i1++] = n1 = n1->nn->dir;
  }
  while (n1 != netfs_root_node);

  do
  {
    assert (i2 < MAX_PATH_DEPTH);
    path2[i2++] = n2 = n2->nn->dir;
  }
  while (n2 != netfs_root_node);

  /* Get to the last common node. */
  while (path1[--i1] == path2[--i2]);

  return path1[++i1];
}

/* Makes NODE a symlink to TARGET, relatively to root directory ROOTDIR.  */
error_t
fs_link_node (struct node *node, struct node *target)
{
  char *toroot, *tolink, *link;
  struct node *rootdir;

  /* Make it look like a symlink. */
  node->nn_stat.st_mode |= S_IFLNK;
  node->nn_translated   |= S_IFLNK;

  rootdir = get_common_root (node, target);
  toroot  = fs_get_path_to_root (rootdir, node);
  tolink  = fs_get_path_from_root (rootdir, target);
  link    = calloc (strlen (toroot) + 1 + strlen (tolink) + 1, sizeof (char));
  sprintf (link, "%s/%s", toroot, tolink);

  node->nn->symlink     = link;
  node->nn_stat.st_size = strlen (link);

  return 0;
}

/* Turn NODE into a symbolic link to TARGET.  */
error_t
fs_link_node_path (struct node *node, const char *target)
{
  /* Make it look like a symlink. */
  node->nn_stat.st_mode |= S_IFLNK;
  node->nn_translated   |= S_IFLNK;

  assert (node->nn);
  node->nn->symlink = strdup (target);
  node->nn_stat.st_size = strlen (target);

  return 0;
}

/* Creates a new node NODE, in directory DIR, with name NAME and mode
   M, hard linked to TARGET.  */
error_t
fs_hard_link_node (struct node **node, struct node *dir, char* name,
		   const mode_t m, struct node *target)
{
  struct netnode *nn;
  struct node*   newnode = NULL;

  /* Alloctes a new netnode */
  nn = (struct netnode*) calloc (1, sizeof (struct netnode));
  if (!nn)
    return ENOMEM;
  newnode = netfs_make_node (nn);
  if (!newnode)
    return ENOMEM;

  /* Increase TARGET's hard links count.  */
  target->nn_stat.st_nlink++;
  netfs_nref (target);

  /* Copies netnode from TARGET (optional since only TARGET should be
     accessed).  */
  newnode->nn_stat = target->nn_stat;
  newnode->nn_stat.st_mode = m; /* FIXME: Should keep the upper bits.  */
  newnode->nn_translated = m;
  newnode->nn_stat.st_nlink--;  /* XXX: One less hard link?  */
  *newnode->nn      = *target->nn;
  newnode->nn->name = name;
  newnode->next     = NULL;
  newnode->prevp    = NULL;

  /* Mark NEWNODE as a hard link to TARGET.  */
  newnode->nn->hardlink = target;

  if (dir)
  {
    struct node *p;
    netfs_nref (dir);

    /* Insert the new node *at the end* of the linked list of DIR entries. */
    if (dir->nn->entries)
    {
      for (p = dir->nn->entries;
	   p->next;
	   p = p->next);
      newnode->prevp = &p;
      p->next = newnode;
    }
    else
      dir->nn->entries = newnode;

    newnode->nn->dir = dir;

    /* Make sure that DIR is a directory. */
    dir->nn_stat.st_mode |= S_IFDIR;
  }

  fshelp_touch (&newnode->nn_stat,
      TOUCH_ATIME|TOUCH_CTIME|TOUCH_MTIME, curr_time);

  if (node)
    *node = newnode;

  return 0;
}

/* Unlink NODE *without* freeing its resources.  */
error_t
fs_unlink_node (struct node *node)
{
  struct node *dir  = node->nn->dir;
  struct node *next = node->next;

  if (node->nn->entries)
    return ENOTEMPTY;

  /* Check the number of hard links to NODE.  */
  if (S_ISDIR (node->nn_stat.st_mode))
  {
    if (node->nn_stat.st_nlink > 2)
      return EBUSY;
  }
  else
  {
    if (node->nn_stat.st_nlink > 1)
      return EBUSY;
  }

  /* PREVP should never be zero.  */
  assert (node->prevp);

  /* Unlink NODE */
  if (*node->prevp)
    *node->prevp = next;

  if (next)
    next->prevp = node->prevp;

  /* Decrease the reference count to the hardlink targets */
  if (node->nn->hardlink)
  {
    node->nn->hardlink->nn_stat.st_nlink--;
    netfs_nput (node->nn->hardlink);
  }

  /* Same for directories ('..' links to DIR) */
  if (dir && S_ISDIR (node->nn_stat.st_mode))
    dir->nn_stat.st_nlink--;

  /* Finally, drop a reference to the node itself, which may result
     in calling netfs_node_norefs ().  */
  netfs_nput (node);

  /* Drop a reference from DIR */
  netfs_nput (dir);

  return 0;
}

/* Frees all memory associated to NODE (which is assumed to be already
   unlinked) except its 'nn->info' field.  */
void
fs_free_node (struct node *node)
{
  struct netnode *nn = node->nn;

  assert (nn);

  if (nn->name)
    free (nn->name);
  if (nn->symlink)
    free (nn->symlink);

  free (nn);
  node->nn = NULL;
}
