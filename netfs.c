/* tarfs interface to libnetfs.
   Copyright (C) 2002 Ludovic Courtès <ludo@type-z.org>
 
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

#include <hurd.h>
#include <hurd/netfs.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "backend.h"
#include "debug.h"

/* BACKEND is defined in main.c */
extern struct fs_backend backend;

/* The following flag may be defined in order to hide files
   to users who do not own them. */
#ifdef HIDE_FILES_NOT_OWNED
#define OWNERSHIP(__node, __user) \
  fshelp_isowner (&(__node)->nn_stat, (__user));
#else
# define OWNERSHIP(__node, __user)  (0)
#endif /* HIDE_FILES_NOT_OWNED */

/* The user must define this function.  Lookup NAME in DIR (which is
   locked) for USER; set *NP to the found name upon return.  If the
   name was not found, then return ENOENT.  On any error, clear *NP.
   (*NP, if found, should be locked and a reference to it generated.
   This call should unlock DIR no matter what.)  */
error_t
netfs_attempt_lookup (struct iouser *user, struct node *dir, 
		      char *name, struct node **np)
{
  error_t err = 0;

  /* Lookups for "." and "..". */
  if (name[0] == '.' &&
      (name[1] == '\0' ||
       (name[1] == '.' && name[2] == '\0')) )
    /* Make sure that DIR is an actual directory. */
    if (S_ISDIR (dir->nn_stat.st_mode))
    {
      if (name[1] == '.')
        *np = dir->nn->dir;
      else
        *np = dir;
    }
    else
    {
      *np = NULL;
      err = ENOTDIR;
    }
  else
    /* Regular nodes. */
    err = backend.lookup_node (np, dir, name);

  /* Create a reference to the node (and lock it); unlock DIR. */
  if (!err && *np)
  {
    if (*np != dir)
      mutex_lock (&(*np)->lock);

    debug (("Node %s: %i references", name, (*np)->references));
    netfs_nref (*np);
  }

  mutex_unlock (&dir->lock);

  return err;
}

/* The user must define this function.  Read the contents of locked
   node NP (a symlink), for USER, into BUF.  */
error_t
netfs_attempt_readlink (struct iouser *user, struct node *np,
			char *buf)
{
  if ((!buf) || (!np->nn->symlink))
    return EAGAIN;	/* This should never happen. */
  
  strcpy (buf, np->nn->symlink);

  return 0;
}

/* The user must define this function. Locked node NP is being opened
   by USER, with FLAGS.  NEWNODE is nonzero if we just created this
   node.  Return an error if we should not permit the open to complete
   because of a permission restriction.  */
error_t
netfs_check_open_permissions (struct iouser *user, struct node *np,
			      int flags, int newnode)
{
  error_t err = OWNERSHIP (np, user);

  if (! err && (flags & O_READ))
    err = fshelp_access (&np->nn_stat, S_IREAD, user);
  if (! err && (flags & O_WRITE))
    err = fshelp_access (&np->nn_stat, S_IWRITE, user);
  if (! err && (flags & O_EXEC))
    err = fshelp_access (&np->nn_stat, S_IEXEC, user);

  return err;
}

/* The user must define this function.  Read from the locked file NP
   for user CRED starting at OFFSET and continuing for up to *LEN
   bytes.  Put the data at DATA.  Set *LEN to the amount successfully
   read upon return.  */
error_t
netfs_attempt_read (struct iouser *cred, struct node *np,
			    loff_t offset, size_t *len, void *data)
{
  return backend.read_node (np, offset, len, data);
}
  

/* The user must define this function.  Write to the locked file NP
   for user CRED starting at OFSET and continuing for up to *LEN bytes
   from DATA.  Set *LEN to the amount successfully written upon
   return.  */
error_t
netfs_attempt_write (struct iouser *cred, struct node *np,
			     loff_t offset, size_t *len, void *data)
{
  if (! backend.write_node)
    return EROFS;
  else
    return backend.write_node (np, offset, len, data);
}

/* The user must define this function.  Return the valid access
   types (bitwise OR of O_READ, O_WRITE, and O_EXEC) in *TYPES for
   locked file NODE and user CRED.  */
error_t
netfs_report_access (struct iouser *cred, struct node *node,
			     int *types)
{
  error_t err = OWNERSHIP (node, cred);

  /* FIXME: For a ro-fs, this should only set TYPES to O_READ.  */
  if (! err)
    {
      *types = 0;
      if (fshelp_access (&node->nn_stat, S_IREAD, cred) == 0)
	*types |= O_READ;
      if (fshelp_access (&node->nn_stat, S_IWRITE, cred) == 0)
	*types |= O_WRITE;
      if (fshelp_access (&node->nn_stat, S_IEXEC, cred) == 0)
	*types |= O_EXEC;
    }

  return err;
}

/* The user must define this function.  Create a new user from the
   specified UID and GID arrays. */
struct iouser*
netfs_make_user (uid_t *uids, int nuids,
		 uid_t *gids, int ngids)
{
  debug (("Not implemented"));
  return NULL;
}

/* The user must define this function.  Node NODE has no more references;
   free all its associated storage. */
void
netfs_node_norefs (struct node *node)
{
  debug (("Entering"));

  backend.free_node (node);
}

/* The user must define this function.  Fill the array *DATA of size
   BUFSIZE slast  up to NENTRIES dirents from DIR (which is locked)
   starting with entry ENTRY for user CRED.  The number of entries in
   the array is stored in *AMT and the number of bytes in *DATACNT.
   If the supplied buffer is not large enough to hold the data, it
   should be grown.  */
error_t
netfs_get_dirents (struct iouser *cred, struct node *dir,
                           int entry, int nentries, char **data,
			   mach_msg_type_number_t *datacnt,
			   vm_size_t bufsize, int *amt)
{
  int           curr_entry;	/* current entry */
  static int    curr_amt;
  struct dirent* curr_dirent;
  char*         curr_datap;	/* current position in DATA */
  int           no_more = 0;	/* no more entries? */

  curr_amt = 0;
  curr_datap = *data;

  /* Start with entry ENTRY */
  backend.set_curr_dir (dir);
  no_more = backend.skip_entries (entry);

  for (curr_entry = entry;
       !no_more;
       curr_entry++)
  {
    /* No limitiation when NENTRIES==-1. */
    if ((nentries >= 0) && (curr_entry > entry+nentries))
      no_more = 1;
    else
      no_more = backend.get_next_entry(&curr_dirent);

    if (!no_more)
    {
#ifdef HIDE_FILES_NOT_OWNED
      /* FIXME: We should do something here to avoid the ENOENT
         during a dir_lookup () after a dir_readdir ().  */
#endif
      curr_amt++;

      /* Grow the buffer pointed to by DATA if necessary. */
      if (((curr_datap - *data) + curr_dirent->d_reclen) > bufsize)
      {
        void* newdata;
	size_t prev_size = bufsize;

	/* Makes BUFSIZE a multiple of VM_PAGE_SIZE or double it. */
	if (!bufsize)
	  bufsize = vm_page_size;
	else
	  bufsize = (bufsize%vm_page_size) ?
		     ((bufsize/vm_page_size)+vm_page_size):
		     (bufsize*2);

	newdata = mmap (0, bufsize, PROT_READ|PROT_WRITE,
	    MAP_ANONYMOUS, 0, 0);
	assert (newdata != (void*)-1);
	assert (newdata != NULL);
		    
	if (newdata != *data)
	{
	  size_t s = curr_datap - *data;
	  memcpy (newdata, (void*) *data, s);
	  curr_datap = (char*)newdata + s;

	  munmap (*data, prev_size);
	  *data = (char*)newdata;
	}
      }
      assert (*data != NULL);

      /* Copy CURR_DIRENT into DATA. */
      memcpy (curr_datap, curr_dirent, curr_dirent->d_reclen);
      curr_datap += curr_dirent->d_reclen;
      munmap (curr_dirent, curr_dirent->d_reclen);
    }
  }
  
#if 0
  /* Deallocate if necessary. */
  if (bufsize > (curr_datap - *data))
    munmap (curr_datap, bufsize - (curr_datap - *data));
#endif

  /* Return */
  *amt = curr_amt;
  *datacnt = curr_datap - *data;

  return 0;
}

/* The user may define this function.  For a full description,
   see hurd/hurd_types.h.  The default response indicates a network
   store.  If the supplied buffers are not large enough, they should
   be grown as necessary.  NP is locked.  */
error_t
netfs_file_get_storage_info (struct iouser *cred,
    				     struct node *np,
				     mach_port_t **ports,
				     mach_msg_type_name_t *ports_type,
				     mach_msg_type_number_t *num_ports,
				     int **ints,
				     mach_msg_type_number_t *num_ints,
				     loff_t **offsets,
				     mach_msg_type_number_t *num_offsets,
				     char **data,
				     mach_msg_type_number_t *data_len)
{
#ifdef DEBUG
  error (0, 0, __FUNCTION__);
#endif
  return EOPNOTSUPP;
}

/* The user must define this function.  Make sure that NP->nn_stat is
   filled with the most current information.  CRED identifies the user
   responsible for the operation. NP is locked.  */
error_t
netfs_validate_stat (struct node *np, struct iouser *cred)
{
  return OWNERSHIP (np, cred);
}

/* The user must define this function.  This should attempt a utimes
   call for the user specified by CRED on locked node NP, to change
   the atime to ATIME and the mtime to MTIME.  If ATIME or MTIME is
   null, then set to the current time.  */
error_t
netfs_attempt_utimes (struct iouser *cred, struct node *np,
			      struct timespec *atime, struct timespec *mtime)
{
  error_t err = 0;

  if (backend.change_stat)
  {
    int flags = TOUCH_CTIME;
    io_statbuf_t st = np->nn_stat;

    err = fshelp_isowner (&np->nn_stat, cred);

    if (! err)
      {
	if (atime)
	  {
	    st.st_atime = atime->tv_sec;
	    st.st_atime_usec = atime->tv_nsec / 1000;
	  }
	else
	  flags |= TOUCH_ATIME;

	if (mtime)
	  {
	    st.st_mtime = mtime->tv_sec;
	    st.st_mtime_usec = mtime->tv_nsec / 1000;
	  }
	else
	  flags |= TOUCH_MTIME;

	err = backend.change_stat (np, &st);
      }
  }
  else
    err = EROFS;

  return err;
}

/* The user must define this function.  This should attempt to set the
   size of the locked file NP (for user CRED) to SIZE bytes long.  */
error_t
netfs_attempt_set_size (struct iouser *cred, struct node *np,
			loff_t size)
{
  error_t err = 0;

  if (backend.change_stat)
  {
    io_statbuf_t st = np->nn_stat;
    st.st_size = size;
    backend.change_stat (np, &st);
  }
  else
    err = EROFS;

  return err;
}

/* The user must define this function.  This should attempt to fetch
   filesystem status information for the remote filesystem, for the
   user CRED. NP is locked.  */
error_t
netfs_attempt_statfs (struct iouser *cred, struct node *np,
		      fsys_statfsbuf_t *st)
{
#ifdef DEBUG
  error (0, 0, __FUNCTION__);
#endif
  return EOPNOTSUPP;
}

/* The user must define this function.  This should sync the locked
   file NP completely to disk, for the user CRED.  If WAIT is set,
   return only after the sync is completely finished.  */
error_t
netfs_attempt_sync (struct iouser *cred, struct node *np,
			    int wait)
{
#ifdef DEBUG
  error (0, 0, __FUNCTION__);
#endif
  return EOPNOTSUPP;
}

/* The user must define this function.  This should sync the entire
   remote filesystem.  If WAIT is set, return only after the sync is
   completely finished.  */
error_t
netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  error_t err = 0;

  if (backend.sync_fs)
  {
    if (cred)
    {
      err = fshelp_isowner (&netfs_root_node->nn_stat, cred);
      if (err)
        return err;
      err = backend.sync_fs (wait);
    }
    else
      /* From libnetfs source code, CRED is set to zero in the fsys-goaway
	 stub, so we call go_away () here.  */
      if (backend.go_away)
	err = backend.go_away (); /* This should call sync_fs () */
      else
        err = backend.sync_fs (wait);
  }
  else
    err = EOPNOTSUPP;

  return err;
}

/* The user may define this function.  Attempt to set the passive
   translator record for FILE to ARGZ (of length ARGZLEN) for user
   CRED. NP is locked.  */
error_t
netfs_set_translator (struct iouser *cred, struct node *np,
    char *argz, size_t argzlen)
{
  return EOPNOTSUPP;
}

/* The user may define this function (but should define it together
   with netfs_set_translator).  For locked node NODE with S_IPTRANS
   set in its mode, look up the name of its translator.  Store the
   name into newly malloced storage, and return it in *ARGZ; set
   *ARGZ_LEN to the total length.  */
error_t
netfs_get_translator (struct node *node, char **argz,
			      size_t *argz_len)
{
  *argz_len = 0;
  *argz = (char*)malloc (sizeof (char));
  (*argz)[0] = '\0';

  return 0;
}

/* The user must define this function.  This should attempt a chmod
   call for the user specified by CRED on locked node NP, to change
   the owner to UID and the group to GID.  */
error_t
netfs_attempt_chown (struct iouser *cred, struct node *np,
			     uid_t uid, uid_t gid)
{
  error_t err = 0;

  if (backend.change_stat)
  {
    io_statbuf_t st;

    err = fshelp_isowner (&np->nn_stat, cred);

    if (! err)
    {
      st = np->nn_stat;
      st.st_uid = uid;
      st.st_gid = gid;
      err = backend.change_stat (np, &st);
    }
  }
  else
    err = EROFS;

  return err;
}

/* The user must define this function.  This should attempt a chauthor
   call for the user specified by CRED on locked node NP, thereby
   changing the author to AUTHOR.  */
error_t
netfs_attempt_chauthor (struct iouser *cred, struct node *np,
				uid_t author)
{
  error_t err = 0;

  if (backend.change_stat)
  {
    io_statbuf_t st;

    err = fshelp_isowner (&np->nn_stat, cred);

    if (! err)
    {
      st = np->nn_stat;
      st.st_author = author;
      err = backend.change_stat (np, &st);
    }
  }
  else
    err = EROFS;

  return err;
}

/* The user must define this function.  This should attempt a chmod
   call for the user specified by CRED on locked node NODE, to change
   the mode to MODE.  Unlike the normal Unix and Hurd meaning of
   chmod, this function is also used to attempt to change files into
   other types.  If such a transition is attempted which is
   impossible, then return EOPNOTSUPP.  */
error_t
netfs_attempt_chmod (struct iouser *cred, struct node *np,
		     mode_t mode)
{
  error_t err = 0;

  if (backend.change_stat)
  {
    io_statbuf_t st;

    err = fshelp_isowner (&np->nn_stat, cred);

    if (! err)
    {
      st = np->nn_stat;

      if (mode & S_IFMT)
      {
        /* User wants to change file type, check whether this is possible */
        if (S_ISDIR (st.st_mode) || S_ISDIR (mode))
        {
          /* Any->Dir and Dir->Any are forbidden transitions */
          if ((st.st_mode & S_IFMT) != (mode & S_IFMT))
	    err = EOPNOTSUPP;
	}
        else
	  /* Let him do it */
	  st.st_mode = 0;
      }
      else
	/* Only clear the permission bits */
	st.st_mode &= ~S_ISPARE;

      if (!err)
      {
	st.st_mode |= mode;
	err = backend.change_stat (np, &st);
      }
    }
  }
  else
    err = EROFS;

  return err;
}

/* The user must define this function.  Attempt to turn locked node NP
   (user CRED) into a symlink with target NAME.  */
error_t
netfs_attempt_mksymlink (struct iouser *cred, struct node *np,
			 char *name)
{
  error_t err;

  if (backend.symlink_node)
  {
    err = fshelp_isowner (&np->nn_stat, cred);
    /* FIXME: Call fshelp_access () ?! */

    if (! err)
      err = backend.symlink_node (np, name);
  }
  else
    err = EOPNOTSUPP;

  return err;
}

/* The user must define this function.  Attempt to turn NODE (user
   CRED) into a device.  TYPE is either S_IFBLK or S_IFCHR.  NP is
   locked.  */
error_t
netfs_attempt_mkdev (struct iouser *cred, struct node *np,
		     mode_t type, dev_t indexes)
{
  error_t err;

  if (backend.mkdev_node)
  {
    err = fshelp_isowner (&np->nn_stat, cred); /* FIXME: See above */

    if (! err)
      err = backend.mkdev_node (np, type, indexes);
  }
  else
    err = EOPNOTSUPP;

  return err;
}

/* The user must define this function.  This should attempt a chflags
   call for the user specified by CRED on locked node NP, to change
   the flags to FLAGS.  */
error_t
netfs_attempt_chflags (struct iouser *cred, struct node *np,
			       int flags)
{
  debug (("Not implemented"));
  return EOPNOTSUPP;
}

/* The user must define this function.  Delete NAME in DIR (which is
   locked) for USER.  */
error_t
netfs_attempt_unlink (struct iouser *user, struct node *dir,
		      char *name)
{
  error_t err;

  if (backend.unlink_node)
  {
    struct node *node;

    err = backend.lookup_node (&node, dir, name);
    //usleep (500); /* FIXME!!! The incredible race condition! */

    if (!err)
    {
      mutex_lock (&node->lock);

      debug (("Node %s: %i references", name, node->references));
      err = fshelp_isowner (&node->nn_stat, user);

      if (!err)
	err = backend.unlink_node (node);

      mutex_unlock (&node->lock);
    }
  }
  else
    err = EROFS;

  return err;
}

/* The user must define this function.  Attempt to rename the
   directory FROMDIR to TODIR. Note that neither of the specific nodes
   are locked.  */
error_t
netfs_attempt_rename (struct iouser *user, struct node *fromdir,
			      char *fromname, struct node *todir, 
			      char *toname, int excl)
{
  debug (("FIXME: Not implemented"));
  return EOPNOTSUPP;
}

/* The user must define this function.  Attempt to create a new
   directory named NAME in DIR (which is locked) for USER with mode
   MODE. */
error_t
netfs_attempt_mkdir (struct iouser *user, struct node *dir,
		     char *name, mode_t mode)
{
  error_t err = fshelp_isowner (&dir->nn_stat, user);
  struct node *newdir;

  if (!backend.create_node)
    err = EROFS;
  else
    err = backend.create_node (&newdir, dir, name, mode);

  return err;
}


/* The user must define this function.  Attempt to remove directory
   named NAME in DIR (which is locked) for USER.  */
error_t
netfs_attempt_rmdir (struct iouser *user, struct node *dir, char *name)
{
  /* Simply redirect the call */
  return netfs_attempt_unlink (user, dir, name);
}


/* The user must define this function.  Create a link in DIR with name
   NAME to FILE for USER. Note that neither DIR nor FILE are
   locked. If EXCL is set, do not delete the target.  Return EEXIST if
   NAME is already found in DIR.  */
error_t
netfs_attempt_link (struct iouser *user, struct node *dir,
		    struct node *file, char *name, int excl)
{
  error_t err;

  if (backend.link_node)
  {
    err = fshelp_isowner (&dir->nn_stat, user);
    if (!err)
      err = backend.link_node (dir, file, name, excl);
  }
  else
    err = EROFS;

  return err;
}


/* The user must define this function.  Attempt to create an anonymous
   file related to DIR (which is locked) for USER with MODE.  Set *NP
   to the returned file upon success. No matter what, unlock DIR.  */
error_t
netfs_attempt_mkfile (struct iouser *user, struct node *dir,
		      mode_t mode, struct node **np)
{
  /* Redirect the call */
  return netfs_attempt_create_file (user, dir, NULL, mode, np);
}


/* The user must define this function.  Attempt to create a file named
   NAME in DIR (which is locked) for USER with MODE.  Set *NP to the
   new node upon return.  On any error, clear *NP.  *NP should be
   locked on success; no matter what, unlock DIR before returning.  */
error_t
netfs_attempt_create_file (struct iouser *user, struct node *dir,
			   char *name, mode_t mode, struct node **np)
{
  error_t err = fshelp_isowner (&dir->nn_stat, user);

  if (!backend.create_node)
  {
    err = EROFS;
    *np = NULL;
  }
  else
  {
    /* Note: create_node () must handle nameless node creation
       (see netfs_attempt_mkfile ()).  */
    err = backend.create_node (np, dir, name, mode);

    /* Lock the new node and add a reference to it on success.  */
    if (!err && *np)
    {
      debug (("Node %s: %i references", name, (*np)->references));
      mutex_lock (&(*np)->lock);
      netfs_nref (*np);
    }
  }

  mutex_unlock (&dir->lock);

  return err;
}

/* Append to the malloced string *ARGZ of length *ARGZ_LEN a NUL-separated
   list of the arguments to this translator.  The default definition of this
   routine simply calls netfs_append_std_options.  */
error_t
netfs_append_args (char **argz, unsigned *argz_len)
{
  error_t err = 0;

  if (backend.get_args)
    err = backend.get_args (argz, argz_len);

  return err;
}

/* Parse and execute the runtime options in ARGZ & ARGZ_LEN.  EINVAL is
   returned if some option is unrecognized.  The default definition of this
   routine will parse them using NETFS_RUNTIME_ARGP. */
error_t
netfs_set_options (char *argz, size_t argz_len)
{
  error_t err = EINVAL;

  if (backend.set_options)
    err = backend.set_options (argz, argz_len);

  return err;
}


/* We need a particular syncfs stub that doesn't lock the NODE
   which was passed to file_syncfs () since we want to lock it
   in tarfs_sync_fs ().  */
error_t
netfs_S_file_syncfs (struct protid *user,
		     int wait,
		     int dochildren)
{
  error_t err;
  
  if (!user)
    return EOPNOTSUPP;
  
  err = netfs_attempt_syncfs (user->user, wait);

  return err;
}

/* The following stub has been added as a reminder.  */
#if 0
error_t
netfs_S_io_map (struct protid *user, 
		mach_port_t *rdobj, mach_msg_type_name_t *rdobjtype,
		mach_port_t *wrobj, mach_msg_type_name_t *wrobjtype)
{
  error (0, 0, "Warning: io_map () not supported");
  return EOPNOTSUPP;
}
#endif
