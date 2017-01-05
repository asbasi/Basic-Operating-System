#include "ThreadControlBlock.h"
#include <vector>

#ifndef ARV_MNJ_MUTUX_H
#define ARV_MNJ_MUTEX_H

extern "C"
{

#define NUM_PRIORITIES 4
#define NULL_MUTEX     0

class Mutex
{
    public:
        TVMMutexID mid;    // The Mutex's ID.
        bool isLocked;     // Is it locked?
        TVMThreadID owner; // Current owner.

        std::vector<ThreadControlBlock*> waiting[NUM_PRIORITIES];

        Mutex();
        ~Mutex();

        // Gets the next owner if someone is waiting for it.
        ThreadControlBlock* getNextOwner();

        // Adds the thread specified to the appropriate waiting queue.
        void wantsMutex(ThreadControlBlock* thread);

        // Remove a mutex from the waiting queue.
        void stopWaiting(TVMThreadID tid);
};

}
#endif
