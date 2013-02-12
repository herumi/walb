#!/bin/sh

#
# You can test walb-log related commands.
# You must have privilege of 'disk' group to use losetup commands.
# /dev/loop0 and /dev/loop1 will be used.
#

LDEV=ldev32M
DDEV=ddev32M
WLOG=wlog
CTL=../walbctl
LOOP0=/dev/loop0
LOOP1=/dev/loop1

format_ldev()
{
  dd if=/dev/zero of=$LDEV bs=1M count=32
  dd if=/dev/zero of=${DDEV}.0 bs=1M count=32
  losetup $LOOP0 $LDEV
  losetup $LOOP1 ${DDEV}.0
  $CTL format_ldev --ldev $LOOP0 --ddev $LOOP1
  RING_BUFFER_SIZE=$(./wlinfo $LOOP0 |grep ringBufferSize |awk '{print $2}')
  echo $RING_BUFFER_SIZE
  sleep 1
  losetup -d $LOOP0
  losetup -d $LOOP1
}

#
# Initialization.
#
format_ldev
./wlgen -s 32M -z 16M --maxPackSize 4M -o ${WLOG}.0
./wlredo ${DDEV}.0 < ${WLOG}.0

#
# Simple test.
#
./wlrestore --verify $LDEV < ${WLOG}.0
./wlcat $LDEV -v -o ${WLOG}.1
./bdiff -b 512 ${WLOG}.0 ${WLOG}.1
if [ $? -ne 0 ]; then
  echo "TEST1_FAILURE"
  exit 1
fi

restore_test()
{
  local testId=$1
  local lsidDiff=$2
  local invalidLsid=$3
  local ret0
  local ret1
  local ret2

  dd if=/dev/zero of=${DDEV}.1 bs=1M count=32
  dd if=/dev/zero of=${DDEV}.2 bs=1M count=32
  dd if=/dev/zero of=${DDEV}.3 bs=1M count=32
  ./wlrestore $LDEV --verify --lsidDiff $lsidDiff --invalidLsid $invalidLsid < ${WLOG}.0
  ./wlcat $LDEV -v -o ${WLOG}.1
  losetup $LOOP0 ${LDEV}
  $CTL cat_wldev --wldev $LOOP0 > ${WLOG}.2
  sleep 1
  losetup -d $LOOP0
  ./bdiff -b 512 ${WLOG}.1 ${WLOG}.2
  if [ $? -ne 0 ]; then
    echo ${WLOG}.1 and ${WLOG}.2 differ.
    echo "TEST${testId}_FAILURE"
    exit 1
  fi

  ./wlredo ${DDEV}.1 < ${WLOG}.1
  losetup $LOOP1 ${DDEV}.2
  $CTL redo_wlog --ddev $LOOP1 < ${WLOG}.1
  sleep 1
  losetup -d $LOOP1
  losetup $LOOP0 ${LDEV}
  losetup $LOOP1 ${DDEV}.3
  $CTL redo --ldev $LOOP0 --ddev $LOOP1
  sleep 1
  losetup -d $LOOP0
  losetup -d $LOOP1
  if [ $invalidLsid -eq -1 ]; then
    ./bdiff -b 512 ${DDEV}.0 ${DDEV}.1
    if [ $? -ne 0 ]; then
      echo "failed: ./bdiff -b 512 ${DDEV}.0 ${DDEV}.1"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ./bdiff -b 512 ${DDEV}.0 ${DDEV}.2
    if [ $? -ne 0 ]; then
      echo "failed: ./bdiff -b 512 ${DDEV}.0 ${DDEV}.2"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ./bdiff -b 512 ${DDEV}.0 ${DDEV}.3
    if [ $? -ne 0 ]; then
      echo "failed: ./bdiff -b 512 ${DDEV}.0 ${DDEV}.3"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
  else
    ./bdiff -b 512 ${DDEV}.1 ${DDEV}.2
    if [ $? -ne 0 ]; then
      echo "failed: ./bdiff -b 512 ${DDEV}.1 ${DDEV}.2"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
    ./bdiff -b 512 ${DDEV}.1 ${DDEV}.3
    if [ $? -ne 0 ]; then
      echo "failed: ./bdiff -b 512 ${DDEV}.1 ${DDEV}.3"
      echo "TEST${testId}_FAILURE"
      exit 1
    fi
  fi
}

restore_test 3 $(expr $RING_BUFFER_SIZE - 1) -1
restore_test 4 $(expr $RING_BUFFER_SIZE - 2) -1
restore_test 5 0 1024 #512KB
restore_test 6 0 8192 #4MB

echo TEST_SUCCESS
exit 0