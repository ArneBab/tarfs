/* Gzip store backend.

   Copyright (C) 1995,96,97,99,2000,01, 02 Free Software Foundation, Inc.
   Written by Ludovic Courtes <ludovic.courtes@utbm.fr>
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
#include <zlib.h>
#include <error.h>

#include <hurd.h>
#include <hurd/store.h>

#include "zipstores.h"

#ifndef DEBUG_ZIP
# undef DEBUG
#endif
#include "debug.h"

/* A simple gzip header, inspired by zlib's gzio:check_header ().  */
struct gzip_header
{
  /* Regular header: 10 bytes */
  char magic[2];
  char method;		/* Compression method */
  char flags;
  char unused[6];	/* time, xflags and OS code */
};

#define GZIP_HEADER_SIZE  (sizeof (struct gzip_header))

/* Gzip magic header */
static char gzip_magic[2] = { 0x1f, 0x8b };

/* Gzip flag byte */
#define ASCII_FLAG   0x01 /* bit 0 set: file probably ascii text */
#define HEAD_CRC     0x02 /* bit 1 set: header CRC present */
#define EXTRA_FIELD  0x04 /* bit 2 set: extra field present */
#define ORIG_NAME    0x08 /* bit 3 set: original file name present */
#define COMMENT      0x10 /* bit 4 set: file comment present */
#define RESERVED     0xE0 /* bits 5..7: reserved */


static inline error_t gzip_error (z_stream *stream, int zerr);

static error_t gzip_read_header (struct store *store,
				 store_offset_t *end_of_header,
				 struct gzip_header *hdr);

static error_t gzip_write_header (error_t (* write) (char *buf, size_t amount));

static error_t gzip_verify_crc (z_stream *stream, uLong crc);

static error_t gzip_write_suffix (z_stream *stream, uLong crc,
				  error_t (* write) (char *buf, size_t amount));


/* The following macros are defined to be then used by the zip store generic
   code included below.  */
#define ZIP_TYPE  gzip

#define ZIP_DECOMPRESS(Stream)       inflate ((Stream), Z_SYNC_FLUSH)

/* windowBits is passed < 0 to tell that there is no zlib header.
   Note that in this case inflate *requires* an extra "dummy" byte
   after the compressed stream in order to complete decompression and
   return Z_STREAM_END. Here the gzip CRC32 ensures that 4 bytes are
   present after the compressed stream.  */
#define ZIP_DECOMPRESS_INIT(Stream)  inflateInit2 ((Stream), -MAX_WBITS)

#define ZIP_DECOMPRESS_END(Stream)   inflateEnd   ((Stream))

#define ZIP_DECOMPRESS_RESET(Stream) inflateReset ((Stream))

#define ZIP_COMPRESS(Stream)         deflate ((Stream), Z_NO_FLUSH)

#define ZIP_COMPRESS_FINISH(Stream)  deflate ((Stream), Z_FINISH)

/* windowBits is passed < 0 to suppress zlib header */
#define ZIP_COMPRESS_INIT(Stream)    deflateInit2 ((Stream), \
						    Z_DEFAULT_COMPRESSION, \
						    Z_DEFLATED, \
						    -MAX_WBITS, \
						    8, \
						    Z_DEFAULT_STRATEGY)

#define ZIP_COMPRESS_END(Stream)     deflateEnd ((Stream))

#define ZIP_CRC_UPDATE(Crc, Buf, Len) crc32 (Crc, Buf, Len)

#define ZIP_CRC_VERIFY(Stream, Crc)   gzip_verify_crc (Stream, Crc)

/* Zlib constants */
#define ZIP_HAS_HEADER
#define ZIP_STREAM                   z_stream
#define ZIP_STREAM_END               Z_STREAM_END

#include "zipstores.c"


/* Convert a zlib error into a libc error.  */
static inline error_t
gzip_error (z_stream *stream, int zerr)
{
  error_t err;

  /* Z_BUF_ERROR should not happen since we try to always maintain
     the `next_in' and `next_out' buffers up-to-date.  */
  assert (zerr != Z_BUF_ERROR);

  if (stream->msg)
  {
    error (1, 0, "zlib error: %s", stream->msg);
    free (stream->msg);
    stream->msg = NULL;
  }

  switch (zerr)
  {
    case Z_OK:
      return 0;
    case Z_ERRNO:
      err = errno;
      break;
    case Z_MEM_ERROR:
      err = ENOMEM;
      break;
    case Z_VERSION_ERROR:
      err = EFTYPE;
      break;
    case Z_STREAM_ERROR:
      err = EINVAL;
      break;
    case Z_DATA_ERROR:
    case Z_STREAM_END:
      err = EIO;
      break;
    default:
      err = EAGAIN;	/* Shoudn't happen */
  }

  debug (("zlib error: %s", zError (zerr)));

  return err;
}


/* Looks for a gzip header in STORE, starting at its beginning.
   Returns the position of the first byte available after the header
   in END_OF_HEADER, and returns the header read in HEADER.  */
static error_t
gzip_read_header (struct store *store,
                  store_offset_t *end_of_header, struct gzip_header *hdr)
{
  error_t err;
  char buf[ZIP_BUFSIZE];
  char *p = buf;
  size_t amount, index = 0;

  /* Load next block and continue */
  inline
  error_t read_next ()
  {
    error_t err;
    size_t len;
    err = store_simple_read (store, index * ZIP_BUFSIZE,
			     ZIP_BUFSIZE, buf, &len);
    if (err)
      return err;

    index++;

    if (len != ZIP_BUFSIZE)
      return EIO;

    p = buf;

    return err;
  }

  /* Reads from STORE.  */
  err = store_simple_read (store, 0, ZIP_BUFSIZE, buf, &amount);
  if (err)
    return err;

  p += GZIP_HEADER_SIZE;
  memcpy (hdr, buf, GZIP_HEADER_SIZE);
  *end_of_header = GZIP_HEADER_SIZE;

  debug (("Gzip compression method: 0x%02x", hdr->method));
  
  /* Make sure this is a gzip header.  */
  if (strncmp (hdr->magic, gzip_magic, sizeof (gzip_magic)))
  {
    error (0, 0, "Invalid gzip header");
    return EFTYPE;
  }

  /* Parse the header and skip unused information.  */
  if (hdr->method != Z_DEFLATED || (hdr->flags & RESERVED) != 0)
    {
      return EIO;			/* Z_DATA_ERROR */
    }

  if ((hdr->flags & EXTRA_FIELD) != 0)
    {				/* skip the extra field */
      size_t size = 0;
      size = *(p++);
      size += (*(p++)) << 8;
      debug (("gzip extra field size: %u", size));
      /* size is garbage if EOF but the loop below will quit anyway */
      while (size--)
	if ((p++) - buf >= ZIP_BUFSIZE)
	{
	  err = read_next ();
	  if (err)
	    return err;
	}
    }
  if ((hdr->flags & ORIG_NAME) != 0)
    {				/* skip the original file name */
      debug (("gzip origname: %s", p)); /* XXX: might segfault */
      while (*p)
	if ((p++) - buf >= ZIP_BUFSIZE)
	{
	  err = read_next ();
	  if (err)
	    return err;
	}

      p++;
    }
  if ((hdr->flags & COMMENT) != 0)
    {				/* skip the .gz file comment */
      debug (("gzip comment: %s", p)); /* XXX: might segfault */
      while (*p)
	if ((p++) - buf >= ZIP_BUFSIZE)
	{
	  err = read_next ();
	  if (err)
	    return err;
	}

      p++;
    }
  if ((hdr->flags & HEAD_CRC) != 0)
    {				/* skip the header crc */
      p += 2;
    }

  *end_of_header = p - buf + (index * ZIP_BUFSIZE);

  return 0;
}

/* Compute a CRC and compare it with the last 4 bytes of the gzip file.  */
static error_t
gzip_verify_crc (z_stream *stream, uLong crc)
{
  error_t err = 0;
  uLong read_crc; /* The crc that we read from file */
  Bytef *buf = stream->next_in;

  if (stream->avail_in < 4)
  {
    error (0, 0, "Unexpected end of gzip file (no CRC)");
    return EIO;
  }

  /* Check CRC first */
  read_crc = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
  if (read_crc != crc)
  {
    debug (("Invalid CRC: 0x%lx instead of 0x%lx", read_crc, crc));
    error (0, 0, "Invalid gzip CRC");
    err = EIO;
  }
  else
    debug (("Valid gzip CRC"));

  /* Check uncompressed stream length */
  stream->next_in += 4, stream->avail_in -= 4,
  buf = stream->next_in;
  if (stream->avail_in < 4)
  {
    error (0, 0, "Unexpected end of gzip file (no length)");
    return EIO;
  }

  read_crc = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
  if (read_crc != stream->total_out)
  {
    debug (("Got length=%lu instead of %lu", read_crc, stream->total_out));
    error (0, 0, "Invalid gzip file length");
    err = EIO;
  }
  else
    debug (("Valid gzip uncompressed stream size (%lu)", read_crc));

  stream->next_in += 4, stream->avail_in -= 4;
  if (stream->avail_in > 0)
  {
    debug (("%u bytes left", stream->avail_in));
    error (0, 0, "Trailing characters at end of file");
  }

  return err;
}

/* Write a gzip suffix: CRC (4 bytes) and uncompressed stream length
   (4 bytes).  WRITE is called to actually write the suffix.  Assume that
   STREAM is opened for compression.  */
static error_t
gzip_write_suffix (z_stream *stream, uLong crc,
                   error_t (* write) (char *buf, size_t amount))
{
  char buf[8];
  size_t total = stream->total_in;

  buf[0] = (crc & 0xff);
  buf[1] = (crc >>  8) & 0xff;
  buf[2] = (crc >> 16) & 0xff;
  buf[3] = (crc >> 24) & 0xff;
  buf[4] = (total & 0xff);
  buf[5] = (total >>  8) & 0xff;
  buf[6] = (total >> 16) & 0xff;
  buf[7] = (total >> 24) & 0xff;

  return write (buf, 8);
}

/* Write a simple gzip header.  WRITE is the method called to actually
   write the header.  Note: The header format being almost
   completely undocumented: FIXME.  */
static error_t
gzip_write_header (error_t (* write) (char *buf, size_t amount))
{
  struct gzip_header hdr;

  bzero (&hdr, GZIP_HEADER_SIZE);
  hdr.magic[0] = gzip_magic[0];
  hdr.magic[1] = gzip_magic[1];
  hdr.method   = Z_DEFLATED;

  return write ((char *)&hdr, GZIP_HEADER_SIZE);
}
