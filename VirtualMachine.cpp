#include "Machine.h"
#include "MemoryManager.h"
#include "Scheduler.h"
#include "Machine.h"
#include <sys/types.h>
#include <fcntl.h>
#include <math.h>
#include "string.h"
#include <iostream>
#include <iomanip>
using namespace std;

extern "C"
{

    #define MAX_WRITE_SIZE 512
    #define MAX_READ_SIZE  512

    #define WORD_SIZE_16    16
    #define BYTES_PER_ENTRY 32
    #define SECTOR_SIZE     512

    #define DIR_ATTR           11
    #define DIR_NTRES          12
    #define DIR_CRT_TIME_TENTH 13
    #define DIR_CRT_TIME       14
    #define DIR_CRT_DATE       16
    #define DIR_LAST_ACC_DATE  18
    #define DIR_FIRST_CLUS_HI  20
    #define DIR_WRITE_TIME     22
    #define DIR_WRITE_DATE     24
    #define DIR_FIRST_CLUS_LO  26
    #define DIR_FILE_SIZE      28

    #define ATTR_READ_ONLY  0x01
    #define ATTR_HIDDEN     0x02
    #define ATTR_SYSTEM     0x04
    #define ATTR_VOLUME_ID  0x08
    #define ATTR_DIRECTORY  0x10
    #define ATTR_ARCHIVE    0x20
    #define ATTR_LONG_NAME  (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

    #define grabMutex()     VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE)
    #define releaseMutex()  VMMutexRelease(FILE_SYSTEM_MUTEX)


    TVMMainEntry VMLoadModule(const char* module);
    void VMUnloadModule();
    void fileHandler(void* calldata, int result);

    Scheduler* myScheduler;
    MemoryManager* myMemoryManager;

    volatile TVMTick tickCount = 0;
    volatile int tickTime;

    volatile int nextFileDescriptor = 3;
    volatile int nextDirDescriptor  = 3;

    void idle(void* param)
    {
        while(1);
    }

    void waitForIO()
    {
        ThreadControlBlock* currentThread = myScheduler->getCurrentThread();
        currentThread->setWaitingFor(WAITING_IO);
        myScheduler->addToWaiting(currentThread);
        myScheduler->scheduleNext();
    }

    const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 1;


    TVMMemoryPoolID heapID;
    TVMMemoryPoolID stackID;

    TVMMutexID FILE_SYSTEM_MUTEX;

/*******************************************************************************************************
                                        File Functions/Classes
*******************************************************************************************************/
	
    typedef struct
    {
        int dirdescriptor;  // Descriptor associated with the file.
        bool isRoot;        // Is it the root direcotry?
        uint8_t* currEntry; // The entry are we currently on (the next one that should be read in).

        uint16_t startingCluster; // Starting cluster of the directory.
        uint16_t currentCluster;  // Current cluster that we're reading fro
        int currentSector;
    } Directory;

    typedef struct
    {
        int filedescriptor;    // Descriptor associated with the file.
        unsigned int filePtr;  // The file pointer which keeps track of where in the file we are.
        int flags;             // The flags the file was opened with.
        int mode;              // The mode the file was opened with.
        uint8_t* entry;        // The entry for the file.
    } File;

    typedef struct
    {
        uint8_t* data;
        uint16_t clusterNum;
    } Cluster;

    class FileSystem
    {
        public:
            // Holds the current working directory.
            char cwd[VM_FILE_SYSTEM_MAX_PATH + 1];

            // General File System Attributes.
            char* mount;
            int   fileDescriptor;
            uint8_t* base;

            // BPB Values.
            uint16_t BytesPerSector;
            uint8_t  SectorsPerCluster;
            uint16_t ReservedSectorCount;
            uint8_t  NumFATs;
            uint16_t RootEntryCount;
            uint16_t TotalSector16;
            uint16_t FATSize16;
            uint32_t HiddenSectors;
            uint32_t TotalSector32;

            uint16_t FirstRootSector;
            uint16_t FirstDataSector;

            // Fat Table.
            uint16_t* FatTable;

            // All Entries in root.
            uint8_t* RootEntries;

        public:
            FileSystem(char* mount, int fileDescriptor, void* base)
            {
                this->cwd[0] = VM_FILE_SYSTEM_DIRECTORY_DELIMETER; // '/'
                this->cwd[1] = '\0';

                this->mount         = mount;
                this->fileDescriptor = fileDescriptor;
                this->base          = (uint8_t*)base;

                processBPB();
                processFAT();
                processRoot();

                FirstRootSector = ReservedSectorCount + NumFATs * FATSize16;
                FirstDataSector = FirstRootSector + (RootEntryCount * 32 / 512);
            }

            ~FileSystem()
            {
                uint16_t* fat = this->FatTable;

                // Write back FAT table and duplicate.
                grabMutex();

                for(int i = 0; i < this->FATSize16; i++)
                {
                    memcpy(base, (void*)fat, MAX_WRITE_SIZE);
                    fat += this->BytesPerSector / 2;

                    writeSector(this->ReservedSectorCount + i, base, MAX_WRITE_SIZE);
                    writeSector(this->ReservedSectorCount + 17 + i, base, MAX_WRITE_SIZE);
                }

                releaseMutex();

                // Delete the FAT table.
                delete[] FatTable;

                // Write back all the entires.
                grabMutex();

                uint8_t* root = RootEntries;

                int RootDirectorySectors = (this->RootEntryCount * BYTES_PER_ENTRY) / this->BytesPerSector;
                for(int i = 0; i < RootDirectorySectors; i++)
                {
                    memcpy(base, (void*)root, MAX_WRITE_SIZE);
                    root += 512;

                    writeSector(this->ReservedSectorCount + (this->NumFATs * this->FATSize16) + i, base, MAX_WRITE_SIZE);
                }

                releaseMutex();

                // Delete the root entries.
                delete[] RootEntries;
            }

            char* getCWD()
            {
                return this->cwd;
            }

            uint8_t* getRoot()
            {
                return this->RootEntries;
            }

            void processBPB()
            {
                grabMutex();

                ThreadControlBlock* currentThread = myScheduler->getCurrentThread();
                MachineFileRead(this->fileDescriptor, this->base, MAX_READ_SIZE, fileHandler, (void*)currentThread);
                waitForIO();

                // Read in BPB.
                this->BytesPerSector = *((uint16_t*)(this->base + 11));
                this->SectorsPerCluster = this->base[13];
                this->ReservedSectorCount = *((uint16_t*)(this->base + 14));
                this->NumFATs = this->base[16];
                this->RootEntryCount = *((uint16_t*)(this->base + 17));
                this->TotalSector16 = *((uint16_t*)(this->base + 19));
                this->FATSize16 = *((uint16_t*)(this->base + 22));
                this->HiddenSectors = *((uint32_t*)(this->base + 28));
                this->TotalSector32 = *((uint32_t*)(this->base + 32));

                /*cout << "Bytes Per Sector: " <<  this->BytesPerSector << endl;
                cout << "Sectors Per Cluster: " << (uint16_t)this->SectorsPerCluster << endl;
                cout << "Reserved Sector Count: " << this->ReservedSectorCount << endl;
                cout << "Num FATs: " << (uint16_t)this->NumFATs << endl;
                cout << "Root Entry Count: " << this->RootEntryCount << endl;
                cout << "Total Sector 16: " << this->TotalSector16 << endl;
                cout << "FAT Size 16: " << this->FATSize16 << endl;
                cout << "Hidden Sectors: " << this->HiddenSectors << endl;
                cout << "Total Sector 32: " << this->TotalSector32 << endl;*/

                releaseMutex();
            }

            void processFAT()
            {
                FatTable = new uint16_t[this->FATSize16 * this->BytesPerSector / 2];

                grabMutex();

                for(int i = 0; i < this->FATSize16; i++)
                {
                    readSector(this->ReservedSectorCount + i, (uint8_t*)(FatTable + (i * this->BytesPerSector / 2)), MAX_READ_SIZE);
                }

                releaseMutex();

                /*for(int i = 0; i < this->FATSize16 * this->BytesPerSector / WORD_SIZE_16; i++)
                {
                    cout << std::setw(8) << std::setfill('0') << std::uppercase << std::hex << i * WORD_SIZE_16  << ": ";

                    for(int j = 0; j < WORD_SIZE_16 / 2; j++)
                    {
                        cout << std::setw(4) << std::setfill('0') << std::uppercase << std::hex << *(FatTable + (i * WORD_SIZE_16 / 2) + j) << " ";
                    }
                    cout << endl;
                }*/
            }

            void processRoot()
            {
                RootEntries = new uint8_t[this->RootEntryCount * BYTES_PER_ENTRY];

                grabMutex();

                int RootDirectorySectors = (this->RootEntryCount * BYTES_PER_ENTRY) / this->BytesPerSector;
                for(int i = 0; i < RootDirectorySectors; i++)
                {
                    readSector(this->ReservedSectorCount + (this->NumFATs * this->FATSize16) + i, RootEntries + (i * this->BytesPerSector), MAX_READ_SIZE);
                }

                releaseMutex();

                /*for(int i = 0; i < this->BytesPerSector * BYTES_PER_ENTRY; i += 32)
                {
                    if(RootEntries[i] == 0x00)
                    {
                        return;
                    }
                    else if(RootEntries[i] == 0xE5)
                    {
                        continue;
                    }
                    else
                    {
                        if(RootEntries[i + 11] != ATTR_LONG_NAME)
                        {
                            write(1, RootEntries + i, 11);

                            cout << " " << (uint16_t)RootEntries[i + DIR_ATTR] << endl;
                        }
                    }
                }*/
            }

            void readSector(int sector, uint8_t* base, int size)
            {
                ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

                MachineFileSeek(this->fileDescriptor, sector * this->BytesPerSector, 0, fileHandler, (void*)currentThread);
                waitForIO();

                MachineFileRead(this->fileDescriptor, this->base, size, fileHandler, (void*)currentThread);
                waitForIO();

                memcpy((void*)base, this->base, size);
            }


            void writeSector(int sector, uint8_t* base, int size)
            {
                ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

                MachineFileSeek(this->fileDescriptor, sector * this->BytesPerSector, 0, fileHandler, (void*)currentThread);
                waitForIO();

                memcpy(this->base, (void*)base, size);

                MachineFileWrite(this->fileDescriptor, this->base, size, fileHandler, (void*)currentThread);
                waitForIO();
            }
    };

    FileSystem* myFileSystem;
    vector<Directory*> openDirectories;
    vector<File*> openFiles;
    vector<Cluster*> cachedClusters;

    void createLFN(uint8_t **entry, const char *filename)
    {
        int length = strlen(filename);
        int rec = (length + 12) / 13;
        int num_char;
        bool end_name;
        bool zeroed;
        for(uint8_t i = rec; i > 0; i--)
        {
            //set first byte to number
            if(i == rec)
            {
                **entry = 0x40 | rec;
            }
            else
            {
                **entry = i;
            }

            *(*entry + 11) = ATTR_LONG_NAME;
            *(*entry + 12) = 0x00;
            *(*entry + 13) = 0x00;
            *(*entry + 26) = 0x00;
            *(*entry + 27) = 0x00;

            //assign 0xFF to all LFN place;
            for(int j = 1; j <= 10; j++)
            {
                *(*entry + j) = 0xFF;
            }
            for(int j = 14; j <= 25; j++)
            {
                *(*entry + j) = 0xFF;
            }
            for(int j = 28; j <= 31; j++)
            {
                *(*entry + j) = 0xFF;
            }

            num_char = length - ((i - 1) * 13);
            end_name = false;
            zeroed = false;
            for(int j = 1; j <= 9; j += 2)
            {
                if(end_name == true)
                {
                    *(*entry + j) = 0x00;
                    *(*entry + j + 1) = 0x00;
                    zeroed = true;
                    break;
                }
                else
                {
                    *(*entry + j) = filename[(i - 1) * 13 + (j/2)];
                    *(*entry + j + 1) = 0x00;
                    num_char--;
                    length--;
                    if(num_char == 0)
                    {
                        end_name = true;
                    }
                }
            }

            for(int j = 14; j <= 24; j += 2)
            {
                if(end_name)
                {
                    if(!zeroed)
         	    {
                        *(*entry + j) = 0x00;
                        *(*entry + j + 1) = 0x00;
                        zeroed = true;
                    }
                    break;
                }
                else
                {
                    *(*entry + j) = filename[(i - 1) * 13 + (j/2) - 2];
                    *(*entry + j + 1) = 0x00;
                    num_char--;
                    length--;
                    if(num_char == 0)
                    {
                        end_name = true;
                    }
                }
            }

            for(int j = 28; j <= 30; j += 2)
            {
                if(end_name)
                {
                    if(!zeroed)
                    {
                        *(*entry + j) = 0x00;
                        *(*entry + j + 1) = 0x00;
                        zeroed = true;
                    }
                    break;
                }
                else
                {
                    *(*entry + j) = filename[(i - 1) * 13 + (j/2) - 3];
                    *(*entry + j + 1) = 0x00;
                    num_char--;
                    length--;
                    if(num_char == 0)
                    {
                        end_name = true;
                    }
                }
            }
            *entry += 32;
        }
    }

    void getLFN(uint8_t **entry, char* outputBuffer)
    {
        *outputBuffer = '\0';
        char next_char;
        char next_section[300];
        bool end_entry;
        int len;
        int i;

        //check if entry is a long name entry;
        while(*(*entry + DIR_ATTR) == ATTR_LONG_NAME)
        {
            //clear next_section
            *next_section = '\0';
            end_entry = false;
            i = 1;

            //first part of long name;
            while(!end_entry && i <= 9)
            {
                next_char = (char)(*(*entry + (i)));
                //end of entry
                if(next_char == 0x00)
                {
                    end_entry = true;
                }
                else
                {
                    len = strlen(next_section);
                    next_section[len] = next_char;
                    next_section[len+1] = '\0';
                }
                i += 2;
            }

            //second part of long name;
            i = 14;
            while(!end_entry && i <= 24)
            {
                next_char = (char)(*(*entry + (i)));
                //end of entry
                if(next_char == 0x00)
                {
                    end_entry = true;
                }
                else
                {
                    len = strlen(next_section);
                    next_section[len] = next_char;
                    next_section[len+1] = '\0';
                }
                i += 2;
            }

            i = 28;
            while(!end_entry && i <= 30)
            {
                next_char = (char)(*(*entry + (i)));
                //end of entry
                if(next_char == 0x00)
                {
                    end_entry = true;
                }
                else
                {
                    len = strlen(next_section);
                    next_section[len] = next_char;
                    next_section[len+1] = '\0';
                }
                i += 2;
            }

	    strcat(next_section, outputBuffer);
            strcpy(outputBuffer, next_section);
            *entry = *entry + 32;
        }
    }

    void updateSFN(char *outputBuffer)
    {
        uint8_t *entry = myFileSystem->RootEntries;
        while(*entry != 0x00 && entry < myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
        {
            if(*entry == 0xE5 || (entry[DIR_ATTR] & ATTR_LONG_NAME) == ATTR_LONG_NAME)
            {
                entry += BYTES_PER_ENTRY;
                continue;
            }

            if(strncmp(outputBuffer, (const char*)entry, 7) == 0 && strncmp(outputBuffer + 8, (const char*)(entry + 8), 3) == 0)
            {
                if(outputBuffer[7] <= entry[7])
                {
                    outputBuffer[7] = entry[7] + 1;
                }
            }
            entry += BYTES_PER_ENTRY;
        }
    }

    void Normal_to_SFN(char* outputBuffer, const char* filename, bool update)
    {
        for(int i = 0; i < VM_FILE_SYSTEM_SFN_SIZE; i++)
        {
            outputBuffer[i] = 0x20;
        }
        outputBuffer[VM_FILE_SYSTEM_SFN_SIZE] = '\0';

        int length = strlen(filename);
        int namesize = 0;

        // Finds how long the name is (not including extension).
        while(filename[namesize] != '.' && namesize < length)
        {
            namesize++;
        }

        for(int i = 0; i < namesize; i++)
        {
            if(i == 7 && namesize > 8)
            {
                outputBuffer[6] = '~';
                outputBuffer[7] = '1';
                break;
            }
            else
            {
                outputBuffer[i] = toupper(filename[i]);
            }
        }

        if(filename[namesize] == '.')
        {
            for(int i = 0; i < length - (namesize + 1); i++)
            {
                outputBuffer[8 + i] = toupper(filename[namesize + 1 + i]);
            }
        }

        if(update && outputBuffer[6] == '~' && outputBuffer[7] == '1')
        {
            updateSFN(outputBuffer);
        }
    }
	
    void SFN_to_Normal(char* output_name, const char *sfn)
    {
        int i = 0;
        int count = 0;
        while(i <= 7)
        {
            if(sfn[i] != 0x20)
            {
                output_name[count] = sfn[i];
                count++;
                i++;
            }
            else
            {
                i++;
                continue;
            }
        }

        if(sfn[i] != 0x20)
        {
            output_name[count] = '.';
            count++;
            while(i < 11)
            {
                if(sfn[i] != 0x20)
                {
                    output_name[count] = sfn[i];
                    count++;
                    i++;
                }
                else
                {
                    i++;
                    continue;
                }
            }
        }

        output_name[count] = '\0';
    }
	
    Cluster* findCachedCluster(uint16_t clusterNum)
    {
       Cluster* cluster = NULL;

       for(unsigned int i = 0; i < cachedClusters.size(); i++)
       {
           if(cachedClusters[i]->clusterNum == clusterNum)
           {
               cluster = cachedClusters[i];
               break;
           }
       }
       return cluster;

    }

    void createCachedCluster(Cluster* clus, uint16_t clusterNum)
    {
        clus->data = new uint8_t[myFileSystem->BytesPerSector * myFileSystem->SectorsPerCluster];
        clus->clusterNum = clusterNum;

        unsigned int seekPosition = myFileSystem->FirstDataSector * 512 + (clusterNum - 2) * myFileSystem->SectorsPerCluster * myFileSystem->BytesPerSector;

        MachineFileSeek(myFileSystem->fileDescriptor, seekPosition, 0, fileHandler, (void*)myScheduler->getCurrentThread());
        waitForIO();

        uint8_t* position = clus->data;

        for(unsigned int i = 0; i < myFileSystem->SectorsPerCluster; i++)
        {
            MachineFileRead(myFileSystem->fileDescriptor, myFileSystem->base, 512, fileHandler, (void*)myScheduler->getCurrentThread());
            waitForIO();

            memcpy(position, myFileSystem->base, 512);
            position += 512;
        }
    }

    void deleteCachedCluster(Cluster* clus)
    {
        unsigned int seekPosition = myFileSystem->FirstDataSector * 512 + (clus->clusterNum - 2) * myFileSystem->SectorsPerCluster * myFileSystem->BytesPerSector;

        MachineFileSeek(myFileSystem->fileDescriptor, seekPosition, 0, fileHandler, (void*)myScheduler->getCurrentThread());
        waitForIO();

        uint8_t* position = clus->data;

        // Write cluster back.
        for(unsigned int i = 0; i < myFileSystem->SectorsPerCluster; i++)
        {
            memcpy(myFileSystem->base, position, 512);
            position += 512;

            MachineFileWrite(myFileSystem->fileDescriptor, myFileSystem->base, 512, fileHandler, (void*)myScheduler->getCurrentThread());
            waitForIO();
       	}

        delete[] clus->data;
    }

    Directory* findOpenDir(int dirdesc)
    {
       Directory* dir = NULL;

       for(unsigned int i = 0; i < openDirectories.size(); i++)
       {
           if(openDirectories[i]->dirdescriptor == dirdesc)
           {
               dir = openDirectories[i];
               break;
           }
       }
       return dir;
    }

    void deleteOpenDir(int dirdesc)
    {
       for(unsigned int i = 0; i < openDirectories.size(); i++)
       {
           if(openDirectories[i]->dirdescriptor == dirdesc)
           {
               delete openDirectories[i];
               openDirectories.erase(openDirectories.begin() + i);
               return;
           }
       }
    }

    File* findOpenFile(int filedesc)
    {
        File* file = NULL;

        for(unsigned int i = 0; i < openFiles.size(); i++)
        {
            if(openFiles[i]->filedescriptor == filedesc)
            {
                file = openFiles[i];
                break;
            }
        }
        return file;
    }

    void deleteOpenFile(int filedesc)
    {
       for(unsigned int i = 0; i < openFiles.size(); i++)
       {
           if(openFiles[i]->filedescriptor == filedesc)
           {
               delete openFiles[i];
               openFiles.erase(openFiles.begin() + i);
               return;
           }
       }
    }

/*******************************************************************************************************
                                        General Functions
*******************************************************************************************************/

    void alarmHandler(void* calldata)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        tickCount++;
        myScheduler->processAllWaiting();
        myScheduler->addToReady(myScheduler->getCurrentThread());
        myScheduler->scheduleNext();

        MachineResumeSignals(&sigstate);
    }

    TVMStatus VMStart(int tickms, TVMMemorySize heapsize, TVMMemorySize sharedsize, const char* mount, int argc, char* argv[])
    {
        void* sharedmem;

        if((sharedmem = MachineInitialize(sharedsize)) == NULL)
        {
            return VM_STATUS_FAILURE;
        }
        TVMMainEntry VMMain = VMLoadModule(argv[0]);

        // Failed to load.
        if(VMMain == NULL)
        {
            MachineTerminate();
            return VM_STATUS_FAILURE;
        }

        myScheduler = new Scheduler();
        myMemoryManager = new MemoryManager();

        tickTime = tickms;

        uint8_t* systemHeap = new uint8_t[heapsize];

        myMemoryManager->add_pool((void*)systemHeap, heapsize, &heapID);
        myMemoryManager->add_pool(sharedmem, sharedsize, &stackID);

        // Create main thread & put it into scheduler.
        ThreadControlBlock* mainThread = new ThreadControlBlock(NULL, NULL, VM_THREAD_PRIORITY_NORMAL,
                                                                NULL, 0, NULL);
        myScheduler->addThread(mainThread);
        myScheduler->setCurrentThread(mainThread);
        mainThread->setState(VM_THREAD_STATE_RUNNING);

        // Create the idle thread & put it into scheduler.
        void* stackaddr;
        TVMStatus status = VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, 0x100000, &stackaddr);

        // If memory for the thread's stack space was unable to be allocated.
        if(status != VM_STATUS_SUCCESS)
        {
            return status;
        }

        ThreadControlBlock* idleThread = new ThreadControlBlock(idle, NULL, VM_THREAD_PRIORITY_NONE,
                                                                stackaddr, 0x100000, NULL);
        myScheduler->addThread(idleThread);
        VMThreadActivate(idleThread->getTID());
        myScheduler->addToReady(idleThread);


        // Create a mutex for the file system.
        VMMutexCreate(&FILE_SYSTEM_MUTEX);

        // Allocate a 512 Byte block for the FileSystem in shared memory.
        void* fileSystem_base;
        status = VMMemoryPoolAllocate(stackID, MAX_WRITE_SIZE, &fileSystem_base);

        if(status != VM_STATUS_SUCCESS)
        {
            return status;
        }

        // Try to open the FAT File.
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MachineFileOpen(mount, O_RDWR, 0600, fileHandler, (void*)myScheduler->getCurrentThread());
        waitForIO();

        int fileDescriptor;

        if((fileDescriptor = myScheduler->getCurrentThread()->getResult()) < 0) // Couldn't be mounted.
        {
            return VM_STATUS_FAILURE;
        }

        myFileSystem = new FileSystem((char*)mount, fileDescriptor, fileSystem_base);

        MachineResumeSignals(&sigstate);

        MachineEnableSignals();
        MachineRequestAlarm(tickTime * 1000, alarmHandler, NULL);

        VMMain(argc, argv);

        // Close all open files/directories.
        for(unsigned int i = 0; i < openDirectories.size(); i++)
        {
            delete openDirectories[i];
        }
        openDirectories.clear();

        for(unsigned int i = 0; i < openFiles.size(); i++)
        {
            delete openFiles[i];
        }
        openFiles.clear();

        for(unsigned int i = 0; i < cachedClusters.size(); i++)
        {
            deleteCachedCluster(cachedClusters[i]);
            delete cachedClusters[i];
        }

        // Delete the file system mutex.
        VMMutexDelete(FILE_SYSTEM_MUTEX);

        VMMemoryPoolDeallocate(stackID, fileSystem_base);

        delete myFileSystem;

        MachineFileClose(fileDescriptor, fileHandler, (void*)myScheduler->getCurrentThread());
        waitForIO();

        VMUnloadModule();
        MachineTerminate();

        delete myScheduler;
        delete myMemoryManager;
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMTickMS(int* tickmsref)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(tickmsref == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *tickmsref = tickTime;

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMTickCount(TVMTickRef tickref)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(tickref == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *tickref = tickCount;

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

/*******************************************************************************************************
                                        Directory Functions
*******************************************************************************************************/

    void parseDate(uint16_t date, SVMDateTimeRef DTStruct)
    {
     	DTStruct->DYear  = (unsigned int)(date >> 9) + 1980;
        DTStruct->DMonth = (unsigned char)((date & 511) >> 5);
        DTStruct->DDay   = (unsigned char)(date & 31);
    }

    void parseTime(uint16_t time, uint8_t timetenth, SVMDateTimeRef DTStruct)
    {
     	DTStruct->DMinute = (unsigned char)((time & 2047) >> 5);
        DTStruct->DHour = (unsigned char)(time >> 11);
        DTStruct->DSecond = (unsigned char)(time & 31) * 2;
        DTStruct->DHundredth = (unsigned char)timetenth;
    }

    TVMStatus VMDirectoryOpen(const char* dirname, int* dirdescriptor)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(dirname == NULL || dirdescriptor == NULL)
        {
       	    MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        // If dirname is not root return failure.
        if(strcmp(dirname, "/") != 0)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        // Create a Directory struct for it.
        Directory* dir = new Directory;

        // Assign it the next available dirdescriptor.
        dir->dirdescriptor = nextDirDescriptor;
        *dirdescriptor = dir->dirdescriptor;
        nextDirDescriptor++;

        dir->isRoot = true;
        dir->currEntry = myFileSystem->getRoot();

        while(*dir->currEntry == 0xE5 || *dir->currEntry == 0x00 || dir->currEntry[DIR_ATTR] == ATTR_LONG_NAME)
        {
            // No entries can be found.
            if(*dir->currEntry == 0x00 || dir->currEntry == myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
            {
                dir->currEntry = myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY;
                break;
            }
            else if(dir->currEntry[DIR_ATTR] == ATTR_LONG_NAME && (dir->currEntry[0] & 0x40)  == 0x40)
            {
                break;
            }
            dir->currEntry += BYTES_PER_ENTRY;
        }

        // Add that to the vector of currently open directories.
        openDirectories.push_back(dir);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMDirectoryClose(int dirdescriptor)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        // Locate the directory associated with the dirdescriptor.
        Directory* dir = findOpenDir(dirdescriptor);

        // If no such directory has been opened, return failure.
        if(dir == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        // Remove the directory from the vector of directories.
        deleteOpenDir(dirdescriptor);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(dirent == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        // Locate the directory associated with the dirdescriptor.
        Directory* dir = findOpenDir(dirdescriptor);

        if(dir == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        // If root and we haven't passed last entry.
        if(dir->isRoot && dir->currEntry < myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
        {
            // Read in next entry.
            getLFN(&dir->currEntry, dirent->DLongFileName);
            SFN_to_Normal(dirent->DShortFileName, (char*)dir->currEntry);

            dirent->DSize = *(unsigned int*)(dir->currEntry + DIR_FILE_SIZE);
            dirent->DAttributes = *(unsigned char*)(dir->currEntry + DIR_ATTR);

            parseDate(*(uint16_t*)(dir->currEntry + DIR_CRT_DATE), &dirent->DCreate);
            parseTime(*(uint16_t*)(dir->currEntry + DIR_CRT_TIME), 0, &dirent->DCreate);

            parseDate(*(uint16_t*)(dir->currEntry + DIR_LAST_ACC_DATE), &dirent->DAccess);
            parseTime(0, 0, &dirent->DCreate);

            parseDate(*(uint16_t*)(dir->currEntry + DIR_WRITE_DATE), &dirent->DModify);
            parseTime(*(uint16_t*)(dir->currEntry + DIR_WRITE_TIME), 0, &dirent->DModify);

            // Set the directories current entry to the next available one (if one exists)
            dir->currEntry += BYTES_PER_ENTRY;
            while(*dir->currEntry == 0xE5 || *dir->currEntry == 0x00 || dir->currEntry[DIR_ATTR] == ATTR_LONG_NAME)
            {
                // No entries can be found.
                if(*dir->currEntry == 0x00 || dir->currEntry == myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
                {
                    dir->currEntry = myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY;
                   break;
                }
                else if(dir->currEntry[DIR_ATTR] == ATTR_LONG_NAME && (dir->currEntry[0] & 0x40)  == 0x40)
                {
                    break;
                }

                dir->currEntry += BYTES_PER_ENTRY;
            }
        }
        else
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMDirectoryRewind(int dirdescriptor)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        // Locate the directory associated with the dirdescriptor.
        Directory* dir = findOpenDir(dirdescriptor);

        if(dir == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        // Reset the currentEntry to the first entry in the directory.
        if(dir->isRoot)
        {
       	    dir->currEntry = myFileSystem->getRoot();

            // While loop is used to find the first actual entry.
            while(*dir->currEntry == 0xE5 || *dir->currEntry == 0x00 || dir->currEntry[DIR_ATTR] == ATTR_LONG_NAME)
            {
                // No entries can be found.
                if(*dir->currEntry == 0x00 || dir->currEntry == myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
                {
                    dir->currEntry = myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY;
                    break;
                }
                else if(dir->currEntry[DIR_ATTR] == ATTR_LONG_NAME && (dir->currEntry[0] & 0x40)  == 0x40)
                {
                    break;
                }

                dir->currEntry += BYTES_PER_ENTRY;
            }
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMDirectoryCurrent(char* abspath)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(abspath == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        memcpy(abspath, myFileSystem->getCWD(), strlen(myFileSystem->getCWD()) + 1);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMDirectoryChange(const char* path)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(path == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        char newCWD[VM_FILE_SYSTEM_MAX_PATH + 1];

        TVMStatus status = VM_STATUS_SUCCESS;

        if(VMFileSystemIsAbsolutePath(path) == VM_STATUS_SUCCESS && path[1] != '\0') // Dealing with an absolute path.
        {
            status = VMFileSystemGetAbsolutePath(newCWD, myFileSystem->getCWD(), path + 1);
        }
        else
        {
            status = VMFileSystemGetAbsolutePath(newCWD, myFileSystem->getCWD(), path);
        }

        if(status  == VM_STATUS_SUCCESS && strcmp(newCWD, "/") == 0)
        {
            char* currDir = myFileSystem->getCWD();

            currDir[0] = VM_FILE_SYSTEM_DIRECTORY_DELIMETER; // '/'
            currDir[1] = '\0';
        }
        else
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    // Extra Credit
    TVMStatus VMDirectoryCreate(const char* dirname)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    // Extra Credit
    TVMStatus VMDirectoryUnlink(const char* path)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }


/*******************************************************************************************************
                                       	Memory Pool Functions
*******************************************************************************************************/

    TVMStatus VMMemoryPoolCreate(void* base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(base == NULL || memory == NULL || size == 0)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        myMemoryManager->add_pool(base, size, memory);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MemoryPool* memPool = myMemoryManager->find_pool(memory);

        if(memPool == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        // Check that it's fully deallocated.
        if(memPool->query_remaining() != memPool->getMemSize())
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

        myMemoryManager->delete_pool(memory);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MemoryPool* memPool = myMemoryManager->find_pool(memory);

        if(bytesleft == NULL || memPool == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *bytesleft = memPool->query_remaining();

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void** pointer)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MemoryPool* memPool = myMemoryManager->find_pool(memory);

        if(pointer == NULL || size == 0 || memPool == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        if(!memPool->allocate_memory(pointer, size))
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void* pointer)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        MemoryPool* memPool = myMemoryManager->find_pool(memory);

        if(memPool == NULL || pointer == NULL || !memPool->deallocate_memory(pointer))
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }


/*******************************************************************************************************
                                        Thread Functions
*******************************************************************************************************/

    TVMStatus VMThreadCreate(TVMThreadEntry entry, void* param, TVMMemorySize memsize,
                             TVMThreadPriority prio, TVMThreadIDRef tid)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(entry == NULL || tid == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }


        /********************************************/
        void* stackaddr;

        TVMStatus status = VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, memsize, &stackaddr);

        // If memory for the thread's stack space was unable to be allocated.
        if(status != VM_STATUS_SUCCESS)
        {
            MachineResumeSignals(&sigstate);
            return status;
        }

        /********************************************/

        // Constructor automatically assigns the thread's ID to tid.
        myScheduler->addThread(new ThreadControlBlock(entry, param, prio, stackaddr, memsize, tid));

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadDelete(TVMThreadID threadID)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        ThreadControlBlock* thread = myScheduler->findThread(threadID);

        if(thread == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        else if(thread->getState() != VM_THREAD_STATE_DEAD)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_STATE;
        }


       	/********************************************/

        TVMStatus status = VMMemoryPoolDeallocate(VM_MEMORY_POOL_ID_SYSTEM, thread->getStackAddr());

        // If memory for the thread's stack space was unable to be allocated.
        if(status != VM_STATUS_SUCCESS)
        {
            MachineResumeSignals(&sigstate);
            return status;
        }

        /********************************************/

        myScheduler->deleteThread(threadID);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadActivate(TVMThreadID threadID)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        ThreadControlBlock* thread = myScheduler->findThread(threadID);
        if(thread == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        else if(thread->getState() != VM_THREAD_STATE_DEAD)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

        thread->ThreadCreateContext(); // Create the threads context.

        myScheduler->addToReady(thread); // Will put thread into ready state/queue.

        if(thread->getPriority() > (myScheduler->getCurrentThread())->getPriority())
        {
            myScheduler->addToReady(myScheduler->getCurrentThread()); // Current goes to ready.
            myScheduler->scheduleNext(); // Schedule the next thread.
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadTerminate(TVMThreadID threadID)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        ThreadControlBlock* thread = myScheduler->findThread(threadID);

        if(thread == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        else if(thread->getState() == VM_THREAD_STATE_DEAD)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

        TVMThreadState state = thread->getState();
        thread->setState(VM_THREAD_STATE_DEAD);

        TVMThreadPriority prio = myScheduler->getCurrentThread()->getPriority();
        ThreadControlBlock* newOwner;
        bool needScheduler = false;

        // Release all mutexes.
        for(unsigned int i = 0; i < (thread->mHeld).size(); i++)
        {
            Mutex* mtx = myScheduler->findMutex(thread->mHeld[i]);
            mtx->owner = 0;
            mtx->isLocked = false;
            newOwner = mtx->getNextOwner();
            if(newOwner != NULL)
            {
                myScheduler->removeFromWaiting(newOwner->getTID());
                myScheduler->addToReady(newOwner);

                if(newOwner->getPriority() > prio)
                {
                    needScheduler = true;
                }
            }
        }
        thread->mHeld.clear();

        if(state == VM_THREAD_STATE_READY)
        {
            myScheduler->removeFromReady(threadID);
        }
        else if(state == VM_THREAD_STATE_WAITING)
        {
            // If we were waiting for a mutex.
            if(thread->reasonForWaiting() == WAITING_MUTEX)
            {
                Mutex* mtx = myScheduler->findMutex(thread->getMutexWants());

                if(mtx != NULL)
                {
                    mtx->stopWaiting(thread->getTID());
                }
            }
            else if(thread->reasonForWaiting() == WAITING_MEMORY)
            {
                myMemoryManager->removeFromMemoryQueue(thread->getTID());
            }
            myScheduler->removeFromWaiting(threadID);
        }

        if(state == VM_THREAD_STATE_RUNNING || needScheduler)
        {
            // Not terminating curren't running thread so we need to
            // put it back into ready queue.
            if(state != VM_THREAD_STATE_RUNNING)
            {
                myScheduler->addToReady(myScheduler->getCurrentThread());
            }
            myScheduler->scheduleNext();
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadID(TVMThreadIDRef threadref)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(threadref == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *threadref = (myScheduler->getCurrentThread())->getTID();

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadState(TVMThreadID threadID, TVMThreadStateRef state)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);
        ThreadControlBlock* thread = myScheduler->findThread(threadID);

        if(thread == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        else if(state == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *state = thread->getState();

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMThreadSleep(TVMTick tick)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        ThreadControlBlock* curr = myScheduler->getCurrentThread();

        if(tick == VM_TIMEOUT_INFINITE)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        if(tick == VM_TIMEOUT_IMMEDIATE)
        {
            myScheduler->addToReady(curr);
        }
        else
        {
            curr->setWaitingFor(WAITING_SLEEP);
            curr->setTicks(tick);
            myScheduler->addToWaiting(curr);
        }

        myScheduler->scheduleNext();

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }


/*******************************************************************************************************
                                       	File Functions
*******************************************************************************************************/

    void fileHandler(void* calldata, int result)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        ThreadControlBlock* thread = (ThreadControlBlock*)calldata;

        if(thread->getState() != VM_THREAD_STATE_DEAD) // Make sure thread wasn't terminated.
        {
            thread->setResult(result);
            myScheduler->removeFromWaiting(thread->getTID());
            myScheduler->addToReady(thread);
            ThreadControlBlock* curr = myScheduler->getCurrentThread();

            // Just awoke a higher priority thread.
            if(thread->getPriority() > curr->getPriority())
            {
                myScheduler->addToReady(curr);
                myScheduler->scheduleNext();
            }
        }
        MachineResumeSignals(&sigstate);
    }

    uint16_t FatSearch(uint16_t firstCluster, int clustersToHop)
    {
        uint16_t* table = myFileSystem->FatTable;

        uint16_t currCluster = firstCluster;

        if(clustersToHop == 0)
        {
            return firstCluster;
        }
        else
        {
            for(int i = 0; i < clustersToHop; i++)
            {
                if(currCluster == 0x0000 || currCluster >= 0xFFF7)
                {
                    return -1;
                }
                currCluster = table[currCluster];
            }
        }
        return currCluster;
    }

    uint16_t getCurrDate()
    {
        SVMDateTime DTStruct;
        uint16_t date = 0;

        VMDateTime(&DTStruct);

        date |= (DTStruct.DYear - 1980) << 9;
        date |= DTStruct.DMonth << 5;
        date |= DTStruct.DDay;

        return date;
    }

    uint16_t getCurrTime()
    {
        SVMDateTime DTStruct;
        uint16_t time = 0;

        VMDateTime(&DTStruct);

        time |= DTStruct.DHour << 11;
        time |= DTStruct.DMinute << 5;
        time |= (DTStruct.DSecond * 2);

        return time;
    }

    uint8_t* createRootEntry(uint8_t* entryPtr, const char* filename)
    {
        createLFN(&entryPtr, filename); // Generate it's long file name.

        uint8_t sfn[VM_FILE_SYSTEM_SFN_SIZE + 1];
        Normal_to_SFN((char*)sfn, filename, true);

        memcpy(entryPtr, (void*)sfn, 11);
        uint16_t date = getCurrDate();
        uint16_t time = getCurrTime();

        *(uint8_t*)(entryPtr + DIR_ATTR) = 0;

        *(uint8_t*)(entryPtr + DIR_CRT_TIME_TENTH) = 0;

        *(uint16_t*)(entryPtr + DIR_CRT_DATE) = date;
        *(uint16_t*)(entryPtr + DIR_CRT_TIME) = time;

        *(uint16_t*)(entryPtr + DIR_LAST_ACC_DATE) = date;

        *(uint16_t*)(entryPtr + DIR_WRITE_DATE) = date;
        *(uint16_t*)(entryPtr + DIR_WRITE_TIME) = time;

        *(uint16_t*)(entryPtr + DIR_FIRST_CLUS_HI) = 0;

        *(uint32_t*)(entryPtr + DIR_FILE_SIZE) = 0;


        // Find first free cluster in FAT Table.
        for(int i = 0; i < myFileSystem->FATSize16 * myFileSystem->BytesPerSector / 2; i ++)
        {
            if(myFileSystem->FatTable[i] == 0x0000) // Free.
            {
                *(uint32_t*)(entryPtr + DIR_FIRST_CLUS_LO) = i;
                myFileSystem->FatTable[i] = 0xFFFF;
                break;
            }
        }
        return entryPtr;
    }


    TVMStatus VMFileOpen(const char* filename, int flags, int mode, int* filedescriptor)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(filename == NULL || filedescriptor == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        // Attempt to locate the file inside the root.
        char lfname[VM_FILE_SYSTEM_LFN_SIZE];

        char sfname[VM_FILE_SYSTEM_SFN_SIZE + 1];
        Normal_to_SFN(sfname, filename, false);

        unsigned int namesize = 0;

        // Finds how long the name is (not including extension).
        while(filename[namesize] != '.' && namesize < strlen(filename))
        {
            namesize++;
        }

        bool found = false;
        uint8_t* entry;

        for(uint8_t* rootPtr = myFileSystem->getRoot(); rootPtr < myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY; rootPtr += BYTES_PER_ENTRY)
        {
            if(rootPtr[0] == 0x00) // No more entries.
            {
                break;
            }


            if(rootPtr[DIR_ATTR] == ATTR_LONG_NAME && (rootPtr[0] & 0x40) == 0x40)
            {
                getLFN(&rootPtr, lfname);

                if(strncmp(filename, lfname, strlen(filename)) == 0)
                {
                    found = true;
                    entry = rootPtr;
                    break;
                }
            }

            if(namesize <= 8 && rootPtr[0] != 0xE5 && rootPtr[DIR_ATTR] != ATTR_LONG_NAME)
            {
                if(strncmp(sfname, (char*)rootPtr, 11) == 0)
                {
                    found = true;
                    entry = rootPtr;
                    break;
                }
            }
        }
        // If not found.
        if(!found)
        {
            if((flags & O_CREAT) != 0) // If O_CREAT flag is set.
            {
                // Create a new entry for it (if there's space left in root).
                entry = myFileSystem->getRoot();

                while(entry[0] != 0x00 && entry < myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
                {
                    entry += BYTES_PER_ENTRY;
                }

                // Check if we stopped because we found a free entry or because we ran out of space.
                if(entry == myFileSystem->getRoot() + myFileSystem->RootEntryCount * BYTES_PER_ENTRY)
                {
                    MachineResumeSignals(&sigstate);
                    return VM_STATUS_FAILURE;
                }
                else // Found an space so we create entry for this new file.
                {
                    entry = createRootEntry(entry, filename);
                }
            }
            else
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }
        }

        // Create File pointer to this newly opened file.
        File* file = new File;

        file->filedescriptor = nextFileDescriptor;
        *filedescriptor = file->filedescriptor;
        nextFileDescriptor++;

        // Set file pointer.
        file->filePtr = 0;
        if((flags & O_TRUNC) != 0)
        {
            *(uint32_t*)(entry + DIR_FILE_SIZE) = 0;
        }

        if((flags & O_APPEND) != 0)
        {
            file->filePtr = *(uint32_t*)(entry + DIR_FILE_SIZE);
        }

        file->flags = flags;
        file->mode = mode;
        file->entry = entry;

        openFiles.push_back(file);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMFileClose(int filedescriptor)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        // Locate the file associated with the filedescriptor.
        File* file = findOpenFile(filedescriptor);

        // If no such file has been opened, return failure.
        if(file == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }

        uint16_t date = getCurrDate();

        *(uint16_t*)(file->entry + DIR_LAST_ACC_DATE) = date;


        // Remove the directory from the vector of directories.
        deleteOpenFile(filedescriptor);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMFileRead(int filedescriptor, void* data, int* length)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

      	if(data == NULL || length == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }


        if(filedescriptor < 3) // Standard input.
        {
            ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

            void* read_base;

       	    // Try to allocate the 512 byte minimum.
            TVMStatus status = VMMemoryPoolAllocate(stackID, MAX_READ_SIZE, &read_base);

            while(status  != VM_STATUS_SUCCESS)
       	    {
                 // Block until memory gets freed.
                 currentThread->setWaitingFor(WAITING_MEMORY);

                 myMemoryManager->addToMemoryQueue(currentThread);

                 myScheduler->addToWaiting(currentThread);
                 myScheduler->scheduleNext();

                 status = VMMemoryPoolAllocate(stackID, MAX_READ_SIZE, &read_base);
            }

            int bytesRead = 0;
            int messageSize = *length;
            int numIterations = (int)ceil((double)*length / (double)MAX_READ_SIZE);

            for(int i = 0; i < numIterations; i++)
            {
                // Read up to 512 bytes of the message
                if(messageSize >= MAX_READ_SIZE)
                {
                    MachineFileRead(filedescriptor, read_base, MAX_READ_SIZE, fileHandler, (void*)currentThread);
                }
                else
                {
                    MachineFileRead(filedescriptor, read_base, messageSize, fileHandler, (void*)currentThread);
                }

                currentThread->setWaitingFor(WAITING_IO);
                myScheduler->addToWaiting(currentThread);
                myScheduler->scheduleNext();

       	        if(currentThread->getResult() < 0)
                {
                    VMMemoryPoolDeallocate(stackID, read_base);

                    // See if anyone is waiting for memory.
                    ThreadControlBlock* nextThread = myMemoryManager->getNext();

                    if(nextThread != NULL) // There was a thread waiting for memory.
                    {
                        myScheduler->removeFromWaiting(nextThread->getTID());
                        myScheduler->addToReady(nextThread);
                    }

                    MachineResumeSignals(&sigstate);
                    return VM_STATUS_FAILURE;
                }
                else
                {
                    memcpy(data, read_base, currentThread->getResult());
                    data = (uint8_t*)data + currentThread->getResult();
                    messageSize -= currentThread->getResult();

                    bytesRead += currentThread->getResult();
                }
            }

            *length = bytesRead;

            VMMemoryPoolDeallocate(stackID, read_base);

            // See if anyone is waiting for memory.
            ThreadControlBlock* nextThread = myMemoryManager->getNext();

            if(nextThread != NULL) // There was a thread waiting for memory.
            {
                myScheduler->removeFromWaiting(nextThread->getTID());
                myScheduler->addToReady(nextThread);
                if(nextThread->getPriority() > currentThread->getPriority())
                {
                    myScheduler->addToReady(myScheduler->getCurrentThread());
                    myScheduler->scheduleNext();
                }
            }
        }
        else // File descriptor >= 3.
        {
            File* file = findOpenFile(filedescriptor);

            if(file == NULL)
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }

            uint16_t date = getCurrDate();

            *(uint16_t*)(file->entry + DIR_LAST_ACC_DATE) = date;

            if((file->flags & O_WRONLY) != 0) // Trying to read from a write-only file.
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }

            uint16_t firstCluster = *(uint16_t*)(file->entry + DIR_FIRST_CLUS_LO);
            uint32_t filesize = *(uint32_t*)(file->entry + DIR_FILE_SIZE);
            uint16_t clusterSize = myFileSystem->SectorsPerCluster * myFileSystem->BytesPerSector;

            uint16_t clusterNum = FatSearch(firstCluster, file->filePtr / clusterSize); // Get the cluster number we're reading from.

            unsigned int clusterOffset = file->filePtr % clusterSize;
            unsigned int numReadIn = 0;
            unsigned int amountLeftInCluster;

            unsigned int messageLen = (unsigned int)*length;
            Cluster* currCluster;

            grabMutex();

            while(numReadIn < (unsigned int)*length && file->filePtr != filesize)
            {
                currCluster = findCachedCluster(clusterNum);
                if(currCluster == NULL) // Not found.
               	{
                    currCluster = new Cluster; // Create a new cached cluster entry.
                    createCachedCluster(currCluster, clusterNum);
                    cachedClusters.push_back(currCluster);
               	}

                amountLeftInCluster = clusterSize - clusterOffset;
               	if(amountLeftInCluster > filesize - file->filePtr)
                {
                    amountLeftInCluster = filesize - file->filePtr;
                }

                if(messageLen < amountLeftInCluster)
               	{
                    memcpy(data, (void*)(currCluster->data + clusterOffset), messageLen);

                    numReadIn += messageLen;
                    file->filePtr += messageLen;
                    amountLeftInCluster -= messageLen;
                    data = (uint8_t*)data + messageLen;
                    clusterOffset += messageLen;
                    messageLen = 0;
                    break;

                }
                else // messageLen >= amountLeftInCluster.
                {
                    memcpy(data, (void*)(currCluster->data + clusterOffset), amountLeftInCluster);

                    numReadIn += amountLeftInCluster;
                    file->filePtr += amountLeftInCluster;
                    messageLen -= amountLeftInCluster;
                    clusterOffset += amountLeftInCluster;
                    data = (uint8_t*)data + amountLeftInCluster;
                    amountLeftInCluster = 0;
                }

                if(messageLen > 0) // Not done reading.
                {
                    if(amountLeftInCluster == 0)
                    {
                        if(myFileSystem->FatTable[clusterNum] >= 0xFFF7)
                        {
                            break;
                        }
                        clusterNum = myFileSystem->FatTable[clusterNum];
                        clusterOffset = 0;
                    }
                }
            }
            releaseMutex();

            *length = numReadIn;
        }

        MachineResumeSignals(&sigstate);
       	return VM_STATUS_SUCCESS;
    }

    TVMStatus VMFileWrite(int filedescriptor, void* data, int* length)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(data == NULL || length == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }


        if(filedescriptor < 3) // Standard input.
        {
            ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

            void* write_base;

            // Try to allocate the 512 byte minimum.
            TVMStatus status = VMMemoryPoolAllocate(stackID, MAX_WRITE_SIZE, &write_base);

            while(status != VM_STATUS_SUCCESS)
            {
                 // Block until memory gets freed.
                 currentThread->setWaitingFor(WAITING_MEMORY);

                 myMemoryManager->addToMemoryQueue(currentThread);

                 myScheduler->addToWaiting(currentThread);
                 myScheduler->scheduleNext();
                 status = VMMemoryPoolAllocate(stackID, MAX_WRITE_SIZE, &write_base);
            }

            int bytesWritten = 0;
            int messageSize = *length;
            int numIterations = (int)ceil((double)*length / (double)MAX_WRITE_SIZE);

            for(int i = 0; i < numIterations; i++)
            {
                // Write up to 512 bytes of the message
                if(messageSize >= MAX_WRITE_SIZE)
                {
                    memcpy(write_base, data, MAX_WRITE_SIZE);
                    MachineFileWrite(filedescriptor, write_base, MAX_WRITE_SIZE, fileHandler, (void*)currentThread);
                    data = (uint8_t*)data + MAX_WRITE_SIZE;
                    messageSize -= MAX_WRITE_SIZE;
                }
                else
                {
                    memcpy(write_base, data, messageSize);
                    MachineFileWrite(filedescriptor, write_base, messageSize, fileHandler, (void*)currentThread);
                }

                currentThread->setWaitingFor(WAITING_IO);
                myScheduler->addToWaiting(currentThread);
                myScheduler->scheduleNext();

                if(currentThread->getResult() < 0)
                {
                    VMMemoryPoolDeallocate(stackID, write_base);

                    // See if anyone is waiting for memory.
                    ThreadControlBlock* nextThread = myMemoryManager->getNext();

                    if(nextThread != NULL) // There was a thread waiting for memory.
                    {
                        myScheduler->removeFromWaiting(nextThread->getTID());
                        myScheduler->addToReady(nextThread);
                    }

                    MachineResumeSignals(&sigstate);
                    return VM_STATUS_FAILURE;
                }
                else
                {
                    bytesWritten += currentThread->getResult();
                }
            }

            *length = bytesWritten;

            // Deallocate the memory we were using.
            VMMemoryPoolDeallocate(stackID, write_base);

            // See if anyone is waiting for memory.
            ThreadControlBlock* nextThread = myMemoryManager->getNext();

            if(nextThread != NULL) // There was a thread waiting for memory.
            {
                myScheduler->removeFromWaiting(nextThread->getTID());
                myScheduler->addToReady(nextThread);
                if(nextThread->getPriority() > currentThread->getPriority())
                {
                    myScheduler->addToReady(myScheduler->getCurrentThread());
                    myScheduler->scheduleNext();
                }
            }
        }
        else // File descriptor >= 3
        {
            File* file = findOpenFile(filedescriptor);

            if(file == NULL)
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }

            uint16_t date = getCurrDate();
            uint16_t time = getCurrTime();

            *(uint16_t*)(file->entry + DIR_LAST_ACC_DATE) = date;

            if((file->flags & O_RDONLY) != 0) // Trying to write from a read-only file.
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }

            *(uint16_t*)(file->entry + DIR_WRITE_DATE) = date;
            *(uint16_t*)(file->entry + DIR_WRITE_TIME) = time;

            uint16_t firstCluster = *(uint16_t*)(file->entry + DIR_FIRST_CLUS_LO);
            uint32_t filesize = *(uint32_t*)(file->entry + DIR_FILE_SIZE);
            uint16_t clusterSize = myFileSystem->SectorsPerCluster * myFileSystem->BytesPerSector;

            uint16_t clusterNum = FatSearch(firstCluster, file->filePtr / clusterSize); // Get the cluster number we're reading from.

            unsigned int clusterOffset = file->filePtr % clusterSize;
            unsigned int numWritten = 0;
            unsigned int amountLeftInCluster;

            unsigned int messageLen = (unsigned int)*length;
            Cluster* currCluster;

            grabMutex();

            while(numWritten < (unsigned int)*length)
            {
                currCluster = findCachedCluster(clusterNum);
                if(currCluster == NULL) // Not found.
                {
                    currCluster = new Cluster; // Create a new cached cluster entry.
                    createCachedCluster(currCluster, clusterNum);
                    cachedClusters.push_back(currCluster);
                }
                amountLeftInCluster = clusterSize - clusterOffset;

                if(messageLen < amountLeftInCluster)
                {
                    memcpy((void*)(currCluster->data + clusterOffset), data, messageLen);

                    numWritten += messageLen;
                    file->filePtr += messageLen;
                    amountLeftInCluster -= messageLen;
                    data = (uint8_t*)data + messageLen;
                    clusterOffset += messageLen;
                    messageLen = 0;
                    break;

                }
                else // messageLen >= amountLeftInCluster.
                {
                    memcpy((void*)(currCluster->data + clusterOffset), data, amountLeftInCluster);

                    numWritten += amountLeftInCluster;
                    file->filePtr += amountLeftInCluster;
                    messageLen -= amountLeftInCluster;
                    clusterOffset += amountLeftInCluster;
                    data = (uint8_t*)data + amountLeftInCluster;
                    amountLeftInCluster = 0;
                }

                if(messageLen > 0)
                {
                    if(amountLeftInCluster == 0)
                    {
                        if(myFileSystem->FatTable[clusterNum] >= 0xFFF7) // need to allocate another cluster
                        {
                            // Find first free cluster in FAT Table.
                            for(int i = 0; i < myFileSystem->FATSize16 * myFileSystem->BytesPerSector / 2; i ++)
                            {
                                if(myFileSystem->FatTable[i] == 0x0000) // Free.
                                {
                                     myFileSystem->FatTable[clusterNum] = i;
                                     myFileSystem->FatTable[i] = 0xFFFF;
                                     break;
                                }
                            }

                            // If no free clusters remain, then break out.
                            if(myFileSystem->FatTable[clusterNum] >= 0xFFF)
                            {
                                break;
                            }
                        }
                        clusterNum = myFileSystem->FatTable[clusterNum];
                        clusterOffset = 0;
                    }
                }
            }

            releaseMutex();

            // If the file pointer has gone passed the previous file size, update the file size.
            if(file->filePtr > filesize)
            {
                *(uint32_t*)(file->entry + DIR_FILE_SIZE) = (uint32_t)file->filePtr;
            }

            *length = numWritten;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }


    TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int* newoffset)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(filedescriptor < 3)
        {
            ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

            MachineFileSeek(filedescriptor, offset, whence, fileHandler, (void*)currentThread);

            currentThread->setWaitingFor(WAITING_IO);
            myScheduler->addToWaiting(currentThread);
            myScheduler->scheduleNext();

            if(currentThread->getResult() < 0)
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }

            if(newoffset != NULL)
            {
                *newoffset = currentThread->getResult();
            }
        }
        else
        {
            File* file = findOpenFile(filedescriptor);

            if(file == NULL)
            {
                MachineResumeSignals(&sigstate);
                return VM_STATUS_FAILURE;
            }

            uint32_t filesize = *(uint32_t*)(file->entry + DIR_FILE_SIZE);

            if(whence == 1) // Current
            {
                file->filePtr += offset;
            }
            else if(whence == 2) // End
            {
                 file->filePtr = filesize;
            }
            else // Beginning.
            {
                 file->filePtr = offset;
            }

            if(file->filePtr < 0)
            {
                file->filePtr = 0;
            }
            else if((unsigned)file->filePtr > filesize)
            {
                file->filePtr = filesize;
            }

            *newoffset = file->filePtr;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }


/*******************************************************************************************************
                                       	Mutex Functions
*******************************************************************************************************/

    TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(mutexref == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        *mutexref = myScheduler->createMutex();

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMutexDelete(TVMMutexID mutex)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        Mutex* mtx = myScheduler->findMutex(mutex);

        if(mtx == NULL) // Not found
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        else if(mtx->isLocked)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_STATE;
        }

        myScheduler->deleteMutex(mutex);

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        if(ownerref == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }

        Mutex* mtx = myScheduler->findMutex(mutex);

        if(mtx == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        if(!(mtx->isLocked)) // unlocked.
        {
       	    *ownerref = VM_THREAD_ID_INVALID;
        }

        else
        {
            *ownerref = mtx->owner;
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        Mutex* mtx = myScheduler->findMutex(mutex);
        if(mtx == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        ThreadControlBlock* curr = myScheduler->getCurrentThread();

        if(!(mtx->isLocked))
        {
            curr->mHeld.push_back(mtx->mid);
            mtx->isLocked = true;
            mtx->owner = curr->getTID();
        }
        else if(timeout == VM_TIMEOUT_IMMEDIATE)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_FAILURE;
        }
        else
        {
            curr->setWaitingFor(WAITING_MUTEX);

            if(timeout == VM_TIMEOUT_INFINITE)
            {
                curr->setInfiniteFlag(true);
                curr->setTicks(-1);
            }
            else
            {
                curr->setTicks(timeout);
            }

            mtx->wantsMutex(curr); // Adds thread to mutex waiting queue.
            myScheduler->addToWaiting(curr);

            myScheduler->scheduleNext();

            mtx->stopWaiting(curr->getTID());
            if(mtx->owner != curr->getTID()) // Didn't get Mutex.
            {
               MachineResumeSignals(&sigstate);
               return VM_STATUS_FAILURE;
            }
        }
        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }

    TVMStatus VMMutexRelease(TVMMutexID mutex)
    {
        TMachineSignalState sigstate;
        MachineSuspendSignals(&sigstate);

        Mutex* mtx = myScheduler->findMutex(mutex);
        if(mtx == NULL)
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_ID;
        }
        else if(!(mtx->isLocked) || mtx->owner != (myScheduler->getCurrentThread())->getTID())
        {
            MachineResumeSignals(&sigstate);
            return VM_STATUS_ERROR_INVALID_STATE;
        }
        else
        {
            // Release mutex.
            ThreadControlBlock* thread = myScheduler->findThread(mtx->owner);
            for(unsigned int i = 0; i < thread->mHeld.size(); i++)
            {
                if(thread->mHeld[i] == mtx->mid)
                {
                    thread->mHeld.erase(thread->mHeld.begin() + i);
                }
            }

            mtx->isLocked = false;
            mtx->owner = 0;


            ThreadControlBlock* newOwner = mtx->getNextOwner();
            if(newOwner != NULL) // Someone got the mutex.
            {
                myScheduler->removeFromWaiting(newOwner->getTID()); // Remove from waiting queue.
                myScheduler->addToReady(newOwner); // That thread is ready to run.
                if(newOwner->getPriority() > myScheduler->getCurrentThread()->getPriority())
                {
                    myScheduler->addToReady(myScheduler->getCurrentThread());
                    myScheduler->scheduleNext();
                }
            }
        }

        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
    }
}
