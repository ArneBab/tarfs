# Makefile for the Hurd Tarfs translator.
# Copyright (C) 2002, 2003  Ludovic Courtès <ludo@chbouib.org>

# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2, or (at
# your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


CC      = gcc
INSTALL = install # cp
CFLAGS  = -D_GNU_SOURCE -Wall -g -D_FILE_OFFSET_BITS=64
CFLAGS += -DDEBUG # tarfs.c debugging
#CFLAGS += -DDEBUG_FS # fs.c debugging
CFLAGS += -DDEBUG_ZIP # zip stores debugging

# Uncomment the line below to get a fs that shows file *only* to their owner.
#CFLAGS += -DHIDE_FILES_NOT_OWNED 

# Note: -lz has to be first otherwise inflate() will be the exec server's
#       inflate function
LDFLAGS = -L~ -lz -L. -lnetfs -lfshelp -liohelp -lports \
          -lihash -lshouldbeinlibc -lthreads -lstore -lbz2 #-lpthread
CTAGS   = ctags

SRC     = main.c netfs.c tarfs.c tarlist.c fs.c cache.c tar.c names.c \
          store-bzip2.c store-gzip.c debug.c

OBJ     = $(SRC:%.c=%.o)

TRANS   = tarfs

HURD    = /hurd

# Name of the test node
TNODE   = t

all: $(TRANS) # start-trans

$(TRANS): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

tags: $(SRC)
	$(CTAGS) $(SRC)

start-trans: $(TRANS)
	settrans -ac $(TNODE) ./$(TRANS)

stop-trans:
	settrans -fg $(TNODE)

install: $(TRANS)
	$(INSTALL) -m 555 $(TRANS) $(HURD)

clean:
	-rm -f $(TRANS) $(OBJ) $(TNODE) tags core
