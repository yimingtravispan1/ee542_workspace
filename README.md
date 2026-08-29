# EE 542 Lab 2 – Fast, Reliable File Transfer

This repository contains the source code, experimental results, and report for EE 542 Laboratory #2.

## Repository Structure

```text


├── images/      # Experimental screenshots
├── src/         # Source code
├── README.md    # Repository overview
└── report.md    # Experimental results and analysis
## Case 1 Results

### Network Configuration

Case 1 was configured with the following target conditions:

- Bandwidth limit: **100 Mbit/s**
- RTT target: **10 ms**
- Packet loss: **1% in each direction**
- Client and Server egress interfaces were rate-limited using TBF.
- Both VyOS interfaces were also rate-limited to 100 Mbit/s.
- Due to performance differences in the VMware environment, the TBF parameters were adjusted while keeping the rate fixed at 100 Mbit/s.

Final TBF parameters used:

```bash
rate 100mbit latency 1ms burst 90155
100 Mbit/s Rate-Limit Verification

Before applying delay and packet loss, TCP throughput was tested to verify the bandwidth limitation.

Metric	Result
Unrestricted baseline TCP	4.31 Gbit/s
TCP sender throughput after rate limiting	96.0 Mbit/s
TCP receiver throughput after rate limiting	95.6 Mbit/s
TCP retransmissions	0

The result confirms that the configured link rate is stable at approximately 100 Mbit/s.

Bidirectional Ping Verification

After applying 5 ms delay and 1% packet loss on both VyOS interfaces, ping was tested in both directions.

Direction	Sent	Received	Packet Loss	RTT min / avg / max / mdev
Server → Client	200	196	2.0%	10.494 / 11.887 / 14.273 / 0.613 ms
Client → Server	200	195	2.5%	10.480 / 11.774 / 13.644 / 0.594 ms

The average RTT was approximately 11.8 ms, which is close to the target 10 ms RTT considering the baseline delay of the virtual network.

Because 1% loss is independently applied in both directions, the observed round-trip ping loss can be close to 2%.

UDP Verification

UDP tests were performed in both directions at an offered rate of:

100 Mbit/s

The tests were used to verify that packet loss was applied in both directions under the Case 1 network condition.

TCP Performance Under Case 1

TCP throughput was measured again after applying the full Case 1 delay and packet-loss conditions.

Metric	Result
TCP sender throughput	24.2 Mbit/s
TCP receiver throughput	23.8 Mbit/s
TCP retransmissions	150

Compared with the rate-limit-only baseline of approximately 96 Mbit/s, TCP throughput decreased significantly after introducing RTT and packet loss.

This behavior is expected because packet loss causes TCP retransmissions and congestion-control responses, which reduce the effective throughput.

Case 1 Summary

The Case 1 network environment was successfully configured and verified with:

Approximately 100 Mbit/s bandwidth limit
Approximately 11.8 ms RTT
1% packet loss configured in each direction
Bidirectional ping and UDP verification
Final TCP throughput of approximately 24 Mbit/s

The environment is ready for reliable file-transfer protocol testing.
