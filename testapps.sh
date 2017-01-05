#!/bin/bash
make clean
make
make apps
echo "Testing badprogram"
./vm ./badprogram.so > myBadprogram.txt
./Given/vm_proj4 ./badprogram.so > hisBadprogram.txt
diff myBadprogram.txt hisBadprogram.txt

echo "Testing badprogram2"
./vm ./badprogram2.so > myBadprogram2.txt
./Given/vm_proj4 ./badprogram2.so > hisBadprogram2.txt

diff myBadprogram2.txt hisBadprogram2.txt
echo "Testing file"
./vm ./file.so > myFile.txt
./Given/vm_proj4 ./file.so > hisFile.txt
diff myFile.txt hisFile.txt

echo "Testing file2"
./vm ./file2.so > myFile2.txt
./Given/vm_proj4 ./file2.so > hisFile2.txt
diff myFile2.txt hisFile2.txt

echo "Testing hello"
./vm ./hello.so > myHello.txt
./Given/vm_proj4 ./hello.so > hisHello.txt
diff myHello.txt hisHello.txt

echo "Testing memory"
./vm ./memory.so > myMemory.txt
./Given/vm_proj4 ./memory.so > hisMemory.txt
diff myMemory.txt hisMemory.txt
echo "Testing mutex"

./vm ./mutex.so > myMutex.txt
./Given/vm_proj4 ./mutex.so > hisMutex.txt
diff myMutex.txt hisMutex.txt

echo "Testing sleep"
./vm ./sleep.so > mySleep.txt
./Given/vm_proj4 ./sleep.so > hisSleep.txt
diff mySleep.txt hisSleep.txt

echo "Testing thread"
./vm ./thread.so > myThread.txt
./Given/vm_proj4 ./thread.so > hisThread.txt
diff myThread.txt hisThread.txt

echo "Testing preempt"
./vm ./preempt.so > myPreempt.txt
./Given/vm_proj4 ./preempt.so > hisPreempt.txt
diff myPreempt.txt hisPreempt.txt

rm -f ./my*.txt ./his*.txt ./longtest.txt ./test.txt
make clean
