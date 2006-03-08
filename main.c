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

#include <hurd.h>
#include <hurd/netfs.h>
#include <hurd/paths.h>
#include <argp.h>
#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <maptime.h>

#include "backend.h"

/* Choose the right backend here. */
extern struct fs_backend tarfs_backend;
struct fs_backend backend;

/* The underlying node.  */
mach_port_t ul_node;

/* Has to be defined for libnetfs...  */
int netfs_maxsymlinks = 2;

/* Main.  */
int
main (int argc, char **argv)
{
  struct argp fs_argp;
  mach_port_t bootstrap_port;
  struct iouser *user;
  error_t err;

  /* Defaults to tarfs. */
  backend = tarfs_backend;
  
  backend.get_argp (&fs_argp);
  argp_parse (&fs_argp, argc, argv, 0, 0, 0);

  task_get_bootstrap_port (mach_task_self (), &bootstrap_port);

  /* Init netfs, the root_node and the backend, */
  netfs_init ();
  err = iohelp_create_simple_iouser (&user, getuid (), getgid ());
  if (err)
    error (1, err, "Cannot create iouser");

  err = backend.init (&netfs_root_node, user);
  if (err)
    error (EXIT_FAILURE, err, "cannot create root node");
  ul_node = netfs_startup (bootstrap_port, 0);

  for (;;)
    netfs_server_loop ();

  /* Never reached.  */
  exit (0);
}
