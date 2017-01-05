#include "MemoryPool.h"
#include <iostream>
using namespace std;

extern "C"
{
    static TVMMemoryPoolID nextMemID = 1;

    MemoryPool::MemoryPool(void* base, TVMMemorySize size, TVMMemoryPoolIDRef mid)
    {
        // Create Memory pool.
        this->mem_id   = nextMemID++;
        this->mem_base = base;
        this->mem_size = size;

        *mid = this->mem_id;

        // Create new free segment (entire memory pool).
        MemorySegment* segment = new MemorySegment;
        segment->base = this->mem_base;
        segment->size = this->mem_size;
        segment->next = NEXT_BLOCK;

        this->mem_free.push_back(segment);
    }

    MemoryPool::~MemoryPool()
    {
        for(auto it = mem_free.begin(); it != mem_free.end(); ++it)
        {
            delete (*it);
        }
        mem_free.clear();
		
        for(auto it = mem_alloc.begin(); it != mem_alloc.end(); ++it)
        {
            delete (*it);
        }
        mem_alloc.clear();
    }

    TVMMemoryPoolID MemoryPool::getMemID()
    {
        return this->mem_id;
    }

    TVMMemorySize MemoryPool::getMemSize()
    {
        return this->mem_size;
    }

    void* MemoryPool::getMemBase()
    {
        return this->mem_base;
    }

    TVMMemorySize MemoryPool::query_remaining()
    {
        TVMMemorySize msize = 0;

       	for(auto it = mem_free.begin(); it != mem_free.end(); ++it)
        {
            msize += (*it)->size;
        }

        return msize;
    }

    bool MemoryPool::allocate_memory(void** pointer, TVMMemorySize msize)
    {

        bool found = false;

        msize = (msize + 0x3F) & (~0x3F);

        unsigned int numSegments = mem_free.size();

        // Find free space (if possible).
        for(unsigned int i = 0; i < numSegments; i++)
        {
            if(mem_free[i]->size >= msize)
            {
                found = true;
                *pointer = mem_free[i]->base;

                // Allocate block.
                MemorySegment* segment = new MemorySegment;
                segment->base = mem_free[i]->base;
                segment->size = msize;
                segment->next = NEXT_BLOCK;
                mem_alloc.push_back(segment);

                // Update free blocks.
                if(mem_free[i]->size == msize) // Remove free block.
                {
                    delete mem_free[i];
                    mem_free.erase(mem_free.begin() + i);
                }
                else // Shrink free block.
                {
                    mem_free[i]->size -= msize;
                    mem_free[i]->base = (uint8_t*)mem_free[i]->base + msize;
                }
                break;
            }
        }

        return found;
    }

    bool MemoryPool::deallocate_memory(void* base)
    {
        // Check that they aren't feeding us absolute garbage.
        if(base < this->mem_base || (uint8_t*)base >= (uint8_t*)this->mem_base + this->mem_size)
        {
            return false;
        }

        // Check if that base has actually been allocated.
        bool found = false;
        MemorySegment* segment;

        unsigned int numSegments = mem_alloc.size();

        for(unsigned int i = 0; i < numSegments; i++)
        {
            if(mem_alloc[i]->base == base) // Found block
            {
                found = true;
                segment = mem_alloc[i];
                mem_alloc.erase(mem_alloc.begin() + i);
            }
        }

        if(!found)
        {
            return false;
        }

        // Free & Merge Blocks.
        if(mem_free.size() == 0) // No blocks currently free.
        {
            mem_free.push_back(segment);
        }
        else if(mem_free[0]->base > segment->base) // The block being freed is before everything else.
        {
            if(segment->next == mem_free[0]->base) // Right next to each other
            {
                mem_free[0]->base = segment->base;
                mem_free[0]->size += segment->size;

                delete segment;
            }
            else // Before everything
            {
                mem_free.insert(mem_free.begin(), segment);
            }
        }
        else if(mem_free[mem_free.size() - 1]->next <= segment->base) // The block being freed is after everything else.
        {
            if(mem_free[mem_free.size() - 1]->next == segment->base)  // Right after last block.
            {
                mem_free[mem_free.size() - 1]->next = segment->next;
                mem_free[mem_free.size() - 1]->size += segment->size;

                delete segment;
            }
            else
            {
                mem_free.push_back(segment);
            }
        }
        else
        {
            unsigned int pos;
            for(pos = 0; pos < mem_free.size(); pos++)
            {
                if(mem_free[pos]->base > segment->base)
                {
                    break;
                }
            }

            // Gets position, we'll have to insert at.
            // 0 1 x 2 ... where x's base is less than 2's base
            if(mem_free[BEFORE]->next == segment->base
               && mem_free[AFTER]->base == segment->next) // Touching both sides.
            {
                mem_free[BEFORE]->next = mem_free[AFTER]->next;
                mem_free[BEFORE]->size += mem_free[AFTER]->size + segment->size;

                delete segment;
                delete mem_free[AFTER];

                mem_free.erase(mem_free.begin() + AFTER);
            }
            else if(mem_free[BEFORE]->next == segment->base) // Touching front.
            {
                mem_free[BEFORE]->next = segment->next;
                mem_free[BEFORE]->size += segment->size;

                delete segment;
            }
            else if(mem_free[AFTER]->base == segment->next) // Touching back.
            {
                mem_free[AFTER]->base = segment->base;
                mem_free[AFTER]->size += segment->size;

                delete segment;
            }
            else     // Forever alone.
            {
                mem_free.insert(mem_free.begin() + pos, segment);
            }
        }
        return true;
    }
}
