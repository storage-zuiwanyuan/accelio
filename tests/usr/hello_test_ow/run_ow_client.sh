#!/bin/bash

export LD_LIBRARY_PATH=../../../src/usr/

server_ip=192.168.20.127
#server_ip=192.168.20.236
#server_ip=1.1.1.31
port=2234

#./xio_ow_client -c 1 -p ${port} -n 0 -w 0 ${server_ip}
./xio_ow_client -c 8 -p ${port} -n 0 -w 1024 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 4096 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 8192 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 16384 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 32768 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 65536 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 131072 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 262144 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 524288 ${server_ip}
#./xio_ow_client -c 1 -p ${port} -n 0 -w 1048576 ${server_ip}


