# EE 542 Lab 2 – Fast, Reliable File Transfer

This repository contains a reliable UDP/IP file-transfer utility implemented in C++17 for Linux.

## Repository Structure

```text
.
├── include/     # Protocol definitions
├── src/         # Sender and receiver source code
├── images/      # Experimental screenshots
├── Makefile     # Build configuration
└── report.md    # Design, results, and analysis
```

## Build and Run

```bash
make
./receiver <port> <output_file>
./sender <receiver_ip> <port> <input_file> [payload_size] [window_size] [pacing_rate_mbps]
```

Start the receiver before the sender. See [report.md](report.md) for the protocol design and experimental results.
