#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  
    bool isdir;                         /* is this entry dir? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt, block_sector_t parent)
{
  bool success;
  struct dir* dir;
//  lock_acquire (inode_dir_lock (dir->inode));
  success = inode_create (sector, entry_cnt * sizeof (struct dir_entry));
  
  dir = dir_open (inode_open (sector));
  dir_add (dir, "..", parent, true);
  dir_add (dir, ".", sector, true);
//  lock_release (inode_dir_lock (dir->inode));
  dir_close (dir);

  return success;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
  dir = NULL;
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}


/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector, bool isdir)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;

  e.isdir = isdir;

  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
//  lock_acquire (inode_dir_lock (dir->inode));
  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;
  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;
  if (e.isdir)
  {
    if(!dir_can_removed (dir_open (inode)))
    {
      goto done;
    }
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
//  lock_release (inode_dir_lock (dir->inode));
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

//  lock_acquire (inode_dir_lock (dir->inode));
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
//  lock_release (inode_dir_lock (dir->inode));
          return true;
        } 
    }
//  lock_release (inode_dir_lock (dir->inode));
  return false;
}


struct dir*
find_dir (const char *file_name_, char *fn_copy, struct dir* dir)
{
  bool abs;
  char* token;
  char* save_ptr;
  char *file_name = palloc_get_page (0);
  char *before_tok = palloc_get_page (0);
  strlcpy (file_name ,file_name_, PGSIZE);
  before_tok[0]='\0';
  char *s = file_name;
struct dir_entry *e = malloc (sizeof *e);
size_t *ofs;
  
abs = false;
  if(s[0] == '/')
  {
    abs = true;
    s += 1;
  }
  if (abs)
    dir = dir_open_root ();
  else
    dir = dir_reopen (dir);
  for (token = strtok_r (s, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
  {
bool result=false;

    if(before_tok[0] != '\0')
      result = lookup (dir, before_tok, e, ofs);
    if (result && e->isdir)
    {
        dir_close (dir);
        dir = dir_open (inode_open (e->inode_sector));
        if (dir == NULL)
        {
          free (e);
          return dir;
        }
    }
      strlcpy (fn_copy, token, PGSIZE);
      strlcpy (before_tok, token, PGSIZE);
  }
      path = dir_reopen(dir);
  free (e);
  palloc_free_page (before_tok);
  palloc_free_page (file_name);
  return dir;
}

bool
fd_elem_isdir (struct dir* dir, char* name)
{
  struct dir_entry *e=NULL;
  off_t *ofs;
  if(lookup (dir, name, e, ofs))
  {
    return e->isdir;
  }
  return false;
}

bool
mkdir_by_name (char* name, struct dir* dir)
{
  char* name_copy;
  name_copy = palloc_get_page(0);
  if(dir == NULL)
    dir = dir_open_root ();
  find_dir (name, name_copy, dir);

//  lock_acquire (inode_dir_lock (path->inode));
  if (path == NULL)
  {
    palloc_free_page (name_copy);
//  lock_release (inode_dir_lock (path->inode));
    return false;
  }
  struct inode *inode;
  if (dir_lookup (path, name_copy, &inode) || name_copy == '\0')
  {
    palloc_free_page (name_copy);
//  lock_release (inode_dir_lock (path->inode));
    return false;
  }
  block_sector_t inode_sector = 0;
  bool success = (free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, 16, inode_get_inumber(path->inode))
                  && dir_add (path, name_copy, inode_sector, true));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  
//  lock_release (inode_dir_lock (path->inode));
  dir_close (path);
  return success;
}

bool
isdir_by_name (struct dir* dir, char* name)
{
  struct dir_entry e;
  off_t *ofs;

  if (lookup (dir, name, &e, ofs))
  {
    return e.isdir;
  }
  return false;
}

bool
dir_is_empty (struct dir* dir)
{
  bool result=true;

  bool it;
  char* tmp = palloc_get_page(0);
dir->pos = 0;
  do
  {
    struct inode *inode;
    it = dir_readdir(dir, tmp);
    if (it)
    {
      if(strcmp ("..", tmp) == 0 || strcmp (".", tmp) == 0)
      {
        it = true;
      }
      else if (dir_lookup (dir, tmp, &inode) == false) 
      { 
        it = true;
      }
      else
      {
        it = false;
        result=false;
        break;
      }
    }
  }while(it);
  palloc_free_page(tmp);
  return result;
}

bool
dir_can_removed (struct dir* dir)
{
  if (inode_get_inumber(dir_get_inode (dir)) == ROOT_DIR_SECTOR)
    return false;
  if (!dir_is_empty (dir))
    return false;
  if (dir_is_pwd (dir))
    return false;
  return true;
}
