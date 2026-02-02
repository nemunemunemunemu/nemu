#!/bin/sh
rm -f logs/*
for var in $( seq 0 255 )
do
    printf "Testing %X\n" $var
    bin/run_sst $1 $var >> logs/results.txt
done
