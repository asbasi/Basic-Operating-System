#include "Machine.h"
#include "VirtualMachine.h"
#include <vector>

#ifndef THREAD_CONTROL_BLOCK_H
#define THREAD_CONTROL_BLOCK_H

extern "C"
{

#define NOTHING 4

#define WAITING_MEMORY 3
#define WAITING_SLEEP  2
#define WAITING_MUTEX  1
#define WAITING_IO     0

// Struct used to pass all parameters into the skeleton function.
typedef struct
{
    TVMThreadEntry entry;
    void* param;
    TVMThreadID tid;
} skeletonParams;

class ThreadControlBlock
{
    private:
        // Wrapper function used so thread is "gracefully" terminated.
        static void skeletonEntry(void* sParams)
        {
            TVMThreadEntry entry = ((skeletonParams*)sParams)->entry;
            void* param          = ((skeletonParams*)sParams)->param;
            TVMThreadID tid      = ((skeletonParams*)sParams)->tid;

            MachineEnableSignals();

            entry(param);

            VMThreadTerminate(tid);
        };

        TVMThreadID        tid;
        SMachineContextRef mContext;

        skeletonParams     sParams;
        TVMThreadEntry     entry;
        void*              parameters;


        TVMThreadPriority  priority;
        TVMMemorySize      memsize;
        void*              stackaddr;

        volatile int            waitingFor;
        volatile TVMThreadState state;
        volatile TVMTick        ticksLeft;
        volatile int            result;
        volatile TVMMutexID     mWants;  // Mutex that thread wants.
        volatile bool           infiniteFlag;

    public:
        std::vector<TVMMutexID> mHeld;   // Mutex that thread holds.

    public:
        // Default state is VM_THREAD_STATE_DEAD.
        ThreadControlBlock(TVMThreadEntry entry, void* parameters,
                           TVMThreadPriority priority, void* stackaddr,
                           TVMMemorySize memsize, TVMThreadIDRef tidref);

        ~ThreadControlBlock();

        // Called in VMThreadActivate to create the context of the thread.
        void ThreadCreateContext();

        TVMThreadID getTID();

        SMachineContextRef getContext();

        TVMThreadPriority getPriority();

        void setState(TVMThreadState newState);
        TVMThreadState getState();

        void setWaitingFor(int reason);
        int  reasonForWaiting();

        bool doneWaiting();
        void setTicks(TVMTick ticks);
        void decrementTicks();

        void setResult(int result);
        int getResult();

        void* getStackAddr();

        TVMMutexID getMutexWants();           // Returns the mutex the thread is waiting for.
        void setMutexWants(TVMMutexID mtxid); // Sets the mutex the thread wants.

        bool hasMutex();

        void setInfiniteFlag(bool flag);
};

}

#endif
