/* tarfs - A GNU tar filesystem for the Hurd.
   Copyright (C) 2002, 2003  Ludovic Courtès <ludo@type-z.org>
 
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
 
   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
 
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
   USA */

#include <hurd.h>
#include <hurd/netfs.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <error.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <argp.h>
#include <argz.h>

#include "backend.h"
#include "tarfs.h"
#include "fs.h"
#include "cache.h"
#include "zipstores.h"
#include "debug.h"

/* New netfs variables */
char *netfs_server_name = "tarfs";
char *netfs_server_version = "(rw-alpha)";

/* Filesystem options */
struct tarfs_opts tarfs_options;
const char *argp_program_version =
  "tarfs(rw-alpha) for the GNU Hurd (compiled: " __DATE__ ")";

/* Argp data */
const char *argp_program_bug_address = "Ludovic Courtès <ludo@type-z.org>";
const char *args_doc = "ARCHIVE";
const char *doc = "Hurd tar filesystem:\n"
   "parses a tar archive and creates the corresponding filesystem\n";

const struct argp_option fs_options[] =
{
#ifdef DEBUG
  { "debug",        'D', "FILE", 0, "Print debug output to FILE" },
#endif
  { "gzip",         'z', NULL, 0, "Archive file is gzipped" },
  { "bzip2",        'j', NULL, 0, "Archive file is bzip2'd" },
  { "no-timeout",   't', NULL, 0, "Parse file in a separate thread "
				  "(thus avoiding startup timeouts)" },
  { "readonly",     'r', NULL, 0, "Start tarfs read-only" },
  { "writable",     'w', NULL, 0, "Start tarfs writable (default)" },
  { "volatile",     'v', NULL, 0, "Start tarfs volatile "
  				  "(ie writable but not synced)" },
  { "create",       'c', NULL, 0, "Create tar file if not there" },
#if 0
  { "sync",         's', "INTERVAL", 0, "Sync all data not actually written "
				  "to disk every INTERVAL seconds (by "
				  "default, the data is *not* synced unless "
				  "explicitely requested)" },
#endif
  { 0 }
};

/* Tar file store & lock.  */
static struct store *tar_file;
static struct mutex  tar_file_lock;

/* Archive parsing hook (see tar.c) */
extern int (* tar_header_hook) (tar_record_t *, off_t);

/* List of tar items for this file */
static struct tar_list tar_list;

/* Data used by netfs_get_dirents() */
static struct node *curr_dir;
static struct node *curr_node;
static int    curr_entry = 0;

/* A convenience macro */
#define IF_RWFS \
  if (tarfs_options.readonly) \
    return EROFS;


#define D(_s) strdup(_s)

/* Open the tar file STORE according to TARFS_OPTIONS.  Assumes the
   store is already locked.  */
static error_t
open_store ()
{
  error_t err;
  int     flags = tarfs_options.readonly || tarfs_options.volatil
 		  ? STORE_READONLY
 		  : 0;

  switch (tarfs_options.compress)
  {
    case COMPRESS_NONE:
      err = store_file_open (tarfs_options.file_name, flags, &tar_file);
      break;
    case COMPRESS_GZIP:
      err = store_gzip_open (tarfs_options.file_name, flags, &tar_file);
      break;
    case COMPRESS_BZIP2:
      err = store_bzip2_open (tarfs_options.file_name, flags, &tar_file);
      break;
    default:
      error (1, EINVAL, "Compression method not implemented (yet)");
  }

  return err;
}

/* Close the tar file assuming that it is already locked.  */
static void
close_store ()
{
  store_free (tar_file);
  tar_file = NULL;
}

/* Reads NODE from file.  This is called by the cache backend.  */
static error_t
read_from_file (struct node *node, off_t offset, size_t howmuch,
                size_t *actually_read, void *data)
{
  error_t err = 0;
  store_offset_t start = NODE_INFO(node)->tar->offset;
  void *d = data;

  mutex_lock (&tar_file_lock);

  if (!tar_file)
    err = open_store ();

  if (!err)
    err = store_read (tar_file,
		      start + offset,
		      howmuch,
		      &d,
		      actually_read);

  mutex_unlock (&tar_file_lock);

  if (err)
    return err;

  assert (*actually_read <= howmuch);

  /* Checks whether store_read() has allocated a new buffer.  */
  if (data != d)
  {
    memcpy (data, d, *actually_read);
    munmap (d, *actually_read);
  }

  return 0;
}


/* Argp options parser.  */
error_t
tarfs_parse_opts (int key, char *arg, struct argp_state *sate)
{
  switch (key)
  {
#ifdef DEBUG
    case 'D':
      debug_set_file (arg);
      break;
#endif
    case 'c':
      tarfs_options.create = 1;
      break;
    case 'v':
      tarfs_options.volatil = 1;
      break;
    case 'r':
      tarfs_options.readonly = 1;
      break;
    case 'w':
      tarfs_options.readonly = 0;
      break;
    case 't':
      tarfs_options.threaded = 1;
      break;
    case 'z':
      tarfs_options.compress = COMPRESS_GZIP;
      break;
    case 'j':
      tarfs_options.compress = COMPRESS_BZIP2;
      break;
    case 's':
      tarfs_options.interval = atoi (arg);
      break;
    case ARGP_KEY_ARG:
      tarfs_options.file_name = strdup (arg);
      if (!tarfs_options.file_name || !strlen (tarfs_options.file_name))
	error (1, 1, "No archive specified.");
  }

  return 0;
}

/* Returns the tarfs' struct argp.  */
void
tarfs_get_argp (struct argp *a)
{
  bzero (a, sizeof (struct argp));
  a->options  = fs_options;
  a->parser   = tarfs_parse_opts;
  a->args_doc = args_doc;
  a->doc      = doc;
}

/* Append to the malloced string *ARGZ of len *ARGZ_LEN a NULL-separated list
   of arguments.  */
error_t
tarfs_get_args (char **argz, unsigned *argz_len)
{
  error_t err = 0;
  
  if (!err && tarfs_options.volatil)
    err = argz_add (argz, argz_len, "--volatile");
  else
  {
    if (tarfs_options.readonly)
      err = argz_add (argz, argz_len, "--readonly");
    else
      err = argz_add (argz, argz_len, "--writable");
  }

  if (err)
    return err;

  switch (tarfs_options.compress)
  {
    case COMPRESS_GZIP:
      err = argz_add (argz, argz_len, "--gzip");
      break;
    case COMPRESS_BZIP2:
      err = argz_add (argz, argz_len, "--bzip2");
  }

  if (err)
    return err;

  err = argz_add (argz, argz_len, tarfs_options.file_name);
  
  return err;
}

/* A basic set_options (). Only runtime options can be changed (using
   fsysopts): for instance, --no-timeout won't work (it doesn't make
   sense when tarfs is already running).  */
error_t
tarfs_set_options (char *argz, size_t argz_len)
{
  error_t err = 0;

  /* If going readonly (resp. read-write) while the store is currently
     opened read-write (resp. read-only) then close it first.  */

  if (!strcmp (argz, "-r") || !strcmp (argz, "--readonly"))
  {
    if (!tarfs_options.readonly)
    {
      mutex_lock (&tar_file_lock);
      tarfs_options.readonly = 1;
      close_store ();
      err = open_store ();
      mutex_unlock (&tar_file_lock);

      if (err)
	tarfs_options.readonly = 0;
      else
        tarfs_options.volatil  = 0;
    }
  }
  else if (!strcmp (argz, "-w") || !strcmp (argz, "--writable"))
  {
    if (tarfs_options.readonly)
    {
      mutex_lock (&tar_file_lock);
      tarfs_options.readonly = 0;
      close_store ();
      err = open_store ();
      mutex_unlock (&tar_file_lock);

      if (err)
	tarfs_options.readonly = 1;
      else
        tarfs_options.volatil  = 0;
    }
  }
  else if (!strcmp (argz, "-v") || !strcmp (argz, "--volatile"))
    tarfs_options.readonly = 0, tarfs_options.volatil = 1;
  else
    err = EINVAL;

  return err;
}



error_t tarfs_create_node (struct node **newnode, struct node *dir,
			   char *name, mode_t mode);

/* This function is called every time a header has been successfully parsed.
   It simply creates the node corresponding to the header.
   OFFSET denotes the offset of the header in the archive.  */
int
tarfs_add_header (tar_record_t *hdr, off_t offset)
{
  error_t err;
  static struct tar_item *last_item = NULL;
  struct node *dir, *new = NULL;
  char *name, *notfound, *retry;
  
  assert (hdr != NULL);

  dir = netfs_root_node;
  name = D (hdr->header.arch_name);
  assert (name);
  debug (("name = %s", name));

  /* Find the new node's parent directory.  */
  do
  {
    err = fs_find_node_path (&dir, &retry, &notfound, name);

    /* If a subdirectory wasn't found, then complain, create it and go on.
       eg.: if we wan't to create "/foo/bar" and "/foo" does not exist
            yet, then create "/foo" first and then continue with "bar".  */
    if (retry)
    {
      error (0, 0, "Inconsistent tar archive "
		   "(directory \"%s\" not found)", notfound);
      err = tarfs_create_node (&new, dir, notfound, S_IFDIR | 755);
      assert_perror (err);
      /*fs_make_subdir (&new, dir, notfound);
      NEW_NODE_INFO (new);*/
      free (name);
      name = retry;
    }
  }
  while (retry);

  if (!notfound)
  {
    /* Means that this node is already here: do nothing.  Complain only
       if the node we found is not the root dir ("tar cf x ." creates
       './' as the first tar entry).  */
    if (dir != netfs_root_node)
      error (0, 0, "Warning: node \"%s\" already exists", name);

    return 0;
  }

  /* Now, go ahead and create the node.  */
  name = notfound;
  assert (strlen (name) > 0);

  switch (hdr->header.linkflag)
  {
    /* Hard link.  */
    case LF_LINK:
    {
      char *tgname;
      struct node *target;

      debug (("Hard linking \"%s\"", name));

      /* Get the target's node first. */
      tgname = strdup (hdr->header.arch_linkname);
      target = netfs_root_node;
      fs_find_node_path (&target, &retry, &notfound, tgname);

      if ((!retry) && (!notfound))
      {
	/* FIXME: Call tarfs_create_node () and tarfs_link_node instead */
	fs_hard_link_node (&new, dir, name, target->nn_stat.st_mode, target);

	/* Update node info & stats */
	if (new)
	{
	  NEW_NODE_INFO (new);

          /* No need to create a cache for hard links.  */

	  /* Add the tar item into the list.  */
	  err = tar_make_item (&NODE_INFO(new)->tar, new,
			       0, offset);
	  assert_perror (err);
	}
      }
      else
	error (0, 0, "Hard link target not found (%s -> %s)", name, tgname);

      break;
    }

    /* Other node types.  */
    default:
      err = fs_make_node (&new, dir, name, 0);
      assert_perror (err);

      /* Update node info & stats */
      if (new)
      {
	NEW_NODE_INFO (new);
	tar_header2stat (&new->nn_stat, hdr);

	/* Create a cache for the new node.  */
	err = cache_create (new);
	if (err)
	  error (1, err, "An error occured while creating the filesystem");

	/* Add the tar item into the list.  */
	err = tar_make_item (&NODE_INFO(new)->tar, new,
			     new->nn_stat.st_size, offset);
	assert_perror (err);
      }
  }

  if (!new || err)
    error (1, err, "Filesystem could not be built");

  tar_insert_item (&tar_list, last_item, NODE_INFO(new)->tar);
  last_item = NODE_INFO(new)->tar;

#if 0
  debug (("%s: Created node \"%s\" (size=%li)\n",
          __FUNCTION__, name, new->nn_stat.st_size));
#endif

  /* Symlinks handling */
  if (S_ISLNK (new->nn_stat.st_mode))
  {
    if (hdr->header.arch_linkname[0])
      fs_link_node_path (new, strdup (hdr->header.arch_linkname));
    else
    {
      error (0, 0, "Warning: empty symlink target for node \"%s\"",
	     new->nn->name);
      fs_link_node_path (new, strdup (""));
    }
  }

  /* Directories */
  if (S_ISDIR (new->nn_stat.st_mode))
  {
    new->nn_stat.st_nlink = 2;
    new->nn->dir->nn_stat.st_nlink++;
  }

  return 0;
}


error_t
tarfs_init (struct node **root, struct iouser *user)
{
  error_t err = 0;
  io_statbuf_t st;
  int    flags;
  file_t tarfile;
  mode_t mode = 0644;

  /* Sync the archive.  */
  static void
  sync_archive ()
  {
    error_t tarfs_sync_fs (int wait);

    while (1)
    {
      sleep (tarfs_options.interval);

      if (!tarfs_options.readonly)
	tarfs_sync_fs (0);
    }
  }

  /* Reads and parses a tar archive, possibly in a separate thread.  */
  static void
  read_archive ()
  {
    error_t err;

    /* Go ahead: parse and build.  */
    mutex_lock (&tar_file_lock);
    err = tar_open_archive (tar_file);
    mutex_unlock (&tar_file_lock);

    if (err)
      error (1, 0, "Invalid tar archive (%s)", tarfs_options.file_name);
#if 0
    else
      cthread_fork ((cthread_fn_t) sync_archive, NULL);
#endif
  }


  /* Init. */
  if ((! tarfs_options.file_name) || (! strlen (tarfs_options.file_name)))
    error (1, 0, "No file specified");
  if (tarfs_options.create)
    tarfs_options.readonly = 0, flags = O_CREAT | O_READ | O_WRITE;
  else
    flags = tarfs_options.readonly || tarfs_options.volatil
            ? O_READ
            : O_READ | O_WRITE;

  /* Get the tarfile's stat.  */
  tarfile = file_name_lookup (tarfs_options.file_name, flags, mode);
  if (tarfile != MACH_PORT_NULL)
  {
    /* Check permissions */
    err = io_stat (tarfile, &st);

    if (!err && (flags & O_READ))
      err = fshelp_access (&st, S_IREAD, user);
    if (!err && (flags & O_WRITE))
      err = fshelp_access (&st, S_IWRITE, user);
  }
  else
    err = errno;

  if (err)
  {
    error (1, err, "%s", tarfs_options.file_name);
    return err;
  }
  mach_port_deallocate (mach_task_self (), tarfile);

  err = fs_init ();
  if (err)
    return err;

  /* Create root node */
  st.st_mode &= ~S_IFMT;
  st.st_mode |= S_IFDIR | S_IROOT | S_IATRANS;
  err = fs_make_node (&netfs_root_node, NULL, NULL, st.st_mode);
  if (err)
    return err;

  /* Parse the archive and build the filesystem */
  cache_init (read_from_file);
  tar_header_hook = tarfs_add_header;
  tar_list_init (&tar_list);

  /* Open the corresponding store */
  err = open_store ();
  if (err)
    error (1, err, "%s", tarfs_options.file_name);

  assert (tar_file);

  /* We make the following assumption because this is the way it's gotta
     be with these stores.  */
  assert (tar_file->block_size == 1);

  if (st.st_size)
  {
    if (tarfs_options.threaded)
      cthread_fork ((cthread_fn_t) read_archive, NULL);
    else
      read_archive ();
  }

  return 0;
}


int
tarfs_set_cd (struct node *dir)
{
  curr_dir = dir;
  curr_node = dir->nn->entries;

  /* Skip anonymous nodes (created by dir_mkfile ()).  */
  while (curr_node && (! curr_node->nn->name))
    curr_node = curr_node->next;

  curr_entry = 0;
  return 0;
}

int
tarfs_skip_entries (int n)
{
  assert (n >= 0);

  /* Skip N first DIR entries. */
  curr_node = curr_dir->nn->entries;

  if (n > 2)
  {
    /* Skip more than `.' and `..' */
    curr_entry = 2;
    while ((curr_entry < n) && (curr_node))
    {
      /* Skip anonymous nodes (created by dir_mkfile ()).  */
      do
	curr_node = curr_node->next;
      while (curr_node && (! curr_node->nn->name));

      curr_entry++;
    }
  }
  else
    curr_entry = n;
  
  /* Returns non-null if could not skip N entries. */
  return (curr_entry<=n)?0:1;
}

static inline int
_new_dirent (struct dirent** e, const struct node *n, const char* nodename)
{
  size_t namelen;
  char*  name;

  assert (nodename != NULL);

  /* N==NULL means that we are considering the node on which the
     translator is set.  */
  namelen = (n) ? strlen (nodename) : 2;

  /* Allocate it. */
  *e = mmap (NULL, sizeof (struct dirent) + namelen,
	     PROT_READ|PROT_WRITE, MAP_ANONYMOUS, 0, 0);
  assert (*e != NULL);
  
  /* Copy node name */
  name = &(*e)->d_name[0];

  if (n == NULL)
  {
    /* `..' */
    memcpy (name, nodename, 3);
    namelen = 2;
    (*e)->d_type = DT_DIR;
    (*e)->d_ino  = netfs_root_node->nn_stat.st_ino;
  }
  else
  {
    memcpy (name, nodename, namelen);

    /* Set the type corresponding to n->nn_stat.st_mode */
    if (n->nn_stat.st_mode & S_IFREG)
      (*e)->d_type = DT_REG;
    else if (n->nn_stat.st_mode & S_IFDIR)
      (*e)->d_type = DT_DIR;
    else if (n->nn_stat.st_mode & S_IFLNK)
      (*e)->d_type = DT_LNK;
    else
      (*e)->d_type = DT_UNKNOWN;

    /* if FILENO==0 then the node won't appear. */
    (*e)->d_fileno = n->nn_stat.st_ino;
  }

  assert (namelen != 0);

  (*e)->d_namlen = namelen;
  (*e)->d_reclen = sizeof (struct dirent) + namelen;

  return 0;
}

int
tarfs_get_next_entry (struct dirent **entry)
{
  switch (curr_entry++)
  {
    case 0:
      _new_dirent (entry, curr_dir, ".");
      break;
    case 1:
      _new_dirent (entry, curr_dir->nn->dir, "..");
      break;
    default:
      if (!curr_node)
	return 1;	/* no more entries */
      else
      {
	_new_dirent (entry, curr_node, curr_node->nn->name);

	/* Skip anonymous nodes (created by dir_mkfile ()).  */
	do
	  curr_node = curr_node->next;
	while (curr_node && (! curr_node->nn->name));
      }
      break;
  }

  return 0;
}

/* Looks up node named NAME and returns the result in NODE.  */
error_t
tarfs_lookup_node (struct node** node, struct node* dir, const char* name)
{
  struct node *n = dir->nn->entries;

  /* Look for NAME in DIR entries. */
  while (n)
  {
    char *this = n->nn->name;

    if (this)
      if (!strcmp (this, name))
        break;

    n = n->next;
  }

  *node = n;

  if (!n)
    return ENOENT;

  return 0;
}

error_t
tarfs_read_node (struct node *node, off_t offset, size_t *len, void* data)
{
  if (S_ISDIR (node->nn_stat.st_mode))
  {
    *len = 0;
    return EISDIR;
  }
  else
    return cache_read (node, offset, *len, data, len);
}


/* Write to NODE through its cache.  */
error_t
tarfs_write_node (struct node *node, off_t offset, size_t *len, void *data)
{
  IF_RWFS;

  if (S_ISDIR (node->nn_stat.st_mode))
  {
    *len = 0;
    return EISDIR;
  }
  else
  {
    error_t err;
    /* Checks whether we need to actually write to another node.
       (hard links are not handled by cache_write ()).  */
    struct node *what = node->nn->hardlink ? node->nn->hardlink : node;
    
    err = cache_write (node, offset, data, *len, len);

    /* Synchronize stat with hard link's target.  */
    if ((! err) && (what != node))
      node->nn_stat.st_size = what->nn_stat.st_size;

    return err;
  }
}

/* Update NODE stat structure and mark it as dirty.  */
error_t
tarfs_change_stat (struct node *node, const io_statbuf_t *st)
{
  error_t err = 0;
  struct node *what = node->nn->hardlink ? node->nn->hardlink : node;

  IF_RWFS;

  if (st->st_size != what->nn_stat.st_size)
    /* Update the cache size */
    err = cache_set_size (what, st->st_size);

  if (!err)
  {
    what->nn_stat = *st;
    NODE_INFO(what)->stat_changed = 1;

    /* Synchronize NODE with its TARGET if it's a hard link.  */
    if (what != node)
    {
      node->nn_stat = what->nn_stat;
      NODE_INFO(node)->stat_changed = 1;
    }
  }

  return err;
}


/* Create a node named NAME in directory DIR. If NEWNODE is non-zero then
   it will point to the new node.  NAME is duplicated.  */
error_t
tarfs_create_node (struct node **newnode, struct node *dir,
		   char *name, mode_t mode)
{
  error_t err;
  struct node *new = NULL;

  IF_RWFS;

  /* Allow anonymous (nameless) nodes (created by dir_mkfile ()).  */
  if (name)
  {
    /* NODE's path has to be at most NAMSIZ long. */
    char *path = fs_get_path_from_root (netfs_root_node, dir);
    if (strlen (name) + strlen (path) + 1 > NAMSIZ)
      return ENAMETOOLONG;

    debug (("Creating node %s", name));
  }
  else
  {
    debug (("Creating anonymous node", dir->nn->name));

    /* Don't add anonymous nodes into the tar list.  */
    err = fs_make_node (&new, dir, NULL, mode);

    if (!err && new)
      NEW_NODE_INFO (new);

    *newnode = new;

    err = cache_create (new);

    return err;
  }

  err = fs_make_node (&new, dir, strdup (name), mode);
  if (!err && new)
  {
    struct tar_item *tar, *prev_tar;

    NEW_NODE_INFO (new);
    err = cache_create (new);

    if (!err)
    {
      /* Insert a corresponding tar item into the list.
	 Offset `-1' denotes a note that does not exist inside the tar file.  */
      err = tar_make_item (&tar, new, 0, -1);
      assert_perror (err);

      /* Find a place to put TAR.  */
      tar_put_item (&prev_tar, tar);
      tar_insert_item (&tar_list, prev_tar, tar);
    }
  }

  if (newnode)
    *newnode = new;

  return err;
}

/* Unlink NODE.  NODE's tar_item will remain in the list until the filesystem
   is linked, *except* if its offset is `-1' (new node).  */
error_t
tarfs_unlink_node (struct node *node)
{
  error_t err = 0;
  struct tar_item *tar = NODE_INFO(node)->tar;

  IF_RWFS;

  debug (("Unlinking %s", node->nn->name));

  /* Delete NODE.  */
  err = fs_unlink_node (node);
  if (err)
    return err;

  /* If NODE has never existed inside the tar file, then remove its tar_item
     from the list.  */
  if (tar->offset == -1)
    tar_unlink_item (&tar_list, tar);

  return err;
}


void
tarfs_free_node (struct node *node)
{
  struct tar_item *tar = NODE_INFO (node)->tar;

  /* Free all related resources */
  cache_free (node);
  free (NODE_INFO (node));
  fs_free_node (node);
  tar->node = NULL;
}


/* Tries to create a hard link named NAME in DIR to file NODE.  */
error_t
tarfs_link_node (struct node *dir, struct node *target,
		 char *name, int excl)
{
  error_t err = 0;
  struct tar_item *prev_tar, *tar;
  struct node *new;

  if (fs_find_node (dir, name))
    return excl ? EEXIST : 0;

  /* If the link's target is anonymous (nameless), then don't create
     a new node, just change its name.  */
  if (!target->nn->name)
  {
    new = target;
    new->nn->name = strdup (name);

    /* Insert NEW into the tar list */
    err = tar_make_item (&tar, new, 0, -1);
    if (!err)
      tar_put_item (&prev_tar, tar);
  }
  else
  {
    err = fs_hard_link_node (&new, dir, strdup (name),
			     target->nn_stat.st_mode, target);
    if (! err && new)
    {
      struct tar_item *t;
      NEW_NODE_INFO (new);

      /* Insert NEW into the tar list */
      err = tar_make_item (&tar, new, 0, -1);
      if (!err)
      {
	tar_put_item (&prev_tar, tar);

	/* Since NEW must appear after TARGET in the tar list,
	   Make sure that PREV_TAR comes *before* TARGET's tar, otherwise
	   set PREV_TAR to be TARGET's tar item.  */
	for (t = NODE_INFO(target)->tar;
	     t && (t != prev_tar);
	     t = t->next);

	if (!t)
	  prev_tar = NODE_INFO(new)->tar;
      }
    }
  }

  if (!err)
  {
    tar_insert_item (&tar_list, prev_tar, tar);
    NODE_INFO(new)->tar = tar;
  }

  return err;
}

/* Tries to turn NODE into a symlink to TARGET.  */
error_t
tarfs_symlink_node (struct node *node, const char *target)
{
  error_t err;

  err = fs_link_node_path (node, target);

  return err;
}

/* Tries to turn NODE into a device of type TYPE (either S_IFBLK or S_IFCHR).
 */
error_t
tarfs_mkdev_node (struct node *node, mode_t type, dev_t indexes)
{
  debug (("Not implemented"));
  return EOPNOTSUPP;
}


/* Rounds SIZE to the upper RECORDSIZE.  */
static inline size_t
round_size (size_t s)
{
   return  (RECORDSIZE * ( (s / RECORDSIZE) \
      			    + ((s % RECORDSIZE) ? 1 : 0 )) );
}

/* Cache nodes ahead CURR_TAR whose data reside in the region
   [OFFS, OFFS+SIZE] of the tar file.  */
static inline error_t
cache_ahead (struct tar_item *curr_tar, off_t offs, size_t size)
{
  error_t err = 0;
  struct node *node;	/* Corresponding node */
  size_t node_size;	/* Node size          */
  off_t  node_offs;	/* Node offset        */

  assert (size);

  do
  {
    /* Looks for an item available in the tar file.  */
    while (curr_tar)
      if ((curr_tar->offset != -1) && (curr_tar->node))
	break;
      else
        curr_tar = curr_tar->next;

    if (curr_tar)
    {
      node = curr_tar->node;
      node_offs = curr_tar->offset;
      node_size = node->nn_stat.st_size;

      /* If we are beyond NODE's boundary, assume it's already cached.  */
      if ( (offs < node_offs + node_size)
           && (offs + size > node_offs) )
      {
	/* Cache either the whole node or just what we need.  */
	size_t how_much;
	how_much = (offs + size > node_offs + node_size)
	           ? node_size
	           : offs + size - node_offs;

	debug (("Caching %i bytes from \"%s\"", how_much, node->nn->name));
	err = cache_cache (node, how_much);
	if (err)
	  return err;
      }

      curr_tar = curr_tar->next;
    }
  }
  while (curr_tar && (node_offs < offs + size));

  return err;
}

/* Store the filesystem into the tar file.  */
error_t
tarfs_sync_fs (int wait)
{
  error_t err = 0;
  char buf[RECORDSIZE];
  off_t  file_offs = 0; /* Current offset in the tar file */
  size_t orig_size = 0;	/* Total original tar file size */
  int need_trailing_block = 0; /* Do we need an additional block at the end? */
  struct tar_item *tar;

  /* Dump BUF to the tar file's store, enlarging it if necessary.  */
  error_t
  tar_write (off_t offset, void *buf, size_t len, size_t *amount)
  {
    error_t err = 0;
    int cnt = 0;

    while (1)
    {
      mutex_lock (&tar_file_lock);

      if (!tar_file)
	err = open_store ();

      if (!err)
	err = store_write (tar_file, offset, buf, len, amount);

      mutex_unlock (&tar_file_lock);

      cnt++;

      if (! err)
        break;
      if (cnt > 1)
        break;

      if (err == EIO)
      {
        /* Try to enlarge the file.  */
        debug (("Enlarging file from %lli to %lli",
               tar_file->size, offset + len));
        err = store_set_size (tar_file, offset + len);
        if (err)
          break;
      }
    }

    if (err)
      error (0, err,
	     "Could not write to file (offs="OFF_FMT")", file_offs);

    return err;
  }


  /* Traverse the tar items list and sync them.  */
  tar_list_lock (&tar_list);

  for (tar = tar_list_head (&tar_list);
       tar;
       /* TAR is incremented inside the loop */ )
  {
    struct node *node = tar->node;

    /* Compute the original tar file size.  */
    if (tar->offset != -1)
      orig_size += round_size (tar->orig_size) + RECORDSIZE;

    if (node)
    {
      int have_to_sync;
      char *path;
      size_t size;

      /* Lock the node first */
      mutex_lock (&node->lock);
      have_to_sync = (tar->offset != file_offs + RECORDSIZE);
      path = fs_get_path_from_root (netfs_root_node, node);
      size = node->nn_stat.st_size;

      /* Round SIZE.  */
      size = round_size (size);

      /* Synchronize NODE's stat.  */
      if ((NODE_INFO(node)->stat_changed) ||
	  (node->nn_stat.st_size != tar->orig_size) ||
	  (have_to_sync))
      {
	size_t amount;
	char *target;

	debug (("%s: syncing stat", path));

	/* Cache all the nodes that would have been overwritten otherwise.  */
	err = cache_ahead (tar, file_offs, RECORDSIZE);
	if (err)
	  break;

	target = node->nn->hardlink
		 ? fs_get_path_from_root (netfs_root_node, node->nn->hardlink)
		 : NULL;

	/* Create and write the corresponding tar header.  */
	tar_make_header ((tar_record_t *)buf, &node->nn_stat,
			 path, node->nn->symlink, target);

	err = tar_write (file_offs, buf, RECORDSIZE, &amount);
	if (err)
	  break;

	assert (amount == RECORDSIZE);

	/* Never finish the tar file with a stat record.  */
	need_trailing_block = 1;
      }
      file_offs  += RECORDSIZE;

      /* Synchronize NODE's contents except if it's a directory/link.  */
      if ((! S_ISDIR (node->nn_stat.st_mode))
          && (! node->nn->symlink)
          && (! node->nn->hardlink)
	  && ((! cache_synced (node)) || (have_to_sync)) )
      {
	off_t  start = file_offs; /* NODE's start */
	off_t  offs = 0; /* Offset in NODE */

	/* We don't need to cache_ahead () if we already are more than
	   one block behind the original item since we write only
	   RECORDSIZE bytes record.  */
	int ahead = (tar->offset - (long)start < RECORDSIZE);

	debug (("%s: syncing contents (%i bytes)", path, size));

	/* Write RECORDSIZE-long blocks.  */
	while (offs < size)
	{
	  size_t amount;
	  
	  if (ahead)
	  {
	    /* Cache everything that will be overlapped.  */
	    err = cache_ahead (tar, file_offs, RECORDSIZE);
	    if (err)
	      break;
	  }

	  err = cache_read (node, offs, RECORDSIZE, buf, &amount);
	  assert_perror (err);

	  /* Last block: fill it with zeros if necessary.  */
	  if (amount < RECORDSIZE)
	  {
	    assert (offs + RECORDSIZE == size);
	    bzero (&buf[amount], RECORDSIZE - amount);
	  }

	  /* Write the whole record, regardless of the amount of data
	     actually read.  */
	  err = tar_write (file_offs, buf, RECORDSIZE, &amount);
	  if (err)
	    break;

	  assert (amount == RECORDSIZE);

	  offs += RECORDSIZE;
	  file_offs += RECORDSIZE;
	}

	if (err)
	  break;

	/* Update NODE's offset *after* cache_ahead () !  */
	tar->offset = start;

	/* Update the original item size.  */
	tar->orig_size = node->nn_stat.st_size;

	/* If this is the last tar item, tell whether we need an additional
	   trailing block: if NODE's size is a RECORDSIZE multiple then we
	   need one.  */
	need_trailing_block = ! (tar->orig_size % RECORDSIZE);
      }
      else
      {
	/* Update NODE's offset.  */
	tar->offset = file_offs;

        /* Skip record anyway.  */
        file_offs += RECORDSIZE * ((node->nn_stat.st_size / RECORDSIZE)
                     + ( (node->nn_stat.st_size % RECORDSIZE) ? 1 : 0 ));
      }

      cache_free (node);
      free (path);
      mutex_unlock (&node->lock);

      /* Go to next item.  */
      tar = tar->next;
    }
    else
    {
      struct tar_item *next = tar->next;
      debug (("Node removed (size=%i)", tar->orig_size));
      tar_unlink_item_safe (&tar_list, tar);

      /* Go to next item.  */
      tar = next;
    }
  }

  tar_list_unlock (&tar_list);


  /* Add an empty record (FIXME: GNU tar added several of them) */
  if (!err)
  {
    size_t amount;

    if (!file_offs)
      error (0, 0, "Warning: archive is empty");

    bzero (buf, RECORDSIZE);
    err = tar_write (file_offs, buf, RECORDSIZE, &amount);

    if (err || (amount < RECORDSIZE))
    {
      if (!err)
        err = EIO;
    }

    file_offs += amount;
  }

  /* Checks whether the tar file needs to be truncated.  */
  if (!err && (file_offs < orig_size))
  {
    debug (("Truncating tar file from %u to "OFF_FMT" bytes",
            orig_size, file_offs));

    err = store_set_size (tar_file, file_offs);
    if (err)
      error (0, err, "Cannot truncate \"%s\"", tarfs_options.file_name);
  }


  if (!err)
  {
    /* Call store_free () to commit the changes. This is actually only useful
       for zip stores.  */
    close_store ();
  }

  return err;
}

/* Tarfs destructor.  */
error_t
tarfs_go_away ()
{
  error_t err;

  if (!tarfs_options.readonly && !tarfs_options.volatil)
  {
    err = tarfs_sync_fs (0);
    if (err)
      error (0, err, "Syncing failed");
  }

  if (tar_file)
    store_close_source (tar_file);

  debug (("Bye!"));

  return 0;
}


/* Defines the filesystem backend. */
struct fs_backend tarfs_backend =
{
  tarfs_init,
  tarfs_get_argp,
  tarfs_get_args,
  tarfs_set_options,
  tarfs_set_cd,
  tarfs_skip_entries,
  tarfs_get_next_entry,
  tarfs_lookup_node,
  tarfs_read_node,

  /* Write support */
  tarfs_write_node,
  tarfs_change_stat,
  tarfs_create_node,
  tarfs_unlink_node,

  tarfs_link_node,
  tarfs_symlink_node,
  tarfs_mkdev_node,

  tarfs_free_node,

  tarfs_sync_fs,
  tarfs_go_away
};
