#!/bin/bash

SHELL_PATH=$(cd `dirname $0`; pwd)
SCRIPT_PATH=$SHELL_PATH/script
OUT_PATH=$SHELL_PATH/m5out

for file in $SCRIPT_PATH/*
do
    filename=${file##*/}
    script=$file

    # echo status ${filename} >${OUT_PATH}/status.txt
    touch ${OUT_PATH}/stats.txt
    echo running script $filename
    $script
    echo 'done'
    mv $OUT_PATH/stats.txt $OUT_PATH/${filename%.*}.txt
done
