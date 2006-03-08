/* Gzip store backend.

   Copyright (C) 1995,96,97,99,2000,01, 02 Free Software Foundation, Inc.
   Written by Ludovic Courtes <ludo@chbouib.org>
   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111, USA. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <bzlib.h>
#include <error.h>

#include <hurd.h>
#include <hurd/store.h>

#include "zipstores.h"

#ifndef DEBUG_ZIP
# undef DEBUG
#endif
#include "debug.h"



/* Convert a bzlib error into a libc error.  */
static inline error_t
bzip2_error (bz_stream *stream, int zerr)
{
  error_t err;

  assert (zerr != BZ_OUTBUFF_FULL);

  switch (zerr)
  {
    case BZ_OK:
    case BZ_RUN_OK:
    case BZ_FLUSH_OK:
    case BZ_FINISH_OK:
      err = 0;
      break;
    case BZ_MEM_ERROR:
      err = ENOMEM;
      break;
    case BZ_CONFIG_ERROR:
      err = EFTYPE;
      break;
    case BZ_DATA_ERROR:
    case BZ_DATA_ERROR_MAGIC:
    case BZ_IO_ERROR:
      err = EIO;
      break;
    case BZ_PARAM_ERROR:
      err = EINVAL;
      break;
    case BZ_SEQUENCE_ERROR:
      err = EINVAL;
      break;
    default:
      err = EAGAIN;	/* Shoudn't happen */
  }

  return err;
}

/* The following macros are defined to be then used by the zip store generic
   code included below.  */
#define ZIP_TYPE  bzip2

#define ZIP_DECOMPRESS(Stream)       BZ2_bzDecompress ((Stream))

#define ZIP_DECOMPRESS_INIT(Stream)  BZ2_bzDecompressInit ((Stream), 1, 0)

#define ZIP_DECOMPRESS_END(Stream)   BZ2_bzDecompressEnd ((Stream));

#define ZIP_DECOMPRESS_RESET(Stream) \
   ZIP_DECOMPRESS_END ((Stream)), ZIP_DECOMPRESS_INIT ((Stream))

#define ZIP_COMPRESS(Stream)         BZ2_bzCompress ((Stream), BZ_RUN)

#define ZIP_COMPRESS_FINISH(Stream)  BZ2_bzCompress ((Stream), BZ_FINISH)

#define ZIP_COMPRESS_INIT(Stream)    BZ2_bzCompressInit ((Stream), 4, 1, 0)

#define ZIP_COMPRESS_END(Stream)     BZ2_bzCompressEnd ((Stream))

/* Constants */
#define ZIP_STREAM                   bz_stream
#define ZIP_STREAM_END               BZ_STREAM_END

#include "zipstores.c"
