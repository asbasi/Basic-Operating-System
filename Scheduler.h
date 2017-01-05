#include "ThreadControlBlock.h"
#include "Mutex.h"
#include <vector>

#ifndef MY_SCHEDULER_H
#define MY_SCHEDULER_H

extern "C"
{

#define VM_THREAD_PRIORITY_NONE 0
#define NUM_WAITING_QUEUES	4
#define NUM_READY_QUEUES        4

class Scheduler
{
    private:
        // Holds all threads.
        std::vector<ThreadControlBlock*> all_threads;

        // Holds all ready threads (indexed by priority).
        std::vector<ThreadControlBlock*> ready_queues[NUM_READY_QUEUES];

        // Holds all waiting threads (indexed by reason for waiting).
        std::vector<ThreadControlBlock*> waiting_queues[NUM_WAITING_QUEUES];

        // Holds all mutexes.
        std::vector<Mutex*> mutexes; // All mutexes that have been created.

        // Currently running thread.
        ThreadControlBlock* current;

    public:
       Scheduler();
       ~Scheduler();

       ThreadControlBlock* findThread(TVMThreadID tid); // Finds a thread.

       TVMMutexID createMutex();
       Mutex* findMutex(TVMMutexID mutexID);
       void deleteMutex(TVMMutexID mutexID);

       void addThread(ThreadControlBlock* thread); // Adds a new thread to scheduler.
       void deleteThread(TVMThreadID tid);         // Removes a thread from the scheduler.

       // addToReady will also set state to ready.
       void addToReady(ThreadControlBlock* thread); // Adds a thread to the ready queue.
       void removeFromReady(TVMThreadID tid);       // Removes a thread from the ready queue.

       // addToWaiting will also set state to waiting.
       // removeFromWaiting will set reason to nothing.
       void addToWaiting(ThreadControlBlock* thread); // Adds a thread to the waiting queue.
       void removeFromWaiting(TVMThreadID tid);       // Removes a thread from the waiting queue.

       void processAllWaiting(); // Process all the threads that are waiting.
       void scheduleNext();      // Scheduler next ready thread.

       void setCurrentThread(ThreadControlBlock* thread); // Set the current thread.
       ThreadControlBlock* getCurrentThread();            // Get the current thread.
};

}


#endif
