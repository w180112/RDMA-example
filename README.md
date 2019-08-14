# RDMA programming example

[![BSD license](https://img.shields.io/badge/License-BSD-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

Send 2 numbers to server then send back the sum

## System required:

RDMA capable NIC, Linux kernel > 3.10, libverbs and librdmacm.

## How to use:

Git clone this repository at both client and server

    # git clone https://github.com/w180112/RDMA-example.git

Set iptables to make UDP port 4791 open

On client side,

    # gcc -o client rdma_write_client.c -lrdmacm -libverbs
    # ./client <servername or ip> <val1> <val2>
e.g.

    # ./client 192.168.0.168 123 456

On server side,

    # gcc -o server rdma_write_server.c -lrdmacm -libverbs
    # ./server

## Test environment

1. Mellanox Connectx-4 Lx with SRIOV enable
2. AMD R7-2700 + 64GB RAM
