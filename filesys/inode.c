#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "devices/block.h"
//#include "filesys/cache.h"
#include "threads/synch.h"
#include "userprog/syscall.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct[10];
    block_sector_t single_level;
    block_sector_t double_level;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[114];               /* Not used. */
  };

struct inode_disk_level
  {
    block_sector_t index[128];
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
    block_sector_t sector;              /* Sector number of inode_disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    struct lock lock;
  };

void file_extension(struct inode* inode_, off_t size, off_t offset);




/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  struct inode_disk *disk_inode = &inode->data;
  int quotient = pos / BLOCK_SECTOR_SIZE;
  struct inode_disk_level buffer;
  if (quotient < 10) // direct level
    return disk_inode->direct[quotient];
  quotient -= 10;
  if (quotient < 128) // single level
  {
    block_read (fs_device, disk_inode->single_level, &buffer);
    return buffer.index[quotient];
  }
  quotient -= 128;
  if (quotient < 128 * 128) // double level
  {
    int remainder = quotient % 128;
    quotient = quotient / 128;
    block_read (fs_device, disk_inode->double_level, &buffer);
    block_read (fs_device, buffer.index[quotient], &buffer);
    return buffer.index[remainder];
  }
  return -1; // too large
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with 0 bytes of file data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      int i;
      for(i = 0; i < 10; i++)
	disk_inode->direct[i] = -1;
      disk_inode->single_level = -1;
      disk_inode->double_level = -1;
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      
      block_write(fs_device, sector, disk_inode);
      success = true;
      free (disk_inode);
    }

  return success;
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
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
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
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1); // inode_disk
	  int i, j;
	  for(i = 0; i < 10; i++) // direct level
	  {
	    if(inode->data.direct[i] == -1)
	      goto done;
	    free_map_release (inode->data.direct[i], 1);
	  }
	  struct inode_disk_level buffer1, buffer2;


	  if(inode->data.single_level == -1) // single level
	    goto done;
	  block_read(fs_device, inode->data.single_level, &buffer1);
	  for(i = 0; i < 128; i++)
	  {
	    if(buffer1.index[i] == -1)
	    {
	      free_map_release (inode->data.single_level, 1);
	      goto done;
	    }
	    free_map_release (buffer1.index[i], 1);
	  }
	  free_map_release(inode->data.single_level, 1);


	  if(inode->data.double_level == -1) // double level
	    goto done;
	  block_read(fs_device, inode->data.double_level, &buffer1);
	  for(i = 0; i < 128; i++)
	  {
	    if(buffer1.index[i] == -1)
	    {
	      free_map_release (inode->data.double_level, 1);
	      goto done;
	    }
	    block_read (fs_device, buffer1.index[i], &buffer2);
	    for(j = 0; j < 128; j++)
	    {
	      if(buffer2.index[j] == -1)
	      {
		free_map_release (buffer1.index[i], 1);
		free_map_release (inode->data.double_level, 1);
		goto done;
	      }
	      free_map_release(buffer2.index[j], 1);
	    }
	    free_map_release(buffer1.index[i], 1);
	  }
	  free_map_release(inode->data.double_level, 1);
        }
    done:
      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
//  lock_acquire(&inode->lock);
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  if(offset + size > inode->data.length)
  {
    int diff = offset + size - inode->data.length;
    if((size = size - diff) < 0)
      return 0;
  }

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

//      memcpy(buffer + bytes_read,(uint8_t*)cache_upload(sector_idx,false) + sector_ofs, chunk_size);
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
	  block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        } 
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

//  lock_release(&inode->lock);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
//  lock_acquire(&inode->lock);
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  if (offset + size > inode->data.length)
    file_extension(inode, size, offset);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;
      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
//      memcpy ((uint8_t*)cache_upload(sector_idx, true) + sector_ofs, buffer + bytes_written, chunk_size);

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      { 
        block_write (fs_device, sector_idx, buffer+bytes_written);
      }
      else
      { 
        if (bounce == NULL)
        {
          bounce == malloc (BLOCK_SECTOR_SIZE);
          if (bounce == NULL)
            break;
        }
      

      if (sector_ofs>0 & chunk_size < sector_left)
        block_read (fs_device, sector_idx, bounce);
      else
        memset (bounce, 0, BLOCK_SECTOR_SIZE);
 
      memcpy (bounce + sector_ofs, buffer+bytes_written, chunk_size);
      block_write (fs_device, sector_idx, bounce);
      }
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

//  lock_release(&inode->lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/* Extend the file with INODE.
   1. Calculate the number of new blocks needed for extension. Get the indexes of free block
      sectors in fs_device with free_map_allocate. Make a list of indexes.
      If there are not enough free sectors, return false.
   2. Fill(write) corresponding block sectors with zeroes.
   3. Update the index information of the allocated zero-filled blocks in INODE.
 */

void file_extension(struct inode* inode_, off_t size, off_t offset)
{
lock_acquire (&FILELOCK);
  // 1
  int alloc = inode_->data.length ? (inode_->data.length - 1) / BLOCK_SECTOR_SIZE + 1 : 0;
  int req = (offset + size - 1) / BLOCK_SECTOR_SIZE + 1;
  off_t modified_length, add_length;
  block_sector_t *slist = malloc(req - alloc + 1);
  int i, j, iter = 0;
  for(i = 0; i < req - alloc; i++)
  {
    if(!free_map_allocate(1, &slist[i]))
      break;
  }

  slist[i] = -1;
  add_length = BLOCK_SECTOR_SIZE - (inode_->data.length % BLOCK_SECTOR_SIZE) + i*BLOCK_SECTOR_SIZE;
  modified_length = inode_->data.length + add_length; 

  // 2
  if(inode_->data.length % BLOCK_SECTOR_SIZE) // zero padding on the file's last block
  {
    block_sector_t sector_idx = byte_to_sector(inode_, inode_->data.length);
    int sector_ofs = inode_->data.length % BLOCK_SECTOR_SIZE;
//    memset((uint8_t*)cache_upload(sector_idx, true) + sector_ofs, 0, BLOCK_SECTOR_SIZE - sector_ofs);
  }
//  for(i = 0; slist[i] != -1; i++) // zero padding on new blocks
//    memset((uint8_t*)cache_upload(slist[i], true), 0, BLOCK_SECTOR_SIZE);

  // 3
  struct inode_disk *disk_inode = malloc(BLOCK_SECTOR_SIZE);
  memcpy(disk_inode, &inode_->data, BLOCK_SECTOR_SIZE);
  struct inode_disk_level *level1 = NULL, *level2_1 = NULL, *level2_2 = NULL;

  if(slist[0] == -1) // no additional blocks obtained in extension
    goto fin;

  /* direct level */
  for(i = 0; i < 10; i++)
  {
    if(disk_inode->direct[i] == -1)
    {
      disk_inode->direct[i] = slist[iter++];
      if(slist[iter] == -1)
	goto fin;
    }
  }

  /* single level */
  block_sector_t index;
  level1 = malloc(BLOCK_SECTOR_SIZE);
  if(disk_inode->single_level == -1)
  {
    free_map_allocate(1, &index);
    disk_inode->single_level = index;
    for(i = 0; i < 128; i++)
      level1->index[i] = -1;
  }
  else
    block_read(fs_device, disk_inode->single_level, level1);
  
  for(i = 0; i < 128; i++)
  {
    if(level1->index[i] == -1)
    {
      level1->index[i] = slist[iter++];
      if(slist[iter] == -1)
	goto fin;
    }
  }

  /* double level */
  level2_1 = malloc(BLOCK_SECTOR_SIZE);
  level2_2 = malloc(BLOCK_SECTOR_SIZE);
  if(disk_inode->double_level == -1)
  {
    free_map_allocate(1, &index);
    disk_inode->double_level = index;
    for(i = 0; i < 128; i++)
      level2_1->index[i] = -1;
  }
  else
    block_read(fs_device, disk_inode->double_level, level2_1);

  for(i = 0; i < 128; i++)
  {
    bool written = false;
    if(level2_1->index[i] == -1)
    {
      free_map_allocate(1, &index);
      level2_1->index[i] = index;
      for(j = 0; j < 128; j++)
	level2_2->index[j] = -1;
    }
    else
      block_read(fs_device, level2_1->index[i], level2_2);

    for(j = 0; j < 128; j++)
    {
      if(level2_2->index[j] == -1)
      {
	level2_2->index[j] = slist[iter++];
	written = true;
	if(slist[iter] == -1)
	  goto fin;
      }
    }
    if(written)
      block_write(fs_device, level2_1->index[i], level2_2);
  }

fin:
  disk_inode->length = modified_length;
  block_write(fs_device, inode_->sector, disk_inode);
  memcpy(&inode_->data, disk_inode, BLOCK_SECTOR_SIZE);
  free(disk_inode);
  if(level1 != NULL)
  {
    block_write(fs_device, inode_->data.single_level, level1);
    free(level1);
  }
  if(level2_2 != NULL)
  {
    if(i == 128)
      i--;
    block_write(fs_device, level2_1->index[i], level2_2);
    free(level2_2);
  }
  if(level2_1 != NULL)
  {
    block_write(fs_device, inode_->data.double_level, level2_1);
    free(level2_1);
  }
  free(slist);
lock_release(&FILELOCK);
}

int
inode_deny_cnt (const struct inode* inode)
{
  return inode->deny_write_cnt;
}
