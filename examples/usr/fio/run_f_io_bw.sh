#!/bin/bash


export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../../src/usr/:../../../examples/usr/raio/

taskset -c 1 $FIO_ROOT/fio ./raio-read-bw.fio

wait
