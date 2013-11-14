#!/bin/bash


export LD_LIBRARY_PATH=../../../src/usr/

server_ip=192.168.20.126
#server_ip=192.168.20.236
port=1234

./xio_oneway_server -c 7 -p ${port} -n 0 -w 1024 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 768 -w 512 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 0 -w 16384 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 0 -w 32768 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 0 -w 65536 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 0 -w 131072 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 0 -w 262144 ${server_ip}
#./xio_oneway_server -c 1 -p ${port} -n 0 -w 1048576 ${server_ip}

