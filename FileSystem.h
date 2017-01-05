#include "VirtualMachine.h"
#include "ThreadControlBlock.h"
#include "Scheduler.h"
#include <sys/types.h>
#include <fcntl.h>
#include <math.h>
#include "string.h"
#include <iostream>
#include <iomanip>
using namespace std;

#ifndef FILE_SYSTEM_H
#define FILE_SYSTEM_H

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

    extern ThreadControlBlock* currentThread;
    extern TVMMutexID FILE_SYSTEM_MUTEX;

    extern void waitForIO();
    extern void fileHandler(void* calldata, int result);

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
        private:
            Scheduler* myScheduler;

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
            FileSystem(char* mount, int fileDescriptor, void* base, Scheduler* myScheduler);
            ~FileSystem();

            char* getCWD();
            uint8_t* getRoot();

            void processBPB();
            void processFAT();
            void processRoot();

            void readSector(int sector, uint8_t* base, int size);
            void writeSector(int sector, uint8_t* base, int size);
    };
}

#endif
