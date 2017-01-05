#include "ThreadControlBlock.h"

extern "C"
{
    static TVMThreadID nextTID = 1;

    ThreadControlBlock::ThreadControlBlock(TVMThreadEntry entry, void* parameters,
                                           TVMThreadPriority priority, void* stackaddr,
                                           TVMMemorySize memsize, TVMThreadIDRef tidref)
    {
        this->tid       = nextTID;
        nextTID++;

        // Let the caller know the TID of the thread that was just created.
        if(tidref != NULL)
        {
            *tidref = this->tid;
        }

        this->mContext  = new SMachineContext;
        this->entry     = entry;
        this->parameters = parameters;

        this->sParams.entry = this->entry;
        this->sParams.param = this->parameters;
        this->sParams.tid   = this->tid;

        this->priority  = priority;

        this->memsize   = memsize;

        this->stackaddr = stackaddr;

        this->waitingFor = NOTHING;
        this->state = VM_THREAD_STATE_DEAD;
        this->ticksLeft = 0;
        this->mWants    = 0;

        this->infiniteFlag = false;
    }

    ThreadControlBlock::~ThreadControlBlock()
   {
        delete mContext;
    }

    void ThreadControlBlock::ThreadCreateContext()
    {
        MachineContextCreate(this->mContext, skeletonEntry, &(this->sParams), this->stackaddr,
                             this->memsize);
    }

    TVMThreadID ThreadControlBlock::getTID()
    {
        return this->tid;
    }


    SMachineContextRef ThreadControlBlock::getContext()
    {
        return this->mContext;
    }

    TVMThreadPriority ThreadControlBlock::getPriority()
    {
        return this->priority;
    }

    void ThreadControlBlock::setState(TVMThreadState newState)
    {
        this->state = newState;
    }

    TVMThreadState ThreadControlBlock::getState()
    {
        return this->state;
    }

    void ThreadControlBlock::setWaitingFor(int reason)
    {
        this->waitingFor = reason;
    }

    int  ThreadControlBlock::reasonForWaiting()
    {
        return this->waitingFor;
    }

    bool ThreadControlBlock::doneWaiting()
    {
        if(this->ticksLeft == 0)
        {
            return true;
        }

        return false;
    }

    void ThreadControlBlock::setTicks(TVMTick ticks)
    {
        this->ticksLeft = ticks;
    }

    void ThreadControlBlock::decrementTicks()
    {
        if(!this->infiniteFlag)
            --(this->ticksLeft);
    }

    void ThreadControlBlock::setResult(int result)
    {
        this->result = result;
    }

    int ThreadControlBlock::getResult()
    {
        return this->result;
    }

    TVMMutexID ThreadControlBlock::getMutexWants()
    {
        return this->mWants;
    }

    void ThreadControlBlock::setMutexWants(TVMMutexID mtxid)
    {
        this->mWants = mtxid;
    }

    bool ThreadControlBlock::hasMutex()
    {
        for(unsigned int i = 0; i < mHeld.size(); i++)
        {
            if(mWants == mHeld[i])
            {
                return true;
            }
        }
        return false;
    }

    void ThreadControlBlock::setInfiniteFlag(bool flag)
    {
        this->infiniteFlag = flag;
    }

    void* ThreadControlBlock::getStackAddr()
    {
        return this->stackaddr;
    }
}
