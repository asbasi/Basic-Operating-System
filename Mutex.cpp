#include "Mutex.h"
#include <iostream>
using namespace std;

extern "C"
{
    static TVMMutexID nextMID = 1;

    Mutex::Mutex()
    {
        this->mid = nextMID;
        nextMID++;

        this->isLocked = false;

        this->owner = NULL_MUTEX;
    }

    Mutex::~Mutex()
    {
    }

    // Gets the next owner if someone is waiting for it.
    // Returns a pointer to that owner.
    ThreadControlBlock* Mutex::getNextOwner()
    {
        ThreadControlBlock* newOwner = NULL;
        if(!(this->isLocked))
        {
            for(int i = VM_THREAD_PRIORITY_HIGH; i >= 0; i--)
            {
                if(!(waiting[i].empty()))
                {
                    newOwner = waiting[i].front();
                    waiting[i].erase(waiting[i].begin());
                    newOwner->mHeld.push_back(this->mid);
                    newOwner->setMutexWants(NULL_MUTEX);
                    this->owner = newOwner->getTID();
                    this->isLocked = true;
                    break;
                }
            }
        }
        return newOwner;
    }

    // Adds the thread specified to the appropriate waiting queue.
    void Mutex::wantsMutex(ThreadControlBlock* thread)
    {
        thread->setMutexWants(this->mid);
        waiting[thread->getPriority()].push_back(thread);
    }

    // Remove a mutex from the waiting queue.
    void Mutex::stopWaiting(TVMThreadID tid)
    {
        for(unsigned int i = 0; i < NUM_PRIORITIES; i++)
        {
            for(unsigned int j = 0; j < waiting[i].size(); j++)
            {
                if(waiting[i][j]->getTID() == tid)
                {
                    waiting[i][j]->setMutexWants(NULL_MUTEX);
                    waiting[i].erase(waiting[i].begin() + j);
                    return;
                }
            }
        }
    }
}
