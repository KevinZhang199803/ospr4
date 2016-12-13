#include <stdio.h>
#include <stdlib.h>
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include <list.h>
#include "devices/timer.h"
#include "threads/thread.h"

struct list cache_list;
int cache_size;
struct lock cache_lock;

struct cache *clock_cache;

void init_cache ()
{
	list_init (&cache_list);
	lock_init (&cache_lock);
	cache_size = 0;
	thread_create ("write_behind", 0, write_behind, NULL);
}

struct cache *get_cache (block_sector_t sector)
{
	lock_acquire (&cache_lock);
	struct list_elem *e;
	struct cache *c;
	c = NULL;
	for(e = list_begin (&cache_list); e != list_end (&cache_list); e = list_next (e))
	{
		c = list_entry (e, struct cache, elem);
		if (c->sector == sector)
		{
			c->used++;
			lock_release (&cache_lock);
			return c;
		}
	}
	if (cache_size == 64)
		evict_cache ();
	cache_size++;
	c = malloc (sizeof (struct cache));
	if (c == NULL)
	{
		lock_release (&cache_lock);
		ASSERT(0);
		return NULL;
	}
	c->sector = sector;
	block_read (fs_device, c->sector, &c->data);
	c->accessed = true;
	c->dirty = false;
	c->used = 1;
	list_push_back (&cache_list, &c->elem);
	if(cache_size == 1)
		clock_cache = c;
	lock_release (&cache_lock);
	return c;
//	return make_cache (sector);
}

struct cache *make_cache (block_sector_t sector)
{
	lock_acquire (&cache_lock);
	struct cache *c;
	if (cache_size == 64)
		evict_cache ();
	cache_size++;
	c = malloc (sizeof (struct cache));
	if (c == NULL)
	{
		lock_release (&cache_lock);
		ASSERT(0);
		return NULL;
	}
	c->sector = sector;
	block_read (fs_device, c->sector, &c->data);
	c->accessed = true;
	c->dirty = false;
	c->used = 0;
	c->used++;
	list_push_back (&cache_list, &c->elem);
	if(cache_size == 1)
		clock_cache = c;
	lock_release (&cache_lock);
	return c;
}

void evict_cache ()
{
	struct list_elem *e;
	struct cache *c;
	if (clock_cache == NULL)
		e = list_begin (&cache_list);
	else
		e = &clock_cache->elem;
//	printf ("start_evict\n");
	while (true)
	{
		c = list_entry (e, struct cache, elem);
		e = list_next (e);
		if (e = list_end (&cache_list))
			e = list_begin (&cache_list);
		if (c->used)
		{
//			printf("used\n");
		}
		else if (c->accessed)
		{
//			printf("accessed\n");
			c->accessed = false;
		}
		else
		{
//			printf("whatTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT\n");
			if (c->dirty)
			{
				block_write (fs_device, c->sector, &c->data);
			}
				clock_cache = list_entry (e, struct cache, elem);
				list_remove (&c->elem);
				free (c);
				cache_size--;
//				printf("end_evict\n");
				return;			
		}
	}
//	printf("end_evict\n");
}

void close_cache ()
{
	struct list_elem *e;
	struct cache *c;
	for (e = list_begin (&cache_list); e != list_end (&cache_list);)
	{
		c = list_entry (e, struct cache, elem);
		e = list_next (e);
		if (c->dirty)
			block_write (fs_device, c->sector, &c->data);
		list_remove (&c->elem);
		free (c);
	}
}

void write_behind ()
{
	struct list_elem *e;
	struct cache *c;
	while (true)
	{
//		printf ("start_write\n");
		timer_sleep (300);
//		printf ("end_sleep\n");
		lock_acquire (&cache_lock);
//		printf ("get lock\n");
		for (e = list_begin (&cache_list); e != list_end (&cache_list); e = list_next (e))
		{
//			printf("writing...\n");
			c = list_entry (e, struct cache, elem);
			if (c->dirty)
			{
				block_write (fs_device, c->sector, &c->data);
				c->dirty = false;
			}
				
		}
		lock_release (&cache_lock);
//		printf ("end_write\n");
	}
}



