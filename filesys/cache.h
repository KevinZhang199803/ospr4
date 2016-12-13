#include "devices/block.h"
#include <list.h>

struct cache
{
	uint32_t data[128];
	block_sector_t sector;
	bool accessed;
	bool dirty;
	int used;
	struct list_elem elem;
};

struct cache *get_cache (block_sector_t sector);
struct cache *make_cache (block_sector_t sector);
void evict_cache (void);
void close_cache (void);
void write_behind (void);

