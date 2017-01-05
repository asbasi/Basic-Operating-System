#include "MemoryPool.h"
#include "ThreadControlBlock.h"
#include <cstddef>
#include <vector>

#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

extern "C"
{

#define NUM_MEMORY_QUEUES 4

class MemoryManager
{
    private:
        std::vector<MemoryPool*> pools;
        std::vector<ThreadControlBlock*> waitingQueue[NUM_MEMORY_QUEUES];

    public:
       MemoryManager();
       ~MemoryManager();

       void add_pool(void* base, TVMMemorySize size, TVMMemoryPoolIDRef mid);
       MemoryPool* find_pool(TVMMemoryPoolID mid);
       void delete_pool(TVMMemoryPoolID mid);

       void addToMemoryQueue(ThreadControlBlock* thread);
       void removeFromMemoryQueue(TVMThreadID tid);
       ThreadControlBlock* getNext();
};

}

#endif
