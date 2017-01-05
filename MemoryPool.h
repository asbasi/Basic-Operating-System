#include "VirtualMachine.h"
#include <stdint.h>
#include <vector>

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

extern "C"
{

#define MIN_SEGMENT_SIZE 64
#define NEXT_BLOCK     (char*)segment->base + segment->size

#define BEFORE pos - 1
#define AFTER  pos

typedef struct
{
    void* base;
    TVMMemorySize size;
    void* next;
} MemorySegment;

class MemoryPool
{
    private:
        TVMMemoryPoolID mem_id;
        TVMMemorySize mem_size;
        void* mem_base;

        std::vector<MemorySegment*> mem_free;
        std::vector<MemorySegment*> mem_alloc;

    public:
        MemoryPool(void* base, TVMMemorySize size, TVMMemoryPoolIDRef mid);
        ~MemoryPool();

        TVMMemoryPoolID getMemID();
        TVMMemorySize getMemSize();
        void* getMemBase();

        TVMMemorySize query_remaining();

        bool allocate_memory(void** pointer, TVMMemorySize msize);
        bool deallocate_memory(void* base);
};

}

#endif
