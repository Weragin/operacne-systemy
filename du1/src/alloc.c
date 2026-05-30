#include "wrapper.h"



#define HEADER_SIZE 13 // free block header: tag(1) + size(4) + prev(4) + next(4)
#define ALLOC_HEADER_SIZE 5 // allocated block header: tag(1) + size(4)
#define FREE_FLAG 0
#define ALLOC_FLAG 255
#define SIZE_OFFSET 1
#define PREV_OFFSET 5
#define NEXT_OFFSET 9
#define NULL_PTR 0
#define FREE_HEAD_ADDR 0

void write_u32(unsigned int addr, unsigned int val) {
    mwrite(addr, (val >> 24) & 0xFF);
    mwrite(addr + 1, (val >> 16) & 0xFF);
    mwrite(addr + 2, (val >> 8) & 0xFF);
    mwrite(addr + 3, val & 0xFF);
}

unsigned int read_u32(unsigned int addr) {
    return (mread(addr) << 24) | (mread(addr + 1) << 16) | (mread(addr + 2) << 8) | mread(addr + 3);
}

void my_init(void) {
    unsigned int total = msize();
    if (total < HEADER_SIZE + ALLOC_HEADER_SIZE) {
		return;
	}

    write_u32(FREE_HEAD_ADDR, HEADER_SIZE); // free_head points to the first free block
    mwrite(HEADER_SIZE, FREE_FLAG); // flag the first block free
    write_u32(HEADER_SIZE + SIZE_OFFSET, total - HEADER_SIZE); // size
    write_u32(HEADER_SIZE + PREV_OFFSET, 0);
    write_u32(HEADER_SIZE + NEXT_OFFSET, 0);
}

int allocate_block(
	unsigned int current, 
	unsigned int prev, 
	unsigned int block_size, 
	unsigned int required) 
{
	// allocate
	unsigned int remaining = block_size - required;
	if (remaining >= HEADER_SIZE) {
		// split
		unsigned int new_free = current + required;
		mwrite(new_free, FREE_FLAG);
		write_u32(new_free + SIZE_OFFSET, remaining);
		write_u32(new_free + PREV_OFFSET, current);
		write_u32(new_free + NEXT_OFFSET, read_u32(current + NEXT_OFFSET));
		unsigned int next = read_u32(new_free + NEXT_OFFSET);
		if (next != NULL_PTR) write_u32(next + PREV_OFFSET, new_free);
		write_u32(current + NEXT_OFFSET, new_free);
		write_u32(current + SIZE_OFFSET, required);
	}
	// mark allocated
	mwrite(current, ALLOC_FLAG);
	write_u32(current + SIZE_OFFSET, required);
	// remove from free list
	unsigned int next = read_u32(current + NEXT_OFFSET);
	if (prev == NULL_PTR) {
		write_u32(FREE_HEAD_ADDR, next);
	} else {
		write_u32(prev + NEXT_OFFSET, next);
	}
	if (next != NULL_PTR) 
		write_u32(next + PREV_OFFSET, prev);
	return current + ALLOC_HEADER_SIZE;
}

int my_alloc(unsigned int size) {
    if (size == NULL_PTR || size > msize() - ALLOC_HEADER_SIZE)
		return FAIL;
    unsigned int required = ALLOC_HEADER_SIZE + size;
    if (required < HEADER_SIZE) 
		required = HEADER_SIZE; // minimum block size - less would break on free
    unsigned int head = read_u32(FREE_HEAD_ADDR);
    unsigned int current = head;
    unsigned int prev = NULL_PTR;

    while (current != NULL_PTR) {
        unsigned int block_size = read_u32(current + SIZE_OFFSET);
        if (block_size >= required) {
            return allocate_block(current, prev, block_size, required);
        }
        prev = current;
        current = read_u32(current + NEXT_OFFSET);
    }
    return FAIL;
}

int my_free(unsigned int addr) {
    if (addr < ALLOC_HEADER_SIZE || addr >= msize()) return FAIL;
    unsigned int block_addr = addr - ALLOC_HEADER_SIZE;
    if (mread(block_addr) != ALLOC_FLAG) return FAIL;
    unsigned int size = read_u32(block_addr + SIZE_OFFSET);
    // mark free
    mwrite(block_addr, FREE_FLAG);
    // insert into free list
    unsigned int head = read_u32(FREE_HEAD_ADDR);
    if (head == NULL_PTR) {
        write_u32(NULL_PTR, block_addr);
        write_u32(block_addr + PREV_OFFSET, NULL_PTR);
        write_u32(block_addr + NEXT_OFFSET, NULL_PTR);
    } else {
        unsigned int current = head;
        unsigned int prev = NULL_PTR;
        while (current != NULL_PTR && current < block_addr) {
            prev = current;
            current = read_u32(current + NEXT_OFFSET);
        }
        if (prev == NULL_PTR) {
            write_u32(NULL_PTR, block_addr);
            write_u32(block_addr + NEXT_OFFSET, current);
            write_u32(block_addr + PREV_OFFSET, NULL_PTR);
            if (current != NULL_PTR) write_u32(current + PREV_OFFSET, block_addr);
        } else {
            write_u32(prev + NEXT_OFFSET, block_addr);
            write_u32(block_addr + NEXT_OFFSET, current);
            write_u32(block_addr + PREV_OFFSET, prev);
            if (current != NULL_PTR) write_u32(current + PREV_OFFSET, block_addr);
        }
    }
    // coalesce prev
    unsigned int prev_block = read_u32(block_addr + PREV_OFFSET);
    if (prev_block != NULL_PTR && prev_block + read_u32(prev_block + SIZE_OFFSET) == block_addr) {
        unsigned int prev_size = read_u32(prev_block + SIZE_OFFSET);
        write_u32(block_addr + SIZE_OFFSET, size + prev_size);
        write_u32(block_addr + PREV_OFFSET, read_u32(prev_block + PREV_OFFSET));
        unsigned int pprev = read_u32(prev_block + PREV_OFFSET);
        if (pprev != NULL_PTR) write_u32(pprev + NEXT_OFFSET, block_addr);
        else write_u32(NULL_PTR, block_addr);
    }
    // coalesce next
    unsigned int next_block = read_u32(block_addr + NEXT_OFFSET);
    if (next_block != NULL_PTR && block_addr + read_u32(block_addr + SIZE_OFFSET) == next_block) {
        unsigned int next_size = read_u32(next_block + SIZE_OFFSET);
        write_u32(block_addr + SIZE_OFFSET, read_u32(block_addr + SIZE_OFFSET) + next_size);
        write_u32(block_addr + NEXT_OFFSET, read_u32(next_block + NEXT_OFFSET));
        unsigned int nnext = read_u32(next_block + NEXT_OFFSET);
        if (nnext != NULL_PTR) write_u32(nnext + PREV_OFFSET, block_addr);
    }
    return OK;
}
