#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "filesystem.h"
#include "util.h"


/* Consts in random order that came to mind */
#define MAGIC "Weragin7"
#define ROOT_INODE 0 // inode 0 is reserved for root directory. dirent with inode 0 is considered empty
#define EXTENTS_IN_INDIRECT 15
#define DIRENT_SIZE (4 + MAX_FILENAME + 1)
#define ADDRESS_INVALID UINT32_MAX
#define BITS_PER_SECTOR (SECTOR_SIZE * 8)

/* file_t info items */
#define FILE_T_OFFSET 0

uint32_t foo() {
	return 47;
}

// Legacy zero sector, which was used to store information about the single file in the filesystem, if it existed.
typedef struct zero_sector_t {
	uint8_t magic[8];
	uint32_t bitmap_size;
	uint32_t name;
	uint32_t size;

} zero_sector_t;

// 128B superblock
typedef struct superblock_t {
	uint8_t magic[8]; // 8B 

	uint32_t total_sectors; // +4B = 12B
	uint32_t inode_count; // 4B usually 4*number of inode sectors = 16B

	uint32_t inode_bitmap_start; // +4B
	uint32_t inode_bitmap_sectors; // +4B = 24B

	uint32_t block_bitmap_start; // 4B
	uint32_t block_bitmap_sectors; // 4B = 32B
	
	uint32_t inode_table_start; // 4B
	uint32_t inode_table_sectors; // 4B = 40B

	uint32_t data_start; // 4B
	uint32_t data_sectors; // 4B = 48B; 

} superblock_t;

_Static_assert(sizeof(superblock_t) <= SECTOR_SIZE, "superblock too large");

typedef struct bitmap_sector_t {
	uint8_t bitmap[SECTOR_SIZE];
} bitmap_sector_t;

// 8B extent
typedef struct extent_t {
	uint32_t start;
	uint32_t length; 
} extent_t;

typedef struct indirect_extent_block_t {
	uint32_t next; // 4B pointer to next indirect block, 0 if none
	extent_t extents[EXTENTS_IN_INDIRECT]; // 15*8B = 120B, 124B total
} indirect_extent_block_t;

typedef struct inode_t {
	fs_stat_t stat; // 12B
	extent_t extents[2]; // 16B
	uint32_t indirect_extent_block; // 4B
} inode_t;

typedef struct inode_sector_t {
	inode_t inodes[4]; // 4*32B = 128B
} inode_sector_t;

typedef struct data_sector_t {
	uint8_t data[SECTOR_SIZE];
} data_sector_t;

typedef struct dirent_t {
	uint32_t inode; // 4B
	char name[MAX_FILENAME + 1]; // 13B
} dirent_t;


// ================================================
// Global data
// ================================================

static bool fs_mounted = false;
static superblock_t fs_data;

// ================================================
// Helper functions
// ================================================

static inline size_t ceil_div(size_t a, size_t b) {
	return (a + b - 1) / b;
}

static inline void write_u32_le(uint8_t *p, uint32_t v) {
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

static inline uint32_t read_u32_le(const uint8_t *p) {
	return (uint32_t)p[0] | 
		   ((uint32_t)p[1] << 8) | 
		   ((uint32_t)p[2] << 16) | 
		   ((uint32_t)p[3] << 24);
}


// ================================================
// Packing and unpacking of on-disk structures 
// ================================================

void fs_stat_pack(uint8_t *p, const fs_stat_t *stat) {
	write_u32_le(p, stat->st_size);
	write_u32_le(p + 4, stat->st_nlink);
	write_u32_le(p + 8, stat->st_type);
}

void fs_stat_unpack(const uint8_t *p, fs_stat_t *stat) {
	stat->st_size  = read_u32_le(p);
	stat->st_nlink = read_u32_le(p + 4);
	stat->st_type  = read_u32_le(p + 8);
}

void superblock_pack(uint8_t sector[SECTOR_SIZE], const superblock_t *sb) {
	memset(sector, 0, 128);

    memcpy(sector + 0, sb->magic, 8);

    write_u32_le(sector + 8,  sb->total_sectors);
    write_u32_le(sector + 12, sb->inode_count);

    write_u32_le(sector + 16, sb->inode_bitmap_start);
    write_u32_le(sector + 20, sb->inode_bitmap_sectors);

    write_u32_le(sector + 24, sb->block_bitmap_start);
    write_u32_le(sector + 28, sb->block_bitmap_sectors);

    write_u32_le(sector + 32, sb->inode_table_start);
    write_u32_le(sector + 36, sb->inode_table_sectors);

    write_u32_le(sector + 40, sb->data_start);
    write_u32_le(sector + 44, sb->data_sectors);
}

void superblock_unpack(const uint8_t sector[SECTOR_SIZE], superblock_t *sb) {
	memcpy(sb->magic, sector + 0, 8);

    sb->total_sectors        = read_u32_le(sector + 8);
    sb->inode_count          = read_u32_le(sector + 12);

    sb->inode_bitmap_start   = read_u32_le(sector + 16);
    sb->inode_bitmap_sectors = read_u32_le(sector + 20);

    sb->block_bitmap_start   = read_u32_le(sector + 24);
    sb->block_bitmap_sectors = read_u32_le(sector + 28);

    sb->inode_table_start    = read_u32_le(sector + 32);
    sb->inode_table_sectors  = read_u32_le(sector + 36);

    sb->data_start           = read_u32_le(sector + 40);
    sb->data_sectors         = read_u32_le(sector + 44);
}

void extent_pack(uint8_t *p, const extent_t *ext) {
	write_u32_le(p, ext->start);
	write_u32_le(p + 4, ext->length);
}

void extent_unpack(const uint8_t *p, extent_t *ext) {
	ext->start = read_u32_le(p);
	ext->length = read_u32_le(p + 4);
}

void inode_pack(uint8_t *p, const inode_t *inode) {
	fs_stat_pack(p, &inode->stat);
	
	extent_pack(p + 12, &inode->extents[0]);
	extent_pack(p + 20, &inode->extents[1]);

	write_u32_le(p + 28, inode->indirect_extent_block);
}

void inode_unpack(const uint8_t *p, inode_t *inode) {
	fs_stat_unpack(p, &inode->stat);

	extent_unpack(p + 12, &inode->extents[0]);
	extent_unpack(p + 20, &inode->extents[1]);

	inode->indirect_extent_block = read_u32_le(p + 28);
}

void indirect_extent_pack(uint8_t *p, const indirect_extent_block_t *indirect) {
	write_u32_le(p, indirect->next);

	for (size_t i = 0; i < EXTENTS_IN_INDIRECT; i++) {
		extent_pack(p + 4 + i * 8, &indirect->extents[i]);
	}

	memset(p + 124, 0, 4);
}

void indirect_extent_unpack(const uint8_t *p, indirect_extent_block_t *indirect) {
	indirect->next = read_u32_le(p);

	for (int i = 0; i < EXTENTS_IN_INDIRECT; i++) {
		extent_unpack(p + 4 + i * 8, &indirect->extents[i]);
	}
}

void dirent_pack(uint8_t *p, const dirent_t *entry)
{
    write_u32_le(p, entry->inode);
    memcpy(p + 4, entry->name, MAX_FILENAME + 1);
}

void dirent_unpack(const uint8_t *p, dirent_t *entry)
{
    entry->inode = read_u32_le(p);
    memcpy(entry->name, p + 4, MAX_FILENAME + 1);
    entry->name[MAX_FILENAME] = '\0'; // safety termination
}

// ================================================
// Disk read/write wrappers for on-disk structures
// ================================================

bool superblock_read(superblock_t *sb) {
	uint8_t buf[128];
	hdd_read(0, buf);
	superblock_unpack(buf, sb);
	return true;
}

bool superblock_write(const superblock_t *sb) {
	uint8_t buf[128];
	superblock_pack(buf, sb);
	hdd_write(0, buf);
	return true;
}

bool _bitmap_read(uint32_t sector_index, bitmap_sector_t *bitmap) {
	hdd_read(sector_index, bitmap);
	return true;
}

bool _bitmap_write(uint32_t sector_index, const bitmap_sector_t *bitmap) {
	hdd_write(sector_index, bitmap);
	return true;
}

/**
 * @brief Reads the bitmap sector given by its index within the inode bitmap (not the absolute sector index on disk). Returns true on success, false on failure.
 */
bool inode_bitmap_read(uint32_t bitmap_sector_index, bitmap_sector_t *bitmap) {
	if (bitmap_sector_index >= fs_data.inode_bitmap_sectors) {
		return false;
	}

	uint32_t sector_index = fs_data.inode_bitmap_start + bitmap_sector_index;
	_bitmap_read(sector_index, bitmap);
	return true;
}

/**
 * @brief Writes to the bitmap sector given by its index within the inode bitmap (not the absolute sector index on disk). Returns true on success, false on failure.
 */
bool inode_bitmap_write(uint32_t bitmap_sector_index, const bitmap_sector_t *bitmap) {
	if (bitmap_sector_index >= fs_data.inode_bitmap_sectors) {
		return false;
	}

	uint32_t sector_index = fs_data.inode_bitmap_start + bitmap_sector_index;
	_bitmap_write(sector_index, bitmap);
	return true;
}

/**
 * @brief Reads the bitmap sector given by its index within the data bitmap (not the absolute sector index on disk). Returns true on success, false on failure (e.g. if the index is out of bounds).
 */
bool block_bitmap_read(uint32_t bitmap_sector_index, bitmap_sector_t *bitmap) {
	if (bitmap_sector_index >= fs_data.block_bitmap_sectors) {
		return false;
	}

	uint32_t sector_index = fs_data.block_bitmap_start + bitmap_sector_index;
	_bitmap_read(sector_index, bitmap);
	return true;
}

/**
 * @brief Writes to the bitmap sector given by its index within the data bitmap (not the absolute sector index on disk). Returns true on success, false on failure (e.g. if the index is out of bounds).
 */
bool block_bitmap_write(uint32_t bitmap_sector_index, const bitmap_sector_t *bitmap) {
	if (bitmap_sector_index >= fs_data.block_bitmap_sectors) {
		return false;
	}

	uint32_t sector_index = fs_data.block_bitmap_start + bitmap_sector_index;
	_bitmap_write(sector_index, bitmap);
	return true;
}

bool data_read(uint32_t data_sector_index, data_sector_t *data) {
	if (data_sector_index >= fs_data.data_sectors) {
		return false;
	}

	uint32_t sector_index = fs_data.data_start + data_sector_index;

	hdd_read(sector_index, data);
	return true;
}

bool data_write(uint32_t data_sector_index, const data_sector_t *data) {
	if (data_sector_index >= fs_data.data_sectors) {
		return false;
	}

	uint32_t sector_index = fs_data.data_start + data_sector_index;

	hdd_write(sector_index, data);
	return true;
}

bool inode_read(uint32_t inode_index, inode_t *out) {
	if (inode_index >= fs_data.inode_count) {
		return false;
	}

	uint32_t sector = fs_data.inode_table_start + (inode_index / 4);
	uint32_t offset = inode_index % 4;

	uint8_t buf[128];
	hdd_read(sector, buf);

	inode_unpack(buf + offset * 32, out);

	return true;
}

bool inode_write(uint32_t inode_index, const inode_t *inode) {
	if (inode_index >= fs_data.inode_count) {
		return false;
	}

	uint32_t sector = fs_data.inode_table_start + (inode_index / 4);
	uint32_t offset = inode_index % 4;

	uint8_t buf[128];
	hdd_read(sector, buf);

	inode_pack(buf + offset * 32, inode);

	hdd_write(sector, buf);

	return true;
}

bool indirect_extent_read(uint32_t block_index, const indirect_extent_block_t *indirect) {
	uint8_t buf[SECTOR_SIZE];

	uint32_t sector_index = fs_data.data_start + block_index;
	hdd_read(sector_index, buf);

	indirect_extent_unpack(buf, indirect);

	return true;
}

bool indirect_extent_write(uint32_t block_index, const indirect_extent_block_t *indirect) {
	uint8_t buf[SECTOR_SIZE];

	indirect_extent_pack(buf, indirect);

	uint32_t sector_index = fs_data.data_start + block_index;
	hdd_write(sector_index, buf);

	return true;
}

// ================================================
// Bitmap manipulation
// ================================================

static inline void bitmap_set(bitmap_sector_t *bitmap, uint32_t index) {
	bitmap->bitmap[index / 8] |= (1 << (index % 8));
}

static inline void bitmap_unset(bitmap_sector_t *bitmap, uint32_t index) {
	bitmap->bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool bitmap_get(const bitmap_sector_t *bitmap, uint32_t index) {
	return ((bitmap->bitmap[index / 8] >> (index % 8)) & 1) != 0;
}

uint32_t bitmap_find_free(const bitmap_sector_t *bitmap, uint32_t limit) {
	if (limit > BITS_PER_SECTOR) {
		limit = BITS_PER_SECTOR;
	}

	for (uint32_t i = 0; i < limit; i++) {
		if (!bitmap_get(bitmap, i)) {
			return i;
		}
	}
	return ADDRESS_INVALID;
}

// Block and inode index to bitmap sector/offset calculations

static uint32_t bitmap_sector_of_block(uint32_t block_index) {
	return (block_index / BITS_PER_SECTOR); 
}

static uint32_t bitmap_sector_of_inode(uint32_t inode_index) {
	return (inode_index / BITS_PER_SECTOR);
}

static uint32_t bitmap_offset_of_block(uint32_t index) {
	return index % BITS_PER_SECTOR;
}

static uint32_t bitmap_offset_of_inode(uint32_t index) {
	return index % BITS_PER_SECTOR;
}


// Free finders

/**
 * Finds a free data block using the block bitmap.
 * @return logical index of the free block, or ADDRESS_INVALID if no free block is available
 */
uint32_t data_find_free() {
	for (uint32_t i = 0; i < fs_data.block_bitmap_sectors; i++) {
		bitmap_sector_t bitmap;
		block_bitmap_read(i, &bitmap);

		uint32_t free = bitmap_find_free(&bitmap, fs_data.data_sectors - i * BITS_PER_SECTOR);

		if (free != ADDRESS_INVALID && free < fs_data.data_sectors) {
			return i * BITS_PER_SECTOR + free;
		}
	}
	return ADDRESS_INVALID;
}

uint32_t inode_find_free() {
	for (uint32_t i = 0; i < fs_data.inode_bitmap_sectors; i++) {
		bitmap_sector_t bitmap;
		inode_bitmap_read(i, &bitmap);

		uint32_t free = bitmap_find_free(&bitmap, fs_data.inode_count - i * BITS_PER_SECTOR);

		if (free != ADDRESS_INVALID && free < fs_data.inode_count) {
			return i * BITS_PER_SECTOR + free;
		}
	}
	return ADDRESS_INVALID;
}

// ================================================
// inode allocation
// ================================================

bool st_type_check_type(uint32_t type) {
	switch(type) {
		case STAT_TYPE_FILE:
		case STAT_TYPE_DIR:
		case STAT_TYPE_SYMLINK:
			break;
		default:
    		return false;
	}
	return true;
}

bool inode_init(uint32_t type, inode_t *inode) {
	if (!st_type_check_type(type)) {
		return false;
	}

	inode->stat.st_size = 0;
	inode->stat.st_nlink = 1;
	inode->stat.st_type = type;
	inode->extents[0].start = 0;
	inode->extents[0].length = 0;
	inode->extents[1].start = 0;
	inode->extents[1].length = 0;
	inode->indirect_extent_block = 0;

	return true;
}

uint32_t inode_alloc_first(uint32_t type) {
	if (!st_type_check_type(type)) {
		return false;
	}

	uint32_t free_inode = inode_find_free();

	if (free_inode == ADDRESS_INVALID) {
		return ADDRESS_INVALID;
	}
	
	// initiate inode on disk
	inode_t inode;
	if (!inode_init(type, &inode)) {
		// should not happen, since we check type before
		fprintf(stderr, "Function inode_alloc_first: inode_init returned error when called with type %u. This might be a bug in either of the functions, as both check type validity!\n", type);
		return ADDRESS_INVALID;
	}

	inode_write(free_inode, &inode);

	// mark inode in bitmap
	bitmap_sector_t bitmap;
	uint32_t bitmap_sector_index = bitmap_sector_of_inode(free_inode);
	uint32_t offset = bitmap_offset_of_inode(free_inode);

	inode_bitmap_read(bitmap_sector_index, &bitmap);
	bitmap_set(&bitmap, offset);
	inode_bitmap_write(bitmap_sector_index, &bitmap);

	
	return free_inode;
}

uint32_t inode_alloc(uint32_t type) {
	return inode_alloc_first(type);
}

int inode_free(uint32_t inode_index) {
	if (inode_index >= fs_data.inode_count) {
		return FAIL;
	}

	inode_t inode;

	if (!inode_read(inode_index, &inode)) {
		return FAIL;
	}

	if (inode.stat.st_nlink != 0) {
		return FAIL; 
	}

	// TODO: free data blocks used by the inode

	memset(&inode, 0, sizeof(inode_t));
	inode_write(inode_index, &inode);

	bitmap_sector_t bitmap;
	uint32_t bitmap_sector_index = bitmap_sector_of_inode(inode_index);
	uint32_t offset = bitmap_offset_of_inode(inode_index);


	inode_bitmap_read(bitmap_sector_index, &bitmap);
	bitmap_unset(&bitmap, offset);
	inode_bitmap_write(bitmap_sector_index, &bitmap);

	return OK;
}

int inode_release(uint32_t inode_index)
{
    if (inode_index >= fs_data.inode_count) {
        return FAIL;
    }

    inode_t inode;
    if (!inode_read(inode_index, &inode)) {
        return FAIL;
    }

    if (inode.stat.st_nlink == 0) {
        return OK; // already released
    }

	if (inode.stat.st_nlink == 1 && inode.stat.st_type == STAT_TYPE_DIR) {
		if (!dir_is_empty(inode_index)) {
			// do not release non-empty directories
			return FAIL;
		}
	}

    inode.stat.st_nlink--;
    inode_write(inode_index, &inode);

    if (inode.stat.st_nlink > 0) {
        return OK;
    }

    return inode_free(inode_index);
}

// ================================================
// Data block allocation
// ================================================

void block_empty(data_sector_t *block) {
	memset(block, 0, sizeof(data_sector_t));
}

uint32_t block_alloc() {
	uint32_t free_block_index = data_find_free();

	if (free_block_index == ADDRESS_INVALID) {
		return ADDRESS_INVALID;
	}

	data_sector_t block;
	block_empty(&block);
	data_write(free_block_index, &block);

	bitmap_sector_t bitmap;
	uint32_t sector = bitmap_sector_of_block(free_block_index);
	uint32_t offset = bitmap_offset_of_block(free_block_index);

	block_bitmap_read(sector, &bitmap);
	bitmap_set(&bitmap, offset);
	block_bitmap_write(sector, &bitmap);

	return free_block_index;
}

int block_free(uint32_t block_index) {
	if (block_index >= fs_data.data_sectors) {
		return FAIL;
	}

	bitmap_sector_t bitmap;
	uint32_t sector = bitmap_sector_of_block(block_index);
	uint32_t offset = bitmap_offset_of_block(block_index);

	block_bitmap_read(sector, &bitmap);
	if (!bitmap_get(&bitmap, offset)) {
		return FAIL;
	}

	bitmap_unset(&bitmap, offset);
	block_bitmap_write(sector, &bitmap);
	
	data_sector_t block;
	block_empty(&block);
	data_write(block_index, &block);

	return OK;
}

// ================================================
// Extent management
// ================================================

uint32_t extent_get_block(const extent_t *extent, uint32_t logical_block) {
	if (logical_block >= extent->length) {
		return ADDRESS_INVALID;
	}
	return extent->start + logical_block;
}

uint32_t indirect_get_block(const indirect_extent_block_t *indirect, uint32_t logical_block) {
	uint32_t remaining = logical_block;
	for (int i = 0; i < EXTENTS_IN_INDIRECT; i++) {
		extent_t curr_extent = indirect->extents[i];

		uint32_t block_index = extent_get_block(&curr_extent, remaining);
		if (block_index != ADDRESS_INVALID) {
			return block_index;
		}

		if (remaining <= indirect->extents[i].length) {
			return ADDRESS_INVALID;
		}

		remaining -= indirect->extents[i].length;
	}

	return remaining;
}

uint32_t inode_get_block(const inode_t *inode, uint32_t logical_block) {

}

uint32_t inode_append_block(const inode_t *inode) {

}

uint32_t inode_truncate(const inode_t *inode) {

}



// ================================================
// Directory management
// ================================================

uint32_t dir_lookup(uint32_t dir_inode_index, const char *name) {
	inode_t dir_inode;

	if (!inode_read(dir_inode_index, &dir_inode)) {
		return ADDRESS_INVALID;
	}

	if (dir_inode.stat.st_type != STAT_TYPE_DIR) {
		return ADDRESS_INVALID;
	}

	size_t entries = dir_inode.stat.st_size / DIRENT_SIZE;

	size_t seen = 0;

	// read direct extents. indirect extents will be implemented later
	for (size_t extent_index = 0; extent_index < 2; extent_index++) {
		extent_t extent = dir_inode.extents[extent_index];

		if (extent.length == 0) {
			continue;
		}

		for (uint32_t sector_index = 0;
			 sector_index < extent.length;
			 sector_index++) {
			
			data_sector_t data_sector;

			data_read(extent.start + sector_index, &data_sector);

			size_t entries_in_sector = SECTOR_SIZE / DIRENT_SIZE;

			for (size_t i = 0; 
				 i < entries_in_sector;
				 i++) {
				
				if (seen >= entries) {
					return ADDRESS_INVALID;
				}

				uint8_t *entry_data = data_sector.data + i * DIRENT_SIZE;

				dirent_t entry;
				dirent_unpack(entry_data, &entry);

				_Static_assert(false); // TODO: check code below with dir_is_empty and size logic

				if (entry.inode != ROOT_INODE && 
					strcmp(entry.name, entry.name) == 0) {

					return entry.inode;
				}

				seen++;
			}
		}
	}

	return ADDRESS_INVALID;
}

bool dir_is_empty(uint32_t dir_inode_index) {
	inode_t dir_inode;

	if (!inode_read(dir_inode_index, &dir_inode)) {
		return false;
	}

	if (dir_inode.stat.st_type != STAT_TYPE_DIR) {
		return false;
	}

	_Static_assert(false); // Check size logic with dir_lookup code
	return dir_inode.stat.st_size == 0;
}



// ================================================
// Mounting and checking
// ================================================

bool superblock_check(const superblock_t *sector)
{
	return !memcmp(sector->magic, MAGIC, 8);
}

bool fs_mount() {
	if (fs_mounted) {
		return true;
	}

	superblock_read(&fs_data);

	if (!superblock_check(&fs_data)) {
		fprintf(stderr, "Invalid filesystem on disk\n");
		return false;
	}

	fs_mounted = true;
	return true;
}


// ================================================
// Filesystem API
// ================================================

/**
 * Naformatovanie disku.
 *
 * Zavola sa vzdy, ked sa vytvara novy obraz disku.
 */
void fs_format()
{
	size_t size = hdd_size();
	size_t total_sectors = size / SECTOR_SIZE;

	superblock_t superblock;
	memcpy(superblock.magic, MAGIC, 8);
	superblock.total_sectors = (uint32_t)total_sectors;

	// inode count calculation

	size_t inode_table_sectors = total_sectors / 8;
	if (inode_table_sectors < 1) {
		inode_table_sectors = 1;
	}

	superblock.inode_count = (uint32_t)(inode_table_sectors * 4); // 4 inodes per sector
 
	// inode bitmap (1 bit per inode)

	size_t inode_bitmap_bits = superblock.inode_count;
	size_t inode_bitmap_bytes = ceil_div(inode_bitmap_bits, 8);
	size_t inode_bitmap_sectors = ceil_div(inode_bitmap_bytes, SECTOR_SIZE);

	superblock.inode_bitmap_start = 1;
	superblock.inode_bitmap_sectors = (uint32_t)inode_bitmap_sectors;

	// block bitmap (1 bit per data block, depends on remaining space)

	superblock.block_bitmap_start = superblock.inode_bitmap_start + superblock.inode_bitmap_sectors;
	size_t estimated_data_sectors = total_sectors - 1 - superblock.inode_bitmap_sectors - inode_table_sectors;
	
	size_t block_bitmap_bits = estimated_data_sectors;
	size_t block_bitmap_bytes = ceil_div(block_bitmap_bits, 8);
	size_t block_bitmap_sectors = ceil_div(block_bitmap_bytes, SECTOR_SIZE);
	superblock.block_bitmap_sectors = (uint32_t)block_bitmap_sectors;

	// inode table

	superblock.inode_table_start = superblock.block_bitmap_start + superblock.block_bitmap_sectors;
	superblock.inode_table_sectors = (uint32_t)inode_table_sectors;

	// data region

	superblock.data_start = superblock.inode_table_start + superblock.inode_table_sectors;

	if (superblock.data_start >= total_sectors) {
		superblock.data_sectors = 0;
	} else {
		superblock.data_sectors = total_sectors - superblock.data_start;
	}

	if (superblock.data_start > total_sectors) {
		fprintf(stderr, "Not enough space for filesystem structures. Total sectors: %lu, required for superblock + bitmaps + inode table: %u\n", total_sectors, 1 + superblock.inode_bitmap_sectors + superblock.block_bitmap_sectors + superblock.inode_table_sectors);
		return;
	}

	uint8_t buf[SECTOR_SIZE] = { 0 };
	superblock_pack(buf, &superblock);
	hdd_write(0, buf);

	uint8_t zero[SECTOR_SIZE] = {0};
	for (size_t i = 1; i < total_sectors; i++) {
		hdd_write(i, zero);
	}

	// write root inode
	inode_t root;

	root.stat.st_size = 0;
	root.stat.st_nlink = 1;
	root.stat.st_type = STAT_TYPE_DIR;

	root.extents[0].start = 0;
	root.extents[0].length = 0;

	root.extents[1].start = 0;
	root.extents[1].length = 0;

	root.indirect_extent_block = 0;

	inode_write(ROOT_INODE, &root);
}



/**
 * Vytvorenie suboru.
 *
 * Volanie vytvori v suborovom systeme na zadanej ceste novy subor a vrati
 * handle nan. Ak subor uz existoval, bude skrateny na prazdny. Pozicia v subore bude
 * nastavena na 0ty byte. Ak adresar, v ktorom subor ma byt ulozeny, neexistuje,
 * vrati NULL (sam nevytvara adresarovu strukturu, moze vytvarat iba subory).
 */

file_t *fs_creat(const char *path)
{
	if (!fs_mount()) {
		return NULL;
	}

	/* Nepodporujeme adresare */
	

	if (strrchr(path, PATHSEP) != path)
		return NULL;

	uint8_t buffer[SECTOR_SIZE] = { 0 };

	/* Skontrolujeme, ci uz nahodou na disku nie je subor */
	hdd_read(0, buffer);

	if (buffer[0] != 0 ) {
		/* Nemozeme vytvorit dalsi subor, ak nema rovnake meno, ako uz existujuci
		 */
		if (strncmp(path, (char*)buffer, MAX_FILENAME))
			return NULL;

		/* Skratime subor na 0 bajtov */
		((zero_sector_t*)buffer)->name[0] = 0;
		hdd_write(0, buffer);
	}

	/* Vsetko ok, pripravime informacie pre zapis do nulteho sektora */

	/* Meno suboru je na zaciatku*/
	strcpy((char*)buffer, path);

	/* Za castou vyhradenou pre meno je ulozena velkost suboru */
	*((uint32_t*)(buffer + MAX_FILENAME)) = 0;

	/* Zapiseme informacie o novovytvorenom subore na disk */
	hdd_write(0, buffer);

	return fs_open(path);
}


/**
 * Otvorenie existujuceho suboru.
 *
 * Ak zadany subor existuje, funkcia ho otvori a vrati handle nan. Pozicia v
 * subore bude nastavena na 0-ty bajt. Ak subor neexistuje, vrati NULL.
 */
file_t *fs_open(const char *path)
{
	if (!fs_mount()) {
		return NULL;
	}

	uint8_t buffer[SECTOR_SIZE];

	hdd_read(0, &buffer);

	/* Skontrolujeme, ci v prvom sektore je ulozene meno nasho suboru */
	if (memcmp(buffer, path, strnlen(path,12)))
			return NULL;

	/* Subor existuje, alokujeme pren deskriptor */
	file_t *fd = fd_alloc();

	/* mame iba jeden jediny subor, deskriptor vyplnime samymi nulami */
	fd->info[FILE_T_OFFSET] = 0;
	fd->info[1] = 0;
	fd->info[2] = 0;
	fd->info[3] = 0;

	return fd;
}

/**
 * Zatvori otvoreny file handle.
 *
 * Funkcia zatvori handle, ktory bol vytvoreny pomocou volania 'open' alebo
 * 'creat' a uvolni prostriedky, ktore su s nim spojene. V pripade akehokolvek
 * zlyhania vrati FAIL, inak OK.
 */
int fs_close(file_t *fd)
{
	/* Uvolnime filedescriptor, aby sme neleakovali pamat */
	if (!fs_mount()) {
		return FAIL;
	}
	
	fd_free(fd);
	return OK;
}

/**
 * Odstrani subor na ceste 'path'.
 *
 * Ak zadana cesta existuje a je to subor, odstrani subor z disku; nemeni
 * adresarovu strukturu. V pripade chyby vracia FAIL, inak OK.
 */
int fs_unlink(const char *path)
{
	if (!fs_mount()) {
		return FAIL;
	}

	return FAIL;
}

/**
 * Premenuje/presunie polozku v suborovom systeme z 'oldpath' na 'newpath'.
 *
 * Po uspesnom vykonani tejto funkcie bude subor, ktory doteraz existoval na
 * 'oldpath' dostupny cez 'newpath' a 'oldpath' prestane existovat. Opat,
 * funkcia nemanipuluje s adresarovou strukturou (nevytvara nove adresare
 * z cesty newpath, okrem posledneho v pripade premenovania adresara).
 * V pripade zlyhania vracia FAIL, inak OK.
 */
int fs_rename(const char *oldpath, const char *newpath)
{
	if (!fs_mount()) {
		return FAIL;
	}

	return FAIL;
}

/**
 * Nacita z aktualnej pozicie vo 'fd' do bufferu 'bytes' najviac 'size' bajtov.
 *
 * Z aktualnej pozicie v subore precita funkcia najviac 'size' bajtov; na konci
 * suboru funkcia vracia 0. Po nacitani dat zodpovedajuco upravi poziciu v
 * subore. Vrati pocet precitanych bajtov z 'bytes', alebo FAIL v pripade
 * zlyhania. Existujuci subor prepise.
 */
int fs_read(file_t *fd, uint8_t *bytes, size_t size)
{
	if (!fs_mount()) {
		return FAIL;
	}

	/* Podporujeme iba subory s maximalnou velkostou SECTOR_SIZE */
	uint8_t buffer[SECTOR_SIZE] = { 0 };
	/* Vo filedescriptore je ulozena nasa aktualna pozicia v subore */
	int offset = fd->info[FILE_T_OFFSET];

	/* Nacitame celkovu velkost suboru na disku */
	hdd_read(0, buffer);
	size_t file_size = ((zero_sector_t*)buffer)->size;

	hdd_read(1, buffer);
	size_t i;
	for (i = 0; (i < size) && ((i + offset) < file_size); i++) {
		bytes[i] = buffer[offset + i];
	}

	/* Aktualizujeme offset, na ktorom sme teraz */
	fd->info[FILE_T_OFFSET] += i;

	/* Vratime pocet precitanych bajtov */
	return i;
}

/**
 * Zapise do 'fd' na aktualnu poziciu 'size' bajtov z 'bytes'.
 *
 * Na aktualnu poziciu v subore zapise 'size' bajtov z 'bytes'. Ak zapis
 * presahuje hranice suboru, subor sa zvacsi; ak to nie je mozne, zapise sa
 * maximalny mozny pocet bajtov. Po zapise korektne upravi aktualnu poziciu v
 * subore a vracia pocet zapisanych bajtov z 'bytes'.
 * V pripade zlyhania vrati FAIL.
 *
 * Write existujuci obsah suboru prepisuje, nevklada dovnutra nove data.
 * Write pre poziciu tesne za koncom existujucich dat zvacsi velkost suboru.
 */
int fs_write(file_t *fd, const uint8_t *bytes, size_t size)
{
	if (!fs_mount()) {
		return FAIL;
	}

	uint8_t buffer[SECTOR_SIZE] = { 0 };
	/* Vo filedescriptore je ulozena nasa aktualna pozicia v subore */
	int offset = fd->info[FILE_T_OFFSET];

	/* Nacitame celkovu velkost suboru na disku */
	hdd_read(0, buffer);
	size_t file_size = ((zero_sector_t*)buffer)->size;

	/* Nacitame stare data do buffera a prepiseme ich novymi */
	hdd_read(1, buffer);
	size_t i;
	for (i = 0; (i < size) && ((i + offset) < SECTOR_SIZE); i++) {
		buffer[offset + i] = bytes[i];
	}
	hdd_write(1, buffer);

	/* Ak subor narastol, aktualizujeme velkost */

	if (file_size < offset + i) {
		hdd_read(0, buffer);
		((zero_sector_t*)buffer)->size = offset + i;
		hdd_write(0, buffer);
	}

	/* Aktualizujeme offset, na ktorom sme */
	fd->info[FILE_T_OFFSET] += i;

	/* Vratime pocet zapisanych bajtov */
	return i;
}

/**
 * Zmeni aktualnu poziciu v subore na 'pos'-ty byte.
 *
 * Upravi aktualnu poziciu; ak je 'pos' mimo hranic suboru, vrati FAIL a pozicia
 * sa nezmeni, inac vracia OK.
 */
int fs_seek(file_t *fd, size_t pos)
{
	if (!fs_mount()) {
		return FAIL;
	}

	uint8_t buffer[SECTOR_SIZE] = { 0 };

	/* Nacitaj velkost suboru z disku */
	hdd_read(0, buffer);
	size_t file_size = ((zero_sector_t*)buffer)->size;

	/* Nemozeme seekovat za velkost suboru */
	if (pos > file_size) {
		fprintf(stderr, "Can not seek: %lu > %lu\n", pos, file_size);
		return FAIL;
	}

	fd->info[FILE_T_OFFSET] = pos;

	return OK;
}


/**
 * Vrati aktualnu poziciu v subore.
 */

size_t fs_tell(file_t *fd) {
	if (!fs_mount()) {
		return FAIL;
	}

	return fd->info[FILE_T_OFFSET];
}


/**
 * Vrati informacie o 'path'.
 *
 * Funkcia vrati FAIL ak cesta neexistuje, alebo vyplni v strukture 'fs_stat'
 * polozky a vrati OK:
 *  - st_size: velkost suboru v byte-och
 *  - st_nlink: pocet hardlinkov na subor (ak neimplementujete hardlinky, tak 1)
 *  - st_type: hodnota podla makier v hlavickovom subore: STAT_TYPE_FILE,
 *  STAT_TYPE_DIR, STAT_TYPE_SYMLINK
 *
 */

int fs_stat(const char *path, struct fs_stat *fs_stat) { 
	if (!fs_mount()) {
		return FAIL;
	}

	uint8_t buffer[SECTOR_SIZE] = { 0 };
	
	/* Nacitaj velkost suboru z disku */
	hdd_read(0, buffer);

	/* Ak subor neexistuje, FAIL */
	if (buffer[0] == 0)
		return FAIL;

	size_t file_size = ((zero_sector_t*)buffer)->size;
	fs_stat->st_size = file_size;
	fs_stat->st_nlink = 1;
	fs_stat->st_type = STAT_TYPE_FILE;

	return OK;
}

/* Level 3 */
/**
 * Vytvori adresar 'path'.
 *
 * Ak cesta, v ktorej adresar ma byt, neexistuje, vrati FAIL (vytvara najviac
 * jeden adresar), pri korektnom vytvoreni OK.
 */
int fs_mkdir(const char *path) {  
	if (!fs_mount()) {
		return FAIL;
	} }

/**
 * Odstrani adresar 'path'.
 *
 * Odstrani prazdny adresar, na ktory ukazuje 'path'; ak obsahuje subory, neexistuje alebo nie je
 * adresar, vrati FAIL; po uspesnom dokonceni vrati OK.
 */
int fs_rmdir(const char *path) { 	
	if (!fs_mount()) {
		return FAIL;
	}; 
}

/**
 * Otvori adresar 'path' (na citanie poloziek)
 *
 * Vrati handle na otvoreny adresar s poziciou nastavenou na 0; alebo NULL v
 * pripade zlyhania.
 */
file_t *fs_opendir(const char *path) { 
	if (!fs_mount()) {
		return FAIL;
	}
 }

/**
 * Nacita nazov dalsej polozky z adresara.
 *
 * Do dodaneho buffera ulozi nazov polozky v adresari, posunie aktualnu
 * poziciu na dalsiu polozku a vrati OK.
 * V pripade problemu, alebo ak nasledujuca polozka neexistuje, vracia FAIL.
 * (V pripade jedneho suboru v adresari vracia FAIL az pri druhom volani.)
 */
int fs_readdir(file_t *dir, char *item) {
	if (!fs_mount()) {
		return FAIL;
	}
 }

/**
 * Zatvori otvoreny adresar.
 * V pripade neuspechu vrati FAIL, inak OK.
 */
int fs_closedir(file_t *dir) { 
	if (!fs_mount()) {
		return FAIL;
	}
}

/* Level 4 */
/**
 * Vytvori hardlink zo suboru 'path' na 'linkpath'.
 * V pripade neuspechu vrati FAIL, inak OK.
 */
int fs_link(const char *path, const char *linkpath) { 
	if (!fs_mount()) {
		return FAIL;
	}
 }

/**
 * Vytvori symlink z 'path' na 'linkpath'.
 * V pripade neuspechu vrati FAIL, inak OK.
 */
int fs_symlink(const char *path, const char *linkpath) { 
	if (!fs_mount()) {
		return FAIL;
	}
}
