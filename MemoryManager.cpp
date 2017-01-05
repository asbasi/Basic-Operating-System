#include "MemoryManager.h"

extern "C"
{
    MemoryManager::MemoryManager()
    {
    }

    MemoryManager::~MemoryManager()
    {
        for(auto it = pools.begin(); it != pools.end(); ++it)
        {
            delete (*it);
        }
        pools.clear();

        waitingQueue[0].clear();
        waitingQueue[1].clear();
        waitingQueue[2].clear();
        waitingQueue[3].clear();
    }

    void MemoryManager::add_pool(void* base, TVMMemorySize size, TVMMemoryPoolIDRef mid)
    {
        pools.push_back(new MemoryPool(base, size, mid));
    }

    MemoryPool* MemoryManager::find_pool(TVMMemoryPoolID mid)
    {
        MemoryPool* mpool = NULL;

        for(auto it = pools.begin(); it != pools.end(); ++it)
        {
            if((*it)->getMemID() == mid)
            {
                mpool = (*it);
                break;
            }
        }
        return mpool;
    }

    void MemoryManager::delete_pool(TVMMemoryPoolID mid)
    {
        for(auto it = pools.begin(); it != pools.end(); ++it)
        {
            if((*it)->getMemID() == mid)
            {
                delete (*it);
                pools.erase(it);
                return;
            }
        }

    }

    void MemoryManager::addToMemoryQueue(ThreadControlBlock* thread)
    {
        waitingQueue[thread->getPriority()].push_back(thread);
    }

    void MemoryManager::removeFromMemoryQueue(TVMThreadID tid)
    {
        for(unsigned int i = 0; i < NUM_MEMORY_QUEUES; i++)
        {
            for(unsigned int j = 0; j < waitingQueue[i].size(); j++)
            {
                if(tid == waitingQueue[i][j]->getTID())
                {
                    waitingQueue[i].erase(waitingQueue[i].begin() + j);
                }
            }
        }
    }

    ThreadControlBlock* MemoryManager::getNext()
    {
        ThreadControlBlock* nextThread = NULL;

        for(int i = VM_THREAD_PRIORITY_HIGH; i >= 0; i--)
        {
            if(!(waitingQueue[i].empty()))
            {
                nextThread = waitingQueue[i].front();
                waitingQueue[i].erase(waitingQueue[i].begin());
                break;
            }
        }

        return nextThread;
    }
}
