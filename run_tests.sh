#!/bin/sh
for var in $( seq 0 255 )
do
    bin/run_sst "$1" $var
done
