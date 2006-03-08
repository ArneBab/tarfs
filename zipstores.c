/* Compression store backend.

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

#ifndef ZIP_TYPE
# error "Don't try to compile this file directly."
#endif

/* Stringification macros stolen from libstore's unzipstores.c */

#define STRINGIFY(name) STRINGIFY_1(name)
#define STRINGIFY_1(name) #name

#define ZIP(name)               ZIP_1 (ZIP_TYPE, name)
#define ZIP_1(zip, name)        ZIP_2 (zip, name)
#define ZIP_2(zip, name)        zip ## _ ## name

#define STORE_ZIP(name)		STORE_ZIP_1 (ZIP_TYPE, name)
#define STORE_ZIP_1(zip,name)	STORE_ZIP_2 (zip, name)
#define STORE_ZIP_2(zip,name)	store_ ## zip ## _ ## name
#define STORE_STD_CLASS_1(name) STORE_STD_CLASS(name)


#ifndef ZIP_BUFSIZE_LOG2
# define ZIP_BUFSIZE_LOG2  13 /* zlib uses up to 16kb */
# define ZIP_BUFSIZE  (1 << ZIP_BUFSIZE_LOG2)
#endif

/* Copy-on-write cache size */
#define CACHE_BLOCK_SIZE_LOG2  ZIP_BUFSIZE_LOG2
#define CACHE_BLOCK_SIZE       ZIP_BUFSIZE

/* BLOCK_NUMBER gives the number in which Offset can be found
   (equivalent to Offset/CACHE_BLOCK_SIZE).  */
#define BLOCK_NUMBER(Offset) \
  ((Offset) >> CACHE_BLOCK_SIZE_LOG2)

/* BLOCK_RELATIVE_OFFSET gives the relative offset inside a block
   (equivalent to AbsoluteOffset%CACHE_BLOCK_SIZE).  */
#define BLOCK_RELATIVE_OFFSET(AbsoluteOffset) \
  ((AbsoluteOffset) & (CACHE_BLOCK_SIZE - 1))


typedef unsigned char uchar;

/* Read status */
enum status
{
  /* Status `idle' is only relevant for stream, i.e. uninitialized streams */
  STATUS_IDLE = 0,

  /* File/stream is being used */
  STATUS_RUNNING,

  /* End of file/stream has been reached */
  STATUS_EOF
};

/* Compression/decompression state */
struct stream_state
{
  /* The actual stream */
  ZIP_STREAM stream;

  /* A buffer used for input/output */
  char buf[ZIP_BUFSIZE];

  /* Current offset in the underlying store (ie. compressed stream) */
  store_offset_t file_offs;

  /* Current offset in this store (ie. uncompressed stream) */
  store_offset_t zip_offs;

  /* File and zip stream status */
  enum status file_status;
  enum status zip_status;

#if (defined ZIP_CRC_UPDATE && defined ZIP_CRC_VERIFY)
  /* CRC as used by gzip */
  uLong crc;
#endif

  /* Stream lock */
  struct mutex lock;
};

/* Zip object information */
struct ZIP (object)
{
  /* The underlying store */
  struct store *source;

  /* The store represented by this object */
  struct store *store;

#ifdef ZIP_HAS_HEADER
  /* File header (only used by gzip) */
  struct ZIP (header) header;
#endif

  /* Streams for reading and writing */
  struct stream_state read;
  struct stream_state write;

  /* Position of the compressed stream start in the underlying storage
     (ie. right after the gzip header) */
  store_offset_t start_file_offs;

  /* Original size of the uncompressed stream */
  size_t zip_orig_size;
  size_t zip_orig_blocks_size;

  /* Copy-on-write cache of the uncompressed stream */
  struct
  {
    /* Vector of cache blocks */
    char **blocks;

    /* Size of BLOCKS */
    size_t size;

    /* Cache lock */
    struct mutex lock;
  } cache;
};

#ifndef MAX
# define MAX(A,B)  ((A) < (B) ? (B) : (A))
#endif
#ifndef MIN
# define MIN(A,B)  ((A) < (B) ? (A) : (B))
#endif


/* Reads from STORE and stores the data in BUF.
   Makes sure BUF remains unchanged.  */
static inline error_t
store_simple_read (struct store *store, off_t addr,
		   size_t amount, void *buf, size_t *len)
{
  error_t err;
  void *p = buf;

  err = store_read (store, addr, amount, &p, len);
  if (err)
    return err;

  assert (*len <= amount);

  if (p != buf)
  {
    /* Copy the data back to BUF and deallocate P */
    int e;
    memcpy (buf, p, *len);
    e = munmap (p, *len);
    assert (!e);
  }

  return 0;
}

/* Writes AMOUNT bytes to STORE after enlarging STORE if necessary.  */
static inline error_t
store_simple_write (struct store *store, off_t addr, void *buf,
                    size_t len, size_t *amount)
{
  error_t err;
  size_t newsize = addr + len;

  if (newsize > store->size)
  {
    /* Enlarge the underlying store */
    err = store_set_size (store, newsize);
    if (err)
    {
      error (0, err, "Unable to set store size to %u", newsize);
      return err;
    }
  }

  err = store_write (store, addr, buf, len, amount);

  return err;
}


/* Initializes GZIP: Resets its file/zip offsets and prepare it for
   reading.  */
static error_t
ZIP (stream_read_init) (struct ZIP (object) *zip)
{
  error_t err;
  int zerr;
  ZIP_STREAM *stream = &zip->read.stream;

  mutex_lock (&zip->read.lock);

  /* Check whether STREAM had already been initialized */
  if (stream->state)
  {
    zerr = ZIP_DECOMPRESS_END (stream);
    err  = ZIP (error) (stream, zerr);
    assert_perror (err);
  }

  /* Initialize READ.STREAM for decompression.  */
  zerr = ZIP_DECOMPRESS_INIT (stream);
  err = ZIP (error) (stream, zerr);

  if (!err)
  {
    stream->next_in = stream->next_out = NULL;
    stream->avail_in = stream->avail_out = 0;

    zip->read.file_offs = zip->start_file_offs;
    zip->read.zip_offs  = 0;
    zip->read.file_status = zip->read.zip_status = STATUS_RUNNING;

#ifdef ZIP_CRC_UPDATE
    /* Initialize running CRC */
    zip->read.crc = ZIP_CRC_UPDATE (0, NULL, 0);
#endif
  }

  mutex_unlock (&zip->read.lock);

  return err;
}

/* Initializes GZIP: Resets its file/zip offsets and prepare it for
   writing.  */
static error_t
ZIP (stream_write_init) (struct ZIP (object) *zip)
{
  error_t err;
  ZIP_STREAM *stream = &zip->write.stream;
  int zerr;

  mutex_lock (&zip->write.lock);

  /* Check whether STREAM has already been initialized */
  if (stream->state)
  {
    zerr = ZIP_COMPRESS_END (stream);
    err  = ZIP (error) (stream, err);
    assert_perror (err);
  }
#ifdef ZIP_HAS_HEADER
  else
  {
    /* Check whether we need to write a gzip header */
    if (!zip->source->size)
    {
      error_t
      do_write (char *buf, size_t amount)
      {
	size_t len;
	err = store_simple_write (zip->source, 0, buf, amount, &len);
	if (!err && (len != amount))
	  err = EIO;
	zip->start_file_offs = len;
	return err;
      }

      debug (("Writing a new gzip header"));

      err = gzip_write_header (do_write);
      if (err)
        return err;
    }
  }
#endif

  /* Initialize STREAM for compression.  */
  zerr = ZIP_COMPRESS_INIT (stream);
  err = ZIP (error) (stream, zerr);

  if (!err)
  {
    stream->next_in   = NULL;
    stream->avail_in  = 0;
    stream->next_out  = zip->write.buf;
    stream->avail_out = ZIP_BUFSIZE;

    zip->write.file_offs = zip->start_file_offs;
    zip->write.zip_offs  = 0;
    zip->write.file_status = zip->write.zip_status = STATUS_RUNNING;

#ifdef ZIP_CRC_UPDATE
    /* Initialize running CRC */
    zip->write.crc = ZIP_CRC_UPDATE (0, NULL, 0);
#endif
  }

  mutex_unlock (&zip->write.lock);

  return err;
}

#ifdef DEBUG_ZIP
  /* Dumps the state of a stream */
# define DUMP_STATE() \
  { \
    if (!stream->state) \
      debug (("!stream->state")); \
    debug (("offset file/zip = %llu / %llu", \
	   *file_offs, *zip_offs));	   \
    debug (("avail in/out = %u / %u",	   \
	    stream->avail_in, stream->avail_out)); \
  }
#else
# define DUMP_STATE()
#endif

/* Directly read AMOUNT bytes from GZIP's zip stream starting at its current
   position (GZIP->READ.FILE_OFFS). Update the FILE_OFFS and GZIP_OFFS fields.
   This is the canonical way to read the stream.  */
static error_t
ZIP (stream_read) (struct ZIP (object) *const zip,
		   size_t amount, void *const buf,
		   size_t *const len)
{
  error_t err = 0;
  int zerr = 0;
  ZIP_STREAM *stream = &zip->read.stream;
  store_offset_t *zip_offs  = &zip->read.zip_offs,
	         *file_offs = &zip->read.file_offs;
  store_offset_t zip_start  = *zip_offs;


  mutex_lock (&zip->read.lock);
  assert (zip->read.zip_status != STATUS_IDLE);

  /* Check whether we have already reached the end of stream */
  if (zip->read.zip_status == STATUS_EOF)
  {
    debug (("eof: doing nothing"));
    *len = 0;
    mutex_unlock (&zip->read.lock);
    return 0;
  }

  /* Check whether the underlying file is empty */
  if (zip->source->size <= zip->start_file_offs)
  {
    *len = 0;
    zip->read.zip_status  = STATUS_EOF;
    zip->read.file_status = STATUS_EOF;
    mutex_unlock (&zip->read.lock);
    return 0;
  }

  /* Prepare for reading/decompressing */
  stream->next_out  = (uchar *) buf;
  stream->avail_out = amount;

  while (stream->avail_out != 0)
    {
      size_t avail_in, avail_out;

      if (stream->avail_in == 0)
	{
	  /* Load the compressed stream */
	  size_t read = MIN (zip->source->size - *file_offs, ZIP_BUFSIZE);
	  stream->next_in = zip->read.buf;

	  err = store_simple_read (zip->source, *file_offs,
				   read, zip->read.buf, &read);
	  if (err)
	    break;

	  stream->avail_in = read;

	  if (*file_offs + read >= zip->source->size)
	  {
	    zip->read.file_status = STATUS_EOF;
	    debug (("End of file"));
	  }
	}

      /* Inflate and update the file/zip pointers accordingly.  */
      DUMP_STATE ();
      avail_in  = stream->avail_in;
      avail_out = stream->avail_out;

      zerr = ZIP_DECOMPRESS (stream);

      *file_offs += avail_in  - stream->avail_in;
      *zip_offs  += avail_out - stream->avail_out;

      if (zerr == ZIP_STREAM_END)
      {
	zip->read.zip_status = STATUS_EOF;
	debug (("End of stream"));
	if (zip->read.file_status != STATUS_EOF)
	  error (0, 0, "Trailing characters at end of file");
	break;
      }

      err = ZIP (error) (stream, zerr);
      if (err)
	break;
    }

  *len = *zip_offs - zip_start;

#ifdef ZIP_CRC_UPDATE
  zip->read.crc = ZIP_CRC_UPDATE (zip->read.crc, buf, *len);
  if (zip->read.zip_status == STATUS_EOF)
  {
    /* Check gzip's CRC and length (4 bytes) */
    ZIP_CRC_VERIFY (stream, zip->read.crc);
  }
#endif

  debug (("requested/read = %i / %i", amount, *len));
  assert (*len <= amount);

  mutex_unlock (&zip->read.lock);

  return err;
}

/* Directly writes AMOUNT bytes from GZIP's zip stream starting at its current
   position. Update the FILE_OFFS and GZIP_OFFS fields. If FINISH is set, then
   terminate compression and close the compression stream thereafter.
   ADVERTISE is called before writing data to the underlying store in order
   to enable, e.g., caching of the region that is going to be overwritten.
   This is the canonical way to write a stream.
   (note: this function is similar to ZIP (stream_read)) */
static error_t
ZIP (stream_write) (struct ZIP (object) *const zip,
		    size_t amount, void *const buf,
		    size_t *const len, const int finish,
		    error_t (* advertise)
		      (struct ZIP (object) *zip,
		       store_offset_t offs, size_t amount))
{
  error_t err = 0;
  int zerr = 0;
  ZIP_STREAM *stream = &zip->write.stream;
  store_offset_t *zip_offs  = &zip->write.zip_offs,
		 *file_offs = &zip->write.file_offs;
  store_offset_t zip_start  = *zip_offs;

  /* Write AMOUNT bytes from BUF (compressed stream).  */
  error_t
  do_write (char *buf, size_t amount)
  {
    error_t err;
    size_t len;

    /* Advertise the region that's going to be overwritten */
    err = advertise (zip, *file_offs, amount);
    if (err)
      return err;

    err = store_simple_write (zip->source, *file_offs, buf,
		              amount, &len);
    if (!err)
      assert (len == amount);

    *file_offs += amount;

    return err;
  }


  mutex_lock (&zip->write.lock);
  assert (zip->write.zip_status != STATUS_IDLE);

  if (zip->write.zip_status == STATUS_EOF)
  {
    /* End of file reached */
    debug (("eof: doing nothing"));
    *len = 0;
    mutex_unlock (&zip->write.lock);
    return 0;
  }

  stream->next_in  = (uchar *) buf;
  stream->avail_in = amount;

  while (finish || stream->avail_in)
    {
      size_t avail_out, avail_in;

      if (stream->avail_out == 0)
	{
	  /* Flush the compressed stream */
	  err = do_write (zip->write.buf, ZIP_BUFSIZE);
	  if (err)
	    break;

	  stream->next_out  = zip->write.buf;
	  stream->avail_out = ZIP_BUFSIZE;
	}

      /* Inflate and update the file/zip pointers accordingly.  */
      DUMP_STATE ();
      avail_out = stream->avail_out;
      avail_in  = stream->avail_in;

      if (!finish)
	zerr = ZIP_COMPRESS (stream);
      else
	zerr = ZIP_COMPRESS_FINISH (stream);

      *zip_offs  += avail_in  - stream->avail_in;

      if (finish)
      {
        if (zerr == ZIP_STREAM_END)
	  /* Compression over */
	  break;
	else
	  /* Continue till there is no more pending output */
	  continue;
      }

      err = ZIP (error) (stream, zerr);
      if (err)
      {
	debug (("zerr = %s", strerror (err)));
	break;
      }
    }

  *len = *zip_offs - zip_start;

#ifdef ZIP_CRC_UPDATE
  zip->write.crc = ZIP_CRC_UPDATE (zip->write.crc, buf, *len);
#endif

  if (!err && finish)
  {
    /* Terminate */
    size_t write = ZIP_BUFSIZE - stream->avail_out;
    debug (("Flushing & terminating"));

    err = do_write (zip->write.buf, write);
    if (err)
      goto end;

    /* Close the compression stream */
    zerr = ZIP_COMPRESS_END (stream);
    err  = ZIP (error) (stream, zerr);
    assert_perror (err);

#ifdef ZIP_CRC_UPDATE
    /* Write gzip CRC (4 bytes) and total uncompressed stream size (4 bytes) */
    err = ZIP (write_suffix) (stream, zip->write.crc, do_write);
    if (err)
    {
      debug (("Failed to write suffix"));
      goto end;
    }
#endif

    debug (("Finished at file/zip: %lli / %lli", *file_offs, *zip_offs));
  }

end:
  debug (("requested/written = %i / %i", amount, *len));

  mutex_unlock (&zip->write.lock);

  return err;
}


/* Jump at offset OFFS of ZIP's raw decompression stream, without
   taking the cache data into account. When ZIP is being written, only
   forward seeks are allowed.  */
static error_t
ZIP (stream_read_seek) (struct ZIP (object) *const zip, store_offset_t offs)
{
  error_t err = 0;
  char buf[ZIP_BUFSIZE];
  const store_offset_t *zip_offs  = &zip->read.zip_offs;

  if (*zip_offs > offs)
  {
    /* Reverse seek are forbidden when writing */
    assert (zip->write.zip_status != STATUS_RUNNING);

    /* Start from the beginning */
    err = ZIP (stream_read_init) (zip);
    if (err)
      return err;
  }

  if (offs != *zip_offs)
  {
    /* Emulate a seek by reading the data before OFFS */
    size_t len;
    size_t amount;

    debug (("Seeking from "OFF_FMT" to "OFF_FMT, *zip_offs, offs));

    while (*zip_offs < offs)
    {
      /* Read from zero to BLOCK_OFFS.  */
      amount = MIN (offs - *zip_offs, ZIP_BUFSIZE);

      err = ZIP (stream_read) (zip, amount, buf, &len);
      if (err)
        return err;
      if (len < amount)
      {
        debug (("Couln't seek to %lli (got %u instead of %u)",
                 offs, len, amount));
        return EIO;
      }
    }
  }

  /* Make sure we got there.  */
  assert (*zip_offs == offs);

  return err;
}

/* Read AMOUNT bytes from STORE at offset OFFSET. Returns the number of bytes
   actually read in LEN.  */
static error_t
ZIP (read) (struct store *store,
	    store_offset_t offset, size_t index, size_t amount, void **buf,
	    size_t *len)
{
  error_t err = 0;
  struct ZIP (object) *zip = store->misc;
  
  char **blocks;
  size_t blocks_size;
  size_t block = BLOCK_NUMBER (offset);	/* 1st block to read */
  store_offset_t block_offset;
  char  *datap = *buf;	/* current pointer */
  size_t  size;

  if (offset >= store->size)
  {
    *len = 0;
    return EIO;
  }

  /* Adjust SIZE and LEN to the maximum that can be read.  */
  size = store->size - offset;
  size = (size > amount) ? amount : size;
  *len = size;

  /* Get the relative offset inside cache block BLOCK.  */
  block_offset = BLOCK_RELATIVE_OFFSET (offset);

  /* Lock the file during the whole reading (XXX: not very fine-grained) */
  mutex_lock (&zip->cache.lock);
  blocks = zip->cache.blocks;
  blocks_size = zip->cache.size;

  while (size > 0)
  {
    size_t read = (size > CACHE_BLOCK_SIZE)
                  ? (CACHE_BLOCK_SIZE - block_offset)
		  : (size);

    if ((block < blocks_size) && (blocks[block]))
      /* Read block from cache */
      memcpy (datap, &blocks[block][block_offset], read);
    else
    {
      /* Read block directly from file */
      size_t actually_read;

      err = ZIP (stream_read_seek) (zip, offset);
      assert_perror (err);

      err = ZIP (stream_read) (zip, read, datap, &actually_read);
      if (err)
        break;

      /* We should have read everything.  */
      assert (actually_read == read);
    }

    /* Go ahead with next block.  */
    block++;
    size  -= read;
    block_offset = 0;
    datap  = datap + read;
  }

  mutex_unlock (&zip->cache.lock);

  return err;
}

/* Fetches block number BLOCK from STORE and caches it.
   Cache is assumed to be locked when this is called.  */
static inline error_t
fetch_block (struct ZIP (object) *zip, size_t block)
{
  error_t err   = 0;
  char **blocks = zip->cache.blocks;
  size_t last_block = BLOCK_NUMBER (zip->zip_orig_size - 1);
  size_t read;
  size_t actually_read = 0;

  /* Don't try to go beyond the boundaries.  */
  assert (block <= last_block);

  if (blocks[block])
    /* Nothing to do */
    return 0;

  /* Allocate a new block.  */
  blocks[block] = calloc (CACHE_BLOCK_SIZE, sizeof (char));
  if (!blocks[block])
    return ENOMEM;

  /* If this is the last block, then we may have less to read.  */
  if (block == last_block)
    read = zip->zip_orig_size % CACHE_BLOCK_SIZE;
  else
    read = CACHE_BLOCK_SIZE;

  err = ZIP (stream_read_seek) (zip, block << CACHE_BLOCK_SIZE_LOG2);
  assert_perror (err);

  err = ZIP (stream_read) (zip, read, blocks[block], &actually_read);

  if (!err)
    /* We should have read everything.  */
    assert (actually_read == read);

  return err;
}

/* Write LEN bytes from BUF to STORE at offset ADDR. Returns the number of
   bytes actually written in AMOUNT.  */
static error_t
ZIP (write) (struct store *store,
	     store_offset_t offset, size_t index, const void *buf, size_t len,
	     size_t *amount)
{
  error_t err = 0;
  struct ZIP (object) *zip = store->misc;
  size_t size;
  
  char **blocks;
  int   block = BLOCK_NUMBER (offset); /* 1st block to read.  */
  const void *datap = buf; /* current pointer */

  mutex_lock (&zip->cache.lock);
  blocks = zip->cache.blocks;

  if (offset >= store->size)
  {
    debug (("Trying to write at offs %lli (size=%u)", offset, store->size));
    *amount = 0;
    mutex_unlock (&zip->cache.lock);
    return EIO;
  }

  /* Adjust SIZE and LEN to the maximum that can be read.  */
  size = store->size - offset;
  size = (size > len) ? len : size;
  *amount = size;

  /* Set OFFSET to be the relative offset inside cache block num. BLOCK.  */
  offset = BLOCK_RELATIVE_OFFSET (offset);

  while (size > 0)
  {
    size_t write = (size + offset > CACHE_BLOCK_SIZE)
                   ? (CACHE_BLOCK_SIZE - offset)
		   : (size);

    /* Allocate/fetch this block if not here yet (copy-on-write).  */
    if (!blocks[block])
    {
      if (block < zip->zip_orig_blocks_size)
      {
	/* Fetch this block */
	err = fetch_block (zip, block);
	if (err)
	  break;
      }
      else
      {
        /* Allocate a new block */
        char *b = calloc (CACHE_BLOCK_SIZE, sizeof (char));
        if (!b)
        {
          err = ENOMEM;
          break;
        }
        blocks[block] = b;
      }
    }

    /* Copy the new data into cache.  */
    memcpy (&blocks[block][offset], datap, write);

    /* Go ahead with next block.  */
    block++;
    size  -= write;
    offset = 0;
    datap  = datap + write;
  }

  mutex_unlock (&zip->cache.lock);

  return err;
}

/* Set STORE's size to SIZE (in bytes), i.e. adjust its cache size.  */
static error_t
ZIP (set_size) (struct store *store, size_t size)
{
  error_t err = 0;
  struct ZIP (object) *zip = store->misc;
  size_t *blocks_size;
  char ***blocks;
  size_t newsize, oldsize;	/* Size of BLOCKS */

  mutex_lock (&zip->cache.lock);
  blocks_size = &zip->cache.size;
  blocks      = &zip->cache.blocks;
  oldsize     = *blocks_size;
  newsize     = size ? BLOCK_NUMBER (size - 1) + 1 : 0;

  debug (("old/new size = %lli / %u", store->size, size));

  /* Check whether the cache needs to be grown */
  if (size > store->size)
  {
    if (newsize > oldsize)
    {
      /* Enlarge the block vector */
      char **newblocks;

      newblocks = realloc (*blocks, newsize * sizeof (char *));
      if (!newblocks)
        err = ENOMEM;
      else
      {
	*blocks = newblocks;

	/* Zero the new blocks but don't actually allocate them */
	bzero (&(*blocks)[*blocks_size],
	       (newsize - *blocks_size) * sizeof (char *));

	*blocks_size = newsize;
      }
    }
  }
  else
  {
    int i;

    /* Free unused cache blocks */
    for (i = newsize; i < *blocks_size; i++)
      free ((*blocks)[i]);

    /* Reduce cache vector */
    *blocks = realloc (*blocks, newsize * sizeof (char *));
    *blocks_size = newsize;
  }

  if (!err)
    store->size = store->end = store->wrap_src = store->runs[0].length = size;

  mutex_unlock (&zip->cache.lock);

  debug (("newsize is %lli (err = %s)", store->size, strerror (err)));

  return err;
}

/* Modify SOURCE to reflect those runs in RUNS, and return it in STORE.  */
error_t
ZIP (remap) (struct store *source,
	    const struct store_run *runs, size_t num_runs,
	    struct store **store)
{
  return EOPNOTSUPP;
}

error_t
ZIP (allocate_encoding) (const struct store *store, struct store_enc *enc)
{
  return EOPNOTSUPP;
}

error_t
ZIP (encode) (const struct store *store, struct store_enc *enc)
{
  return EOPNOTSUPP;
}

error_t
ZIP (decode) (struct store_enc *enc, const struct store_class *const *classes,
	     struct store **store)
{
  return EOPNOTSUPP;
}

static error_t
ZIP (validate_name) (const char *name, const struct store_class *const *classes)
{
  return 0;
}

static error_t
ZIP (map) (const struct store *store, vm_prot_t prot, mach_port_t *memobj)
{
  *memobj = MACH_PORT_NULL;
  return EOPNOTSUPP;
}


/* Traverses the whole zip store STORE and allocate its cache.
   Returns STORE's size (the uncompressed stream size) in SIZE.
   This should be called *only once* when initializing STORE.  */
static inline error_t
traverse (struct store *const store, size_t *const size)
{
  error_t err;
  struct ZIP (object) *zip = store->misc;
  size_t cache_size, total_size = 0, block = 0;
  char buf[ZIP_BUFSIZE];

  /* No need to lock the cache here since this is called from
     the open method.  */

  /* Create an arbitrary size cache for the uncompressed stream */
  cache_size = (BLOCK_NUMBER (zip->source->size) + 1) << 1;
  zip->cache.blocks = calloc (cache_size, sizeof (char *));
  if (!zip->cache.blocks)
    return ENOMEM;

  zip->cache.size = cache_size;

  /* We could cache the whole file but we don't, in order to minimize memory
     usage.  */
  while (zip->read.zip_status != STATUS_EOF)
  {
    size_t len;

    err = ZIP (stream_read) (zip, ZIP_BUFSIZE, buf, &len);
    if (err || !len)
      break;

    total_size += len;

    if (++block >= cache_size)
    {
      /* Grow the cache block vector */
      char **blocks;
      cache_size <<= 1;
      blocks = realloc (zip->cache.blocks,
			cache_size * sizeof (char *));
      if (!blocks)
      {
	err = ENOMEM;
	break;
      }

      bzero (&blocks[zip->cache.size],
             (cache_size - zip->cache.size) * sizeof (char *));

      zip->cache.size = cache_size;
      zip->cache.blocks = blocks;
    }
  }

  if (err)
  {
    debug (("error = %s", strerror (err)));
    return err;
  }

  debug (("file traversed (offset file/zip = %llu / %llu)",
          zip->read.file_offs, zip->read.zip_offs));

  *size = total_size;

  return err;
}


/* Synchronizes STORE if it's opened read-write and if there are dirty pages.
   This is our cleanup procedure which gets called *only* when the user
   calls store_free ().  */
void
ZIP (sync) (struct store *store)
{
  error_t err;
  int dirty = 0, zerr;
  struct ZIP (object) *zip = store->misc;
  ZIP_STREAM *stream = &zip->write.stream;
  char **blocks;
  size_t block;

  /* This is our ZIP (stream_write) callback. All it does is cache
     the region [OFFS, OFFS+AMOUNT] of the underlying source file
     (or at least skip it) so that we don't lose it when overwriting it.  */
  error_t
  cache_ahead (struct ZIP (object) *zip, store_offset_t offs, size_t amount)
  {
    char lostbuf[CACHE_BLOCK_SIZE];
    store_offset_t *read_foffs = &zip->read.file_offs,
                   *read_zoffs = &zip->read.zip_offs;
    enum status *read_fstatus = &zip->read.file_status;
    size_t block = BLOCK_NUMBER (*read_zoffs);
    size_t read;

    debug (("Region offs=%lli amount=%u", offs, amount));

    /* Cache and put the decompression stream beyond where we are going
       to write.  */
    while ((*read_fstatus != STATUS_EOF) &&
           (*read_foffs < offs + amount))
    {
      /* Assume the read stream is at the beginning of a block */
      assert (BLOCK_RELATIVE_OFFSET (*read_zoffs) == 0);

      debug (("At block %i (offset %lli)", block, *read_zoffs));

      if (!blocks[block])
        /* Cache this block */
        err = fetch_block (zip, block);
      else
        /* Just skip this block */
        err = ZIP (stream_read) (zip, CACHE_BLOCK_SIZE, lostbuf, &read);

      if (err)
        break;
    }

    return err;
  }


  if ((store->flags && STORE_READONLY) ||
      (store->flags && STORE_HARD_READONLY))
    /* Store opened read-only */
    goto terminate;

  /* Hold the cache lock till the end--anyway, no one should try to get
     this lock since we are called from store_free ().  */
  mutex_lock (&zip->cache.lock);
  blocks = zip->cache.blocks;

  /* Initialize STREAM since this should not have be done before.  */
  err = ZIP (stream_write_init) (zip);
  assert_perror (err);

  if (store->size != zip->zip_orig_size)
    /* Size has changed: We need to rewrite the whole file */
    dirty = 1;

  /* Look for dirty cache pages */
  for (block = 0;
       (!dirty) && (block <= BLOCK_NUMBER (store->size - 1));
       block++)
    dirty = (blocks[block] != NULL);

  if (!dirty)
    /* Nothing to do */
    goto terminate;

  /* Traverse the file and sync it */
  debug (("Syncing!"));
  err = ZIP (stream_read_init) (zip);
  assert_perror (err);

  for (block = 0;
       block <= BLOCK_NUMBER (store->size - 1);
       block++)
  {
    int end = (block == BLOCK_NUMBER (store->size - 1));
    size_t amount, len;

    amount = end ? (store->size % CACHE_BLOCK_SIZE) : CACHE_BLOCK_SIZE;

    /* Make sure we do have this block */
    if (!blocks[block])
    {
      if (block < zip->zip_orig_blocks_size)
      {
	/* Fetch this block */
	err = fetch_block (zip, block);
	if (err)
	  break;
      }
      else
      {
        /* Allocate a new (zeroed) block */
        char *b = calloc (CACHE_BLOCK_SIZE, sizeof (char));
        if (!b)
        {
          err = ENOMEM;
          break;
        }
        blocks[block] = b;
      }
    }

    /* Write the compressed stream for this block */
    err = ZIP (stream_write) (zip, amount, blocks[block],
			      &len, end, cache_ahead);
    assert_perror (err);

    free (blocks[block]);
  }

  if (zip->source->size > zip->write.file_offs)
  {
    /* Reduce the underlying store */
    err = store_set_size (zip->source, zip->write.file_offs);
    if (err)
      error (0, err, "Unable to reduce store to %lli", zip->write.file_offs);
  }

terminate:
  debug (("Size file/zip/zip_orig: %lli / %lli / %u",
          zip->source->size, store->size, zip->zip_orig_size));

  /* Deallocate everything and leave */
  zerr = ZIP_DECOMPRESS_END (&zip->read.stream);
  err  = ZIP (error) (stream, zerr);
  assert_perror (err);

  free (zip->cache.blocks);
  free (zip);
  store->misc = NULL;
  store->misc_len = 0;
}


error_t ZIP (open) (const char *name, int flags,
		    const struct store_class *const *classes,
		    struct store **store);

const struct store_class
STORE_ZIP (class) =
{
  STORAGE_OTHER, STRINGIFY (ZIP_TYPE), ZIP (read), ZIP (write), ZIP (set_size),
  ZIP (allocate_encoding), ZIP (encode), ZIP (decode),
  0, 0, ZIP (sync), 0, ZIP (remap), ZIP (open), ZIP (validate_name),
  ZIP (map)
};


/* Open an existing zip store.  */
error_t
ZIP (open) (const char *name, int flags,
		 const struct store_class *const *classes,
		 struct store **store)
{
  error_t err;
  file_t source;
  struct store *from;	/* Underlying store */
  struct ZIP (object) *zip;
  ZIP_STREAM *stream;

#ifdef ZIP_CRC_UPDATE
  /* Begin with a sanity check */
  assert (sizeof (zip->write.crc) == 4);
#endif

  /* Get a port to the underlying file (used by store_create ()) */
  source = file_name_lookup (name,
			     (flags & (STORE_READONLY | STORE_HARD_READONLY))
			     ? O_READ
			     : O_READ | O_WRITE,
			     S_IFREG);
  if (source == MACH_PORT_NULL)
    return errno;

  /* Open the underlying store */
  /* FIXME: We should use store_typed_open () but this requires to have
     a pointer to store_std_classes which we don't have.  */
  err = store_file_open (name, flags, &from);
  if (err)
    return err;

  /* FIXME: The following assumption should be removed at some point.  */
  assert (from->block_size == 1);
  debug (("Underlying file size is %i bytes", from->size));
  
  /* Actually create the store.  */
#if 0
  err = store_create (source,
		      flags | STORE_NO_FILEIO,
		      NULL,
		      store);
#else
  err = store_file_create (source, flags, store);
#endif

  if (err)
    return err;

  /* Allocate our data structure */
  zip = (struct ZIP (object) *) calloc (1, sizeof (struct ZIP (object)));
  if (!zip)
    return ENOMEM;
  
  zip->source = from;
  zip->read.file_status = zip->write.file_status = STATUS_RUNNING;
  zip->store = *store;
  stream = &zip->read.stream;

  mutex_init (&zip->read.lock);
  mutex_init (&zip->write.lock);
  mutex_init (&zip->cache.lock);

  (*store)->flags = flags;
  (*store)->block_size = 1;
  (*store)->log2_block_size = 0;
  (*store)->class = &STORE_ZIP (class);
  (*store)->misc = zip;
  (*store)->misc_len = sizeof (struct ZIP (object));

#ifdef ZIP_HAS_HEADER
  if (from->size)
  {
    /* Read & skip the gzip header */
    err = ZIP (read_header) (zip->source, &zip->start_file_offs,
			     &zip->header);
    assert_perror (err);
  }
#endif

  debug (("start_file_offs = %llu", zip->start_file_offs));

  /* Init zip stream */
  err = ZIP (stream_read_init) (zip);
  assert_perror (err);

  /* Traverse the whole file in order to create its offset map
     and get its size (ie. the uncompressed stream length).  */
  err = traverse (*store, &zip->zip_orig_size);
  if (err)
  {
    free (zip);
    return err;
  }

  zip->zip_orig_blocks_size = zip->zip_orig_size
			      ? BLOCK_NUMBER (zip->zip_orig_size - 1) + 1
			      : 0;
  (*store)->size = (*store)->end = (*store)->wrap_src = zip->zip_orig_size;
  debug (("Uncompressed stream size is %u", zip->zip_orig_size));
  
  {
    /* Assign just a single run to STORE */
    const struct store_run run = { 0, zip->zip_orig_size };
    err = store_set_runs (*store, &run, 1);
    assert_perror (err);

    /* Make sure that store_set_runs () and pals didn't change anything */
    assert ((*store)->size == zip->zip_orig_size);
  }

  return err;
}

error_t
STORE_ZIP (open) (const char *name, int flags, struct store **store)
{
  return ZIP (open) (name, flags, NULL, store);
}
