#include "FileSystem.h"

extern "C"
{
	FileSystem::FileSystem(char* mount, int fileDescriptor, void* base, Scheduler* myScheduler)
	{
                this->myScheduler = myScheduler;

		this->cwd[0] = VM_FILE_SYSTEM_DIRECTORY_DELIMETER; // '/'
		this->cwd[1] = '\0';

		this->mount         = mount;
		this->fileDescriptor = fileDescriptor;
		this->base          = (uint8_t*)base;

		processBPB();
		processFAT();
		processRoot();

		FirstRootSector = ReservedSectorCount + NumFATs * FATSize16;
		FirstDataSector = FirstRootSector + (RootEntryCount * BYTES_PER_ENTRY / SECTOR_SIZE);
	}

	FileSystem::~FileSystem()
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
			root += SECTOR_SIZE;

			writeSector(this->ReservedSectorCount + (this->NumFATs * this->FATSize16) + i, base, MAX_WRITE_SIZE);
		}

		releaseMutex();

		// Delete the root entries.
		delete[] RootEntries;
	}

	char* FileSystem::getCWD()
	{
		return this->cwd;
	}

	uint8_t* FileSystem::getRoot()
	{
		return this->RootEntries;
	}

	void FileSystem::processBPB()
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

	void FileSystem::processFAT()
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

	void FileSystem::processRoot()
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

	void FileSystem::readSector(int sector, uint8_t* base, int size)
	{
		ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

		MachineFileSeek(this->fileDescriptor, sector * this->BytesPerSector, 0, fileHandler, (void*)currentThread);
		waitForIO();

		MachineFileRead(this->fileDescriptor, this->base, size, fileHandler, (void*)currentThread);
		waitForIO();

		memcpy((void*)base, this->base, size);
	}


	void FileSystem::writeSector(int sector, uint8_t* base, int size)
	{
		ThreadControlBlock* currentThread = myScheduler->getCurrentThread();

		MachineFileSeek(this->fileDescriptor, sector * this->BytesPerSector, 0, fileHandler, (void*)currentThread);
		waitForIO();

		memcpy(this->base, (void*)base, size);

		MachineFileWrite(this->fileDescriptor, this->base, size, fileHandler, (void*)currentThread);
		waitForIO();
	}
}
