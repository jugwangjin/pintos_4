#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "filesys/inode.h"
#include "threads/synch.h"
/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Block device that contains the file system. */
struct block *fs_device;

struct cache_block
  {
    void* data;
    bool dirty;
    bool accessed;
    struct inode *inode;
    off_t block_no;
    bool valid;

    off_t size;
    struct lock block_lock;
  };

/* filesys buffer cache
   It will be allocated for 64 blocks array */
struct cache_block *buffer_cache[64];
int buffer_iter;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

struct cache_block* find_cache_block (struct inode *inode, off_t pos);
struct cache_block* load_inode_block (struct inode *inode, off_t pos);
struct cache_block* evict_cache_block (void);
bool cache_write_back (struct cache_block *cache_block);

void cache_flush ();
void flush_thread_func ();
void file_write_back (struct inode *inode);

#endif /* filesys/filesys.h */
