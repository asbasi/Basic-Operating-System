#include "Scheduler.h"
#include <vector>
#include <iostream>
using namespace std;

extern "C"
{
    Scheduler::Scheduler()
    {
    }

    Scheduler::~Scheduler()
    {
        for(auto it = all_threads.begin(); it != all_threads.end(); ++it)
        {
            delete (*it);
        }
        all_threads.clear();

        for(auto it = mutexes.begin(); it != mutexes.end(); ++it)
        {
            delete (*it);
        }
        mutexes.clear();
    }

    ThreadControlBlock* Scheduler::findThread(TVMThreadID tid)
    {
        ThreadControlBlock* foundThread = NULL;
        for(auto it = all_threads.begin(); it != all_threads.end(); ++it)
        {
            if((*it)->getTID() == tid)
            {
                foundThread = (*it);
                break;
            }
        }
        return foundThread;
    }

    void Scheduler::addThread(ThreadControlBlock* thread)
    {
        all_threads.push_back(thread);
    }

    void Scheduler::deleteThread(TVMThreadID tid)
    {
        for(auto it = all_threads.begin(); it != all_threads.end(); ++it)
        {
            if((*it)->getTID() == tid)
            {
                delete (*it);
                all_threads.erase(it);
                break;
            }
        }
    }

    void Scheduler::addToReady(ThreadControlBlock* thread)
    {
        thread->setState(VM_THREAD_STATE_READY);
        ready_queues[thread->getPriority()].push_back(thread);
    }

    void Scheduler::removeFromReady(TVMThreadID tid)
    {
        unsigned int length = 0;

        for(unsigned int i = 0; i < NUM_READY_QUEUES; i++)
        {
            length = ready_queues[i].size();
            for(unsigned int j = 0; j < length; j++)
            {
                if(ready_queues[i][j]->getTID() == tid)
                {
                    ready_queues[i].erase(ready_queues[i].begin() + j);
                    return;
                }
            }
        }
    }

    void Scheduler::addToWaiting(ThreadControlBlock* thread)
    {
        thread->setState(VM_THREAD_STATE_WAITING);
        waiting_queues[thread->reasonForWaiting()].push_back(thread);
    }

    void Scheduler::removeFromWaiting(TVMThreadID tid)
    {
        unsigned int length = 0;

        for(unsigned int i = 0; i < NUM_WAITING_QUEUES; i++)
        {
            length = waiting_queues[i].size();
            for(unsigned int j = 0; j < length; j++)
            {
                if(waiting_queues[i][j]->getTID() == tid)
                {
                    waiting_queues[i][j]->setWaitingFor(NOTHING);
                    waiting_queues[i][j]->setTicks(0);
                    waiting_queues[i][j]->setInfiniteFlag(false);
                    waiting_queues[i].erase(waiting_queues[i].begin() + j);
                    return;
                }
            }
        }
    }

    void Scheduler::processAllWaiting()
    {
        int counter = 0;
        int i = 0;

        if(!(waiting_queues[WAITING_SLEEP].empty()))
        {
            counter = waiting_queues[WAITING_SLEEP].size();
            while(counter != 0)
            {
                counter--;
                waiting_queues[WAITING_SLEEP][i]->decrementTicks();

                if(waiting_queues[WAITING_SLEEP][i]->doneWaiting())
                {
                    waiting_queues[WAITING_SLEEP][i]->setWaitingFor(NOTHING);
                    waiting_queues[WAITING_SLEEP][i]->setInfiniteFlag(false);
                    addToReady(waiting_queues[WAITING_SLEEP][i]);
                    waiting_queues[WAITING_SLEEP].erase(waiting_queues[WAITING_SLEEP].begin() + i);
                    continue;
                }
                i++;
            }
        }

        i = 0;
        if(!(waiting_queues[WAITING_MUTEX].empty()))
        {
            counter = waiting_queues[WAITING_MUTEX].size();
            while(counter != 0)
            {
                counter--;

                waiting_queues[WAITING_MUTEX][i]->decrementTicks();

                if(waiting_queues[WAITING_MUTEX][i]->hasMutex() ||
                   waiting_queues[WAITING_MUTEX][i]->doneWaiting())
                {
                    waiting_queues[WAITING_MUTEX][i]->setWaitingFor(NOTHING);
                    waiting_queues[WAITING_MUTEX][i]->setTicks(0);
                    waiting_queues[WAITING_MUTEX][i]->setInfiniteFlag(false);
                    addToReady(waiting_queues[WAITING_MUTEX][i]);
                    waiting_queues[WAITING_MUTEX][i]->setMutexWants(0);
                    waiting_queues[WAITING_MUTEX].erase(waiting_queues[WAITING_MUTEX].begin() + i);
                    continue;
                }
                i++;
            }
        }
    }

    void Scheduler::scheduleNext()
    {
        ThreadControlBlock* newThread = NULL;

        for(int i = VM_THREAD_PRIORITY_HIGH; i >= VM_THREAD_PRIORITY_NONE; i--)
        {
            if(!(ready_queues[i].empty()))
            {
                newThread = ready_queues[i].front();
                ready_queues[i].erase(ready_queues[i].begin());
                break;
            }
        }
        newThread->setState(VM_THREAD_STATE_RUNNING);

        if(newThread != this->current)
        {
           SMachineContextRef oldContext = (current)->getContext();
           this->setCurrentThread(newThread);
           MachineContextSwitch(oldContext, newThread->getContext());
        }
    }

    void Scheduler::setCurrentThread(ThreadControlBlock* thread)
    {
        this->current = thread;
    }

    ThreadControlBlock* Scheduler::getCurrentThread()
    {
        return this->current;
    }

    TVMMutexID Scheduler::createMutex()
    {
        Mutex* mutex = new Mutex();
        mutexes.push_back(mutex);
        return mutex->mid;
    }

    Mutex* Scheduler::findMutex(TVMMutexID mutexID)
    {
        Mutex* mutex = NULL;
        for(auto it = mutexes.begin(); it != mutexes.end(); ++it)
        {
            if((*it)->mid == mutexID)
            {
                mutex = (*it);
                break;
            }
        }
        return mutex;
    }

    void Scheduler::deleteMutex(TVMMutexID mutexID)
    {
        for(auto it = mutexes.begin(); it != mutexes.end(); ++it)
       	{
            if((*it)->mid == mutexID)
            {
                delete (*it);
                mutexes.erase(it);
                break;
            }
        }
    }
}
