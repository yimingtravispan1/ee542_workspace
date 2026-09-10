# EE 542 Lab 2 – Fast, Reliable File Transfer

This repository contains a reliable UDP/IP file-transfer utility implemented in C++17 for Linux.

## Repository Structure

```text
.
├── src/         # reliable_udp souce code
├── Makefile     # Build configuration
└── report.md    # Design and results
```

## Build and Run

```bash
Start Server before Client
Server:
g++ -O2 -std=c++17 -pthread reliable_udp.cpp -o reliable
./reliable server 51719 received.bin 2

Client:
1. create a 1GiB file : 
dd if=/dev/urandom of=data.bin bs=1M count=1024 status=progress
2. compile and run
g++ -O2 -std=c++17 -pthread quic.cpp -o reliable
./reliable client 192.168.10.100 51719 data.bin 2
```
