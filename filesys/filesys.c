#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  init_cache ();  

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
	close_cache ();
  free_map_close ();
}

struct dir* parse_dir(const char* name)
{
	if(!strlen(name))
		return NULL;
	struct dir *dir;
	char *copy_name = (char *)malloc(strlen(name)+1);
	char *token;
	char *ptoken;
	char *save_ptr;
	//copy name	
	strlcpy(copy_name, name, strlen(name)+1);

	//In case of absolute address or root directory
	if(copy_name[0] == '/' || thread_current()->cudir == NULL)
		dir = dir_open_root();
	else
		dir = dir_reopen(thread_current()->cudir);			

	ptoken = strtok_r(copy_name, "/", &save_ptr);
	for(token = strtok_r(NULL, "/",&save_ptr); token != NULL; token = strtok_r(NULL, "/", &save_ptr))
	{
		struct inode* inode = NULL;
		if(ptoken == NULL)
		{
			return NULL;
		}
		else if(ptoken == ".")
		{
			ptoken = token;
			continue;
		}
		else if(ptoken == "..")
		{	
			if(dir != NULL)
			{
				inode = inode_open(inode_get_parent(dir_get_inode(dir)));
			}
			if(inode == NULL)
				return NULL;
		}
		else if(dir_lookup(dir, ptoken, &inode) == false)
			return NULL;

		if(inode_isdir(inode))
		{
			dir_close(dir);
			dir = dir_open(inode);
		}
		else
		inode_close(inode);
		
		ptoken = token;
	}

	free(copy_name);
	return dir;
}

char* parse_file(const char* name)
{
	if(!strlen(name))
		return NULL;
	char *copy_name = (char *)malloc(strlen(name)+1);
	char *token;
	char *ptoken;
	char *save_ptr;
	//copy name	
	strlcpy(copy_name, name, strlen(name)+1);

	for(token = strtok_r(copy_name, "/", &save_ptr); token !=NULL; token = strtok_r(NULL,"/",&save_ptr))
		ptoken = token;
	char *real_name = (char *)malloc(strlen(ptoken)+1);
	strlcpy(real_name, ptoken, strlen(ptoken) +1);
	free(copy_name);
	return real_name;
}


/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool isdir) 
{
	block_sector_t inode_sector = 0;
 	struct dir *dir = parse_dir(name);
	char* file_name = parse_file(name);

	if(name == NULL || file_name == NULL || dir == NULL)
		return false;	
	if(file_name == "." || file_name == "..")
		return false;
	
	bool success = (dir != NULL
			&& free_map_allocate(1, &inode_sector)
			&& inode_create (inode_sector, initial_size, isdir)
			&& dir_add (dir, file_name, inode_sector));
	if(!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
	dir_close(dir);
	free(file_name);

	return success;
/*
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_root ();
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, isdir)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;*/
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = parse_dir(name);
  char* file_name = parse_file(name);
  struct inode *inode = NULL;
  if(dir == NULL)
  {
	free(file_name);
	return NULL;
  }
  
  if(strcmp(file_name,"/") == 0)
  {
	dir = dir_open_root();
	free(file_name);
	return (struct file *)dir;
  }
  else if(file_name == "." || (inode_get_inumber(dir_get_inode(dir))==ROOT_DIR_SECTOR && strlen(file_name) == 0))
  {
	free(file_name);
	return (struct file *)dir;
  }
  else if(file_name == "..")
  {
	inode = inode_open(inode_get_parent(dir_get_inode(dir)));
	free(file_name);
	if(inode = NULL)
		return NULL;
	return (struct file *)dir_open(inode);
  }
  else
  {
	dir_lookup (dir, file_name, &inode);
 	if(inode == NULL)
	{
		free(file_name);
		return NULL;
	}
  }
  dir_close (dir);

  free(file_name);
 
  if(inode_isdir(inode))
	return (struct file *)dir_open(inode);
  else
	return file_open (inode);

/*
  struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);*/
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = parse_dir(name);
  char * file_name = parse_file(name);
  bool success = dir != NULL && dir_remove (dir, file_name);
  dir_close (dir); 
  free(file_name);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
