#!/bin/sh
DATAPATH=$1
TMPPATH=/tmp/update
TMPDATA=$TMPPATH/tmpdata.bin
SMF_MTD=/dev/mtd1
ADDR=917504
ONESIZE=524288
MTD_PART_SIZE=1294336

mkdir -p $TMPPATH

cd $DATAPATH
TARGETFILE=zImage

if [ ! -e $TARGETFILE ]
then
    echo Error: zImage not found on card
    exit 1
fi

set -- `wc -c $TARGETFILE`
DATASIZE=$1

if [ $DATASIZE -gt $MTD_PART_SIZE ]
then
    echo Error: kernel too big for flash slot
    exit 1
fi

echo Flashing kernel to /dev/mtd1 at offset 917504

DATAPOS=0
while [ $DATAPOS -lt $DATASIZE ]
do
    /sbin/bcut -a $DATAPOS -s $ONESIZE -o $TMPDATA $TARGETFILE
    set -- `wc -c $TMPDATA`
    TMPSIZE=$1
    DATAPOS=`expr $DATAPOS + $TMPSIZE`
    /sbin/nandlogical $SMF_MTD WRITE $ADDR $TMPSIZE $TMPDATA > /dev/null 2>&1
    ADDR=`expr $ADDR + $TMPSIZE`
    rm -f $TMPDATA
    echo wrote $DATAPOS of $DATASIZE
done

echo Kernel flash complete. Power off and reboot.
exit 0
