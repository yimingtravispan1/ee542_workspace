# EE 542 Lab 2 – Fast, Reliable File Transfer

## Part 1: Simulating Networking Environments

### Case 2: RTT 200 ms, Packet Loss 20%

**Configuration:** RTT = 200 ms, packet loss = 20%, network rate = 100 Mbps.

#### Results

##### Ping: Client → Server

![Ping Client to Server](images/case2_ping_client_to_server.png)

##### Ping: Server → Client

![Ping Server to Client](images/case2_ping_server_to_client.png)

##### UDP iperf: Client → Server

![UDP iperf Client to Server](images/case2_udp_client_to_server.png)

##### UDP iperf: Server → Client

![UDP iperf Server to Client](images/case2_udp_server_to_client.png)

##### TCP iperf: Client → Server

![TCP iperf Client to Server](images/case2_tcp_client_to_server.png)

#### Summary

| Test | Direction | Average RTT | Throughput / Bandwidth | Packet Loss |
|---|---|---:|---:|---:|
| Ping | Client → Server | 201.375 ms | - | 37% |
| Ping | Server → Client | 201.418 ms | - | 35.5% |
| UDP iperf | Client → Server | - | 77.6 Mbits/sec | 22% |
| UDP iperf | Server → Client | - | 77.8 Mbits/sec | 22% |
| TCP iperf | Client → Server | - | 95.9 Kbits/sec | - |

#### Observation

UDP throughput remained around 78 Mbps, while TCP throughput dropped significantly under high RTT and packet loss.