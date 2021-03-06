#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t blocks[12];		/* 0-9: direct. 10: indirect. 11: 2 level indirect */
    block_sector_t parent;
    bool isdir;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[112];               /* Not used. */
  };

struct indirect_block
{
	block_sector_t blocks[128];
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
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
	struct indirect_block indirect_block;
	if (pos < inode->data.length)
	{
		if (pos < BLOCK_SECTOR_SIZE * 10)
			return inode->data.blocks[pos/BLOCK_SECTOR_SIZE];
		pos -= BLOCK_SECTOR_SIZE * 10;
		if (pos < BLOCK_SECTOR_SIZE * 128)
		{
			block_read (fs_device, inode->data.blocks[10], &indirect_block);
			return indirect_block.blocks[pos/BLOCK_SECTOR_SIZE];
		}
		pos -= BLOCK_SECTOR_SIZE * 128;
		block_read (fs_device, inode->data.blocks[11], &indirect_block);
		block_read (fs_device, indirect_block.blocks[pos/(BLOCK_SECTOR_SIZE*128)], &indirect_block);
		pos %= BLOCK_SECTOR_SIZE * 128;
		return indirect_block.blocks[pos/BLOCK_SECTOR_SIZE];
	}
	else
		return -1;
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

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
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
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->isdir = isdir;
      disk_inode->parent = ROOT_DIR_SECTOR;
	if (sectors == 0)
		success = true;
	static char zeros[BLOCK_SECTOR_SIZE];
	if (!success)
	{
		int i = 0;
		while (i < 10)
		{
			free_map_allocate (1, &disk_inode->blocks[i]);
			block_write (fs_device, disk_inode->blocks[i], zeros);
			i++;
			sectors--;
			if (sectors == 0)
			{
				success = true;
				break;
			}	
		}
	}
	if (!success)
	{
		struct indirect_block indirect_block;
		free_map_allocate (1, &disk_inode->blocks[10]);
		int j = 0;
		while (j < 128)
		{
			free_map_allocate (1, &indirect_block.blocks[j]);
			block_write (fs_device, indirect_block.blocks[j], zeros);
			j++;
			sectors--;
			if (sectors == 0)
			{
				success = true;
				break;
			}
		}
		block_write (fs_device, disk_inode->blocks[10], &indirect_block);
	}
	if (!success)
	{
		struct indirect_block first_block;
		struct indirect_block second_block;
		free_map_allocate (1, &disk_inode->blocks[11]);
		int k = 0;
		int l = 0;
		while (k < 128)
		{
			free_map_allocate (1, &first_block.blocks[k]);
			while (l < 128)
			{
				free_map_allocate (1, &second_block.blocks[l]);
				block_write (fs_device, second_block.blocks[l], zeros);
				l++;
				sectors--;
				if (sectors == 0)
					break;
			}
			block_write (fs_device, first_block.blocks[k], &second_block);
			k++;
			l = 0;
			if (sectors == 0)
			{
				success = true;
				break;
			}
		}
		block_write (fs_device, disk_inode->blocks[11], &first_block);
	}
	if (success)
		block_write (fs_device, sector, disk_inode);
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
          free_map_release (inode->sector, 1);
		size_t sectors = bytes_to_sectors (inode->data.length);
		if (sectors)
		{
			int i = 0;
			while (i < 10)
			{
				free_map_release (inode->data.blocks[i], 1);
				i++;
				sectors--;
				if(sectors == 0)
					break;
			}
		}
		if (sectors)
		{
			struct indirect_block indirect_block;
			block_read (fs_device, inode->data.blocks[10], &indirect_block);
			int j = 0;
			while (j < 128)
			{
				free_map_release (indirect_block.blocks[j], 1);
				j++;
				sectors--;
				if (sectors == 0)
					break;
			}
			free_map_release (inode->data.blocks[10], 1);
		}
		if (sectors)
		{
			struct indirect_block first_block;
			struct indirect_block second_block;
			block_read (fs_device, inode->data.blocks[11], &first_block);
			int k = 0;
			int l = 0;
			while (k <128)
			{
				block_read (fs_device, first_block.blocks[k], &second_block);
				while (l <128)
				{
					free_map_release (second_block.blocks[l], 1);
					l++;
					sectors--;
					if (sectors == 0)
						break;
				}
				free_map_release (first_block.blocks[k], 1);
				k++;
				l = 0;
				if (sectors == 0)
					break;
			}
			free_map_release (inode->data.blocks[11], 1);
		}
        }
	else
	{
		block_write (fs_device, inode->sector, &inode->data);
	}
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
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  struct cache *cache;
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
	cache = get_cache (sector_idx);
	memcpy (buffer + bytes_read, (uint8_t *) &cache->data + sector_ofs, chunk_size);
	cache->accessed = true;
	cache->used--;

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

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
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  struct cache *cache;
  if (inode->deny_write_cnt)
    return 0;

	if (offset + size > inode->data.length)
	{
		inode_extend (inode, offset+size);
	}

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

	cache = get_cache (sector_idx);
	memcpy ((uint8_t *) &cache->data + sector_ofs, buffer + bytes_written, chunk_size);
	cache->accessed = true;
	cache->dirty = true;
	cache->used--;

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
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

void
inode_extend (struct inode *inode, off_t length)
{
	size_t sectors = bytes_to_sectors (inode->data.length);
	size_t new_sectors = bytes_to_sectors (length) - sectors;

	if (new_sectors == 0)
	{
		inode->data.length = length;	
		return;
	}
	
	static char zeros[BLOCK_SECTOR_SIZE];
	int i = sectors;
	while (i < 10)
	{
		if(!free_map_allocate (1, &inode->data.blocks[i]))
		{
			inode->data.length = length - new_sectors*BLOCK_SECTOR_SIZE;
			return;
		}
		block_write (fs_device, inode->data.blocks[i], zeros);
		i++;
		sectors++;
		new_sectors--;
		if (new_sectors == 0)
		{
			inode->data.length = length;
			return;
		}
	}
	int j = sectors - 10;
	struct indirect_block indirect_block;
	if(j == 0)
	{
		if(!free_map_allocate (1, &inode->data.blocks[10]))
		{
			inode->data.length = length - new_sectors*BLOCK_SECTOR_SIZE;
			return;
		}
	}
	else
		block_read (fs_device, inode->data.blocks[10], &indirect_block);
	while (j < 128)
	{
		if(!free_map_allocate (1, &indirect_block.blocks[j]))
		{
			inode->data.length = length - new_sectors*BLOCK_SECTOR_SIZE;
			block_write (fs_device, inode->data.blocks[10], &indirect_block);
			return;
		}
		block_write (fs_device, indirect_block.blocks[j], zeros);
		j++;
		sectors++;
		new_sectors--;
		if (new_sectors == 0)
		{
			inode->data.length = length;
			block_write (fs_device, inode->data.blocks[10], &indirect_block);
			return;
		}
	}
	block_write (fs_device, inode->data.blocks[10], &indirect_block);
	int k = (sectors - 138) / 128;
	int l = (sectors - 138) % 128;
	struct indirect_block first_block;
	struct indirect_block second_block;
	if (k == 0 && l == 0)
	{
		if(!free_map_allocate (1, &inode->data.blocks[11]))
		{
			inode->data.length = length - new_sectors*BLOCK_SECTOR_SIZE;
			return;
		}
	}
	else
		block_read (fs_device, inode->data.blocks[11], &first_block);
	while (k < 128)
	{
		if (l == 0)
		{
			if(!free_map_allocate (1, &first_block.blocks[k]))
				break;
		}
		else
			block_read (fs_device, first_block.blocks[k], &second_block);
		while (l < 128)
		{
			if(!free_map_allocate (1, &second_block.blocks[l]))
				break;
			block_write (fs_device, second_block.blocks[l], zeros);
			l++;
			new_sectors--;
			if (new_sectors == 0)
			{
				break;
			}
		}
		block_write (fs_device, first_block.blocks[k], &second_block);
		k++;
		l = 0;
		if (new_sectors == 0)
		{
			inode->data.length = length;
			block_write (fs_device, inode->data.blocks[11], &first_block);
			return;
		}
	}
	inode->data.length = length - new_sectors*(BLOCK_SECTOR_SIZE);
	block_write (fs_device, inode->data.blocks[11], &first_block);
	return;
}

bool inode_isdir (const struct inode *inode)
{
	return inode->data.isdir;
}

int inode_get_cnt (const struct inode *inode)
{
	return inode->open_cnt;
}

block_sector_t inode_get_parent (const struct inode *inode)
{
	return inode->data.parent;
}

bool inode_set_parent (block_sector_t child, block_sector_t parent)
{
	struct inode* inode = inode_open(child);
	if(inode == NULL)
		return false;
	else
	{
		inode->data.parent = parent;
		inode_close(inode);
		return true;
	}
}
