#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

static struct file *free_map_file;   /* Free map file. */
static struct bitmap *free_map;      /* Free map, one bit per sector. */
static struct lock free_map_lock;    /* Mutual exclusion. */

/* Initializes the free map. */
void
free_map_init (void) 
{
  lock_init (&free_map_lock);

  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");
  bitmap_mark (free_map, FREE_MAP_SECTOR);
  bitmap_mark (free_map, ROOT_DIR_SECTOR);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool
free_map_allocate (block_sector_t *sectorp)
{
  size_t sector;
  
  lock_acquire (&free_map_lock);
  sector = bitmap_scan_and_flip (free_map, 0, 1, false);
  lock_release (&free_map_lock);

  if (sector != BITMAP_ERROR) 
     *sectorp = sector;
  return sector != BITMAP_ERROR;
}

/* Makes SECTOR available for use. */
void
free_map_release (block_sector_t sector)
{
  lock_acquire (&free_map_lock);
  ASSERT (bitmap_test (free_map, sector));
  bitmap_reset (free_map, sector);
  lock_release (&free_map_lock);
}

/* Opens the free map file and reads it from disk. */
void
free_map_open (void) 
{
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_read (free_map, free_map_file))
    PANIC ("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close (void) 
{
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
  file_close (free_map_file);
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create (void) 
{
  struct inode *inode;

  /* Create inode. */
  inode = file_create (FREE_MAP_SECTOR, 0);
  if (inode == NULL)   
    PANIC ("free map creation failed");
  inode_close (inode);

  /* Write bitmap to file. */
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
}
