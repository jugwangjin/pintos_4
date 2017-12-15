#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "devices/block.h"
#include "devices/intq.h"
#include "threads/thread.h"

#define BLOCKMASK BLOCK_SECTOR_SIZE-1

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  int i;

  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
  
  for (i = 0; i < 64; i++)
  {
    buffer_cache[i] = malloc (sizeof *buffer_cache[i]);
    buffer_cache[i]->data = malloc (BLOCK_SECTOR_SIZE);
    buffer_cache[i]->accessed = false;
    buffer_cache[i]->dirty = false;
    buffer_cache[i]->inode = NULL;
    buffer_cache[i]->block_no = 0;
    buffer_cache[i]->valid = false;
    buffer_cache[i]->size = 0;
    lock_init (&buffer_cache[i]->block_lock);
  }
  buffer_iter = 0; 
  thread_current ()->pwd = dir_open_root (); 
  thread_create ("flush", PRI_MIN, flush_thread_func, NULL);
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  cache_flush ();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir;
  if(path == NULL)
    dir = dir_open_root ();
  else
    dir = dir_reopen (path);
  char* name_copy;
  name_copy = palloc_get_page (0);
  find_dir (name, name_copy, dir);
  bool success = (path != NULL
                  && (strlen(name_copy) <= NAME_MAX)
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (path, name_copy, inode_sector, false));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
palloc_free_page (name_copy);
  dir_close (dir);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir;
  char* name_copy;
if(strcmp (name, "/") == 0)
  return file_open (inode_open (ROOT_DIR_SECTOR));
  name_copy = palloc_get_page(0);

if(path == NULL)
  dir = dir_open_root ();
else
  dir = dir_reopen(path);

  struct inode *inode = NULL;
  find_dir (name, name_copy, dir);
  if (path != NULL)
    dir_lookup (path, name_copy, &inode);
  dir_close (dir);
palloc_free_page (name_copy);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir;
  char* name_copy = palloc_get_page (0);
  if(path == NULL)
    dir = dir_open_root ();
  else
    dir = dir_reopen (path);
  find_dir (name, name_copy, dir);
/*
  if (isdir_by_name (path, name_copy))
  {
    struct inode *inode;
    if(dir_lookup (path, name_copy, inode))
    { 
      if(!dir_can_removed (dir_open(inode))) 
        return false;
    }
  }
*/
  bool success = path != NULL && dir_remove (path, name_copy);
  palloc_free_page (name_copy);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

struct cache_block* find_cache_block (struct inode *inode, off_t pos)
{
  int i;
  off_t block_no;

  block_no = pos / BLOCK_SECTOR_SIZE;
  for (i = 0; i < 64; i++)
  {
    if (buffer_cache[i]->valid)
    {
      if (inode == buffer_cache[i]->inode && block_no == buffer_cache[i]->block_no)
      {
    lock_acquire (&buffer_cache[i]->block_lock);
        return buffer_cache[i];
      }
    }
  }
  // instead of NULL, load file and return that cache block
  // if full, evict and load
  return load_inode_block (inode, pos);
}

struct cache_block* load_inode_block (struct inode *inode, off_t pos)
{
  int i;
  off_t block_no;
  
  block_no = pos / BLOCK_SECTOR_SIZE;
//printf("pos is %x, block no is %d\n", pos, block_no);


  struct cache_block* load_dest = NULL;
 
  for (i = 0; i < 64; i++)
  {
    if (buffer_cache[i]->valid == false)
    {
      load_dest = buffer_cache[i];
    lock_acquire (&buffer_cache[i]->block_lock);
      break;
    }
  }
  if (load_dest == NULL)
  {
    load_dest = evict_cache_block ();
  }
//  enum intr_level old_level;
//  old_level = intr_disable ();
      load_dest->dirty = false;
      load_dest->accessed = false;
      load_dest->inode = inode;
      load_dest->block_no = block_no;
      inode_read_at (load_dest->inode, load_dest->data, BLOCK_SECTOR_SIZE, load_dest->block_no * BLOCK_SECTOR_SIZE);
      load_dest->size = 0;
      load_dest->valid = true;
//  intr_set_level (old_level);
  return load_dest;
}

struct cache_block* evict_cache_block (void)
{
int ret;
  while(true)
  {
    if (buffer_cache[buffer_iter]->accessed)
    {
      buffer_cache[buffer_iter]->accessed=false;
    }
    else
    {
      ret = buffer_iter;
      cache_write_back (buffer_cache[ret]);
      lock_acquire (&buffer_cache[ret]->block_lock);
      buffer_cache[ret]->valid = false;
      buffer_iter = ( buffer_iter + 1 ) % 64;
      return buffer_cache[ret];
    }
    buffer_iter = ( buffer_iter + 1 ) % 64;
  }

}

bool cache_write_back (struct cache_block *cache_block)
{
int written;
  if (!cache_block->valid || !cache_block->dirty)
  { 
    return false;
  }
  lock_acquire (&cache_block->block_lock);
  written=inode_write_at (cache_block->inode, cache_block->data, cache_block->size, cache_block->block_no * BLOCK_SECTOR_SIZE);
  
//hex_dump (0, cache_block->data, BLOCK_SECTOR_SIZE, true);
    cache_block->dirty = false;
  lock_release (&cache_block->block_lock);
    return true;
  
}

void cache_flush (void)
{
  int i;
 
  for (i = 0; i < 64; i++)
  {
      cache_write_back (buffer_cache[i]);
  }
}

void flush_thread_func (void)
{
  cache_flush ();
  timer_sleep (100);
}

void file_write_back (struct inode *inode)
{
  int i;
  
  for (i = 0; i < 64; i++)
  {
    if (buffer_cache[i]->inode == inode)
    {
      cache_write_back (buffer_cache[i]);
    }
  }
}
