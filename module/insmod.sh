#!/bin/sh

sudo dmesg -n7
sudo rmmod walb
sudo insmod walb.ko ddev_major=8 ddev_minor=0 ldev_major=8 ldev_minor=16 ndevices=1
