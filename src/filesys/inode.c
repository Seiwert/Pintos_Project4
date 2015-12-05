#include "filesys/inode.h"
#include <bitmap.h>
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_CNT 123
#define INDIRECT_CNT 1
#define DBL_INDIRECT_CNT 1
#define SECTOR_CNT (DIRECT_CNT + INDIRECT_CNT + DBL_INDIRECT_CNT)

#define PTRS_PER_SECTOR ((off_t) (BLOCK_SECTOR_SIZE / sizeof (block_sector_t)))
#define INODE_SPAN ((DIRECT_CNT                                              \
                     + PTRS_PER_SECTOR * INDIRECT_CNT                        \
                     + PTRS_PER_SECTOR * PTRS_PER_SECTOR * DBL_INDIRECT_CNT) \
                    * BLOCK_SECTOR_SIZE)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[SECTOR_CNT]; /* Sectors. */
    enum inode_type type;               /* FILE_INODE or DIR_INODE. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    struct lock lock;                   /* Protects the inode. */

    /* Denying writes. */
    struct lock deny_write_lock;        /* Protects members below. */
    struct condition no_writers_cond;   /* Signaled when no writers. */ 
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    int writer_cnt;                     /* Number of writers. */
  };

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Controls access to open_inodes list. */
static struct lock open_inodes_lock;

static void deallocate_inode (const struct inode *);

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode of the given TYPE, writes the new inode
   to sector SECTOR on the file system device, and returns the
   inode thus created.  Returns a null pointer if unsuccessful,
   in which case SECTOR is released in the free map. */  
struct inode *
inode_create (block_sector_t sector, enum inode_type type) 
{
  struct cache_block *block;
  struct inode_disk *disk_inode;
  struct inode *inode;

  block = cache_lock (sector, EXCLUSIVE);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);
  disk_inode = cache_zero (block);
  disk_inode->type = type;
  disk_inode->length = 0;
  disk_inode->magic = INODE_MAGIC;
  cache_dirty (block);
  cache_unlock (block);

  inode = inode_open (sector);
  if (inode == NULL)
    free_map_release (sector);
  return inode;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire (&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode->open_cnt++;
          goto done; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    goto done;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  lock_init (&inode->lock);
  inode->deny_write_cnt = 0;
  lock_init (&inode->deny_write_lock);
  cond_init (&inode->no_writers_cond);
  inode->removed = false;
  
 done:
  lock_release (&open_inodes_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&open_inodes_lock);
      inode->open_cnt++;
      lock_release (&open_inodes_lock);
    }
  return inode;
}

/* Returns the type of INODE. */
enum inode_type
inode_get_type (const struct inode *inode) 
{
  struct cache_block *inode_block = cache_lock (inode->sector, NON_EXCLUSIVE);
  struct inode_disk *disk_inode = cache_read (inode_block);
  enum inode_type type = disk_inode->type;
  cache_unlock (inode_block);
  return type;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  lock_acquire (&open_inodes_lock);
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      lock_release (&open_inodes_lock);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        deallocate_inode (inode);

      free (inode); 
    }
  else
    lock_release (&open_inodes_lock);
}

/* Deallocates SECTOR and anything it points to recursively.
   LEVEL is 2 if SECTOR is doubly indirect,
   or 1 if SECTOR is indirect,
   or 0 if SECTOR is a data sector. */
static void
deallocate_recursive (block_sector_t sector, int level) 
{
  if (level > 0) 
    {
      struct cache_block *block = cache_lock (sector, EXCLUSIVE);
      block_sector_t *pointers = cache_read (block);
      int i;
      for (i = 0; i < PTRS_PER_SECTOR; i++)
        if (pointers[i])
          deallocate_recursive (sector, level - 1);
      cache_unlock (block);
    }
  
  cache_free (sector);
  free_map_release (sector);
}

/* Deallocates the blocks allocated for INODE. */
static void
deallocate_inode (const struct inode *inode)
{
  struct cache_block *block = cache_lock (inode->sector, EXCLUSIVE);
  struct inode_disk *disk_inode = cache_read (block);
  int i;
  for (i = 0; i < SECTOR_CNT; i++)
    if (disk_inode->sectors[i]) 
      {
        int level = (i >= DIRECT_CNT) + (i >= DIRECT_CNT + INDIRECT_CNT);
        deallocate_recursive (disk_inode->sectors[i], level); 
      }
  cache_unlock (block);
  deallocate_recursive (inode->sector, 0);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Translates SECTOR_IDX into a sequence of block indexes in
   OFFSETS and sets *OFFSET_CNT to the number of offsets. */
static void
calculate_indices (off_t sector_idx, size_t offsets[], size_t *offset_cnt)
{
  /* Handle direct blocks. */
  if (sector_idx < DIRECT_CNT) 
    {
      offsets[0] = sector_idx;
      *offset_cnt = 1;
      return;
    }
  sector_idx -= DIRECT_CNT;

  /* Handle indirect blocks. */
  if (sector_idx < PTRS_PER_SECTOR * INDIRECT_CNT)
    {
      offsets[0] = DIRECT_CNT + sector_idx / PTRS_PER_SECTOR;
      offsets[1] = sector_idx % PTRS_PER_SECTOR;
      *offset_cnt = 2;
      return;
    }
  sector_idx -= PTRS_PER_SECTOR * INDIRECT_CNT;

  /* Handle doubly indirect blocks. */
  if (sector_idx < DBL_INDIRECT_CNT * PTRS_PER_SECTOR * PTRS_PER_SECTOR)
    {
      offsets[0] = (DIRECT_CNT + INDIRECT_CNT
                    + sector_idx / (PTRS_PER_SECTOR * PTRS_PER_SECTOR));
      offsets[1] = sector_idx / PTRS_PER_SECTOR;
      offsets[2] = sector_idx % PTRS_PER_SECTOR;
      *offset_cnt = 3;
      return;
    }
  NOT_REACHED ();
}

/* Retrieves the data block for the given byte OFFSET in INODE,
   setting *DATA_BLOCK to the block.
   Returns true if successful, false on failure.
   If ALLOCATE is false, then missing blocks will be successful
   with *DATA_BLOCk set to a null pointer.
   If ALLOCATE is true, then missing blocks will be allocated.
   The block returned will be locked, normally non-exclusively,
   but a newly allocated block will have an exclusive lock. */
static bool
get_data_block (struct inode *inode, off_t offset, bool allocate,
                struct cache_block **data_block) 
{
  block_sector_t this_level_sector;
  size_t offsets[3];
  size_t offset_cnt;
  size_t level;

  ASSERT (offset >= 0);

  calculate_indices (offset / BLOCK_SECTOR_SIZE, offsets, &offset_cnt);
  level = 0;
  this_level_sector = inode->sector;
  for (;;) 
    {
      struct cache_block *this_level_block;
      uint32_t *this_level_data;

      struct cache_block *next_level_block;

      /* Check whether the block for the next level is allocated. */
      this_level_block = cache_lock (this_level_sector, NON_EXCLUSIVE);
      this_level_data = cache_read (this_level_block);
      if (this_level_data[offsets[level]] != 0)
        {
          /* Yes, it's allocated.  Advance to next level. */
          this_level_sector = this_level_data[offsets[level]];

          if (++level == offset_cnt) 
            {
              /* We hit the data block.
                 Do read-ahead. */
              if ((level == 0 && offsets[level] + 1 < DIRECT_CNT)
                  || (level > 0 && offsets[level] + 1 < PTRS_PER_SECTOR)) 
                {
                  uint32_t next_sector = this_level_data[offsets[level] + 1];
                  if (next_sector
                      && next_sector < block_size (fs_device))
                    cache_readahead (next_sector); 
                }
              cache_unlock (this_level_block);

              /* Return block. */
              *data_block = cache_lock (this_level_sector, NON_EXCLUSIVE);
              return true;
            }
          cache_unlock (this_level_block);
          continue;
        }
      cache_unlock (this_level_block);

      /* No block is allocated.  Nothing is locked.
         If we're not allocating new blocks, then this is
         "success" (with all-zero data). */
      if (!allocate) 
        {
          *data_block = NULL;
          return true;
        }

      /* We need to allocate a new block.
         Grab an exclusive lock on this level's block so we can
         insert the new block. */
      this_level_block = cache_lock (this_level_sector, EXCLUSIVE);
      this_level_data = cache_read (this_level_block);

      /* Since we released this level's block, someone else might
         have allocated the block in the meantime.  Recheck. */
      if (this_level_data[offsets[level]] != 0)
        {
          cache_unlock (this_level_block);
          continue;
        }

      /* Allocate the new block. */
      if (!free_map_allocate (&this_level_data[offsets[level]]))
        {
          cache_unlock (this_level_block);
          *data_block = NULL;
          return false;
        }
      cache_dirty (this_level_block);

      /* Lock and clear the new block. */
      next_level_block = cache_lock (this_level_data[offsets[level]],
                                     EXCLUSIVE);
      cache_zero (next_level_block);

      /* Release this level's block.  No one else can access the
         new block yet, because we have an exclusive lock on it. */
      cache_unlock (this_level_block);

      /* If this is the final level, then return the new block. */
      if (level == offset_cnt - 1) 
        {
          *data_block = next_level_block;
          return true;
        }

      /* Otherwise, release the new block and go around again to
         follow the new pointer. */
      cache_unlock (next_level_block);
    }
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Sector to read, starting byte offset within sector, sector data. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      struct cache_block *block;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 || !get_data_block (inode, offset, false, &block))
        break;

      if (block == NULL) 
        memset (buffer + bytes_read, 0, chunk_size);
      else 
        {
          const uint8_t *sector_data = cache_read (block);
          memcpy (buffer + bytes_read, sector_data + sector_ofs, chunk_size);
          cache_unlock (block);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Extends INODE to be at least LENGTH bytes long. */
static void
extend_file (struct inode *inode, off_t length) 
{
  if (length > inode_length (inode)) 
    {
      struct cache_block *inode_block = cache_lock (inode->sector, EXCLUSIVE);
      struct inode_disk *disk_inode = cache_read (inode_block);
      if (length > disk_inode->length) 
        {
          disk_inode->length = length;
          cache_dirty (inode_block);
        }
      cache_unlock (inode_block);
    }
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if an error occurs. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  /* Don't write if writes are denied. */
  lock_acquire (&inode->deny_write_lock);
  if (inode->deny_write_cnt) 
    {
      lock_release (&inode->deny_write_lock);
      return 0;
    }
  inode->writer_cnt++;
  lock_release (&inode->deny_write_lock);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector, sector data. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      struct cache_block *block;
      uint8_t *sector_data;

      /* Bytes to max inode size, bytes left in sector, lesser of the two. */
      off_t inode_left = INODE_SPAN - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;

      if (chunk_size <= 0 || !get_data_block (inode, offset, true, &block))
        break;

      sector_data = cache_read (block);
      memcpy (sector_data + sector_ofs, buffer + bytes_written, chunk_size);
      cache_dirty (block);
      cache_unlock (block);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  extend_file (inode, offset);

  lock_acquire (&inode->deny_write_lock);
  if (--inode->writer_cnt == 0)
    cond_signal (&inode->no_writers_cond, &inode->deny_write_lock);
  lock_release (&inode->deny_write_lock);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->deny_write_lock);
  while (inode->writer_cnt > 0)
    cond_wait (&inode->no_writers_cond, &inode->deny_write_lock);
  ASSERT (inode->deny_write_cnt < inode->open_cnt);
  inode->deny_write_cnt++;
  lock_release (&inode->deny_write_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire (&inode->deny_write_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->deny_write_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct cache_block *inode_block = cache_lock (inode->sector, NON_EXCLUSIVE);
  struct inode_disk *disk_inode = cache_read (inode_block);
  off_t length = disk_inode->length;
  cache_unlock (inode_block);
  return length;
}

/* Returns the number of openers. */
int
inode_open_cnt (const struct inode *inode) 
{
  int open_cnt;
  
  lock_acquire (&open_inodes_lock);
  open_cnt = inode->open_cnt;
  lock_release (&open_inodes_lock);

  return open_cnt;
}

/* Locks INODE. */
void
inode_lock (struct inode *inode) 
{
  lock_acquire (&inode->lock);
}

/* Releases INODE's lock. */
void
inode_unlock (struct inode *inode) 
{
  lock_release (&inode->lock);
}
