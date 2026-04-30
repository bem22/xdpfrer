# Phase 1: Tail-Latency CDF Analysis

## Goal

Demonstrate that IEEE 802.1CB FRER (implemented via xdpfrer) removes the long-tail latency spikes inherent to individual Wi-Fi links, by comparing the CDF of one-way delay across five configurations.

## Setup

### Hardware

| Device | Role | Interfaces |
|--------|------|------------|
| FRER-REPL (Raspberry Pi) | Sender / Replicator | aeth0 (veth), wlan0 (192.168.4.148), wlx (192.168.5.88), eth0 (10.42.0.1) |
| FRER-ELIM (Raspberry Pi) | Receiver / Eliminator | clean_in (veth), wlan0 (192.168.4.111), wlx (192.168.5.152), eth0 (10.42.0.2) |
| SAL-AP1 | Wi-Fi Access Point | 192.168.4.0/24 subnet |
| SAL-AP2 | Wi-Fi Access Point | 192.168.5.0/24 subnet |

### Clock Synchronization

- PTP via `ptp4l -i eth0 -H -2` over Ethernet (dedicated 10.42.0.0/24 link)
- `phc2sys -s /dev/ptp0 -c CLOCK_REALTIME -O 0` syncing system clock to PHC
- Achieved offset: ±60 ns (s2 locked state)

### Network Paths

- **Path A**: REPL wlan0 → SAL-AP1 → ELIM wlan0 (VXLAN VNI 101)
- **Path B**: REPL wlx → SAL-AP2 → ELIM wlx (VXLAN VNI 102)

### Software

- xdpfrer: XDP-based IEEE 802.1CB FRER (generic/SKB mode)
- Custom UDP sender/receiver with nanosecond-precision timestamps

## Experimental Conditions

| Condition | xdpfrer | Sender → | Wi-Fi Path | Description |
|-----------|---------|----------|------------|-------------|
| **RAW-A** | Off | ELIM wlan0 (192.168.4.111) | A only | Baseline, path A |
| **RAW-B** | Off | ELIM wlx (192.168.5.152) | B only | Baseline, path B |
| **FRER-A** | On, 1 egress (vxlan_a) | teth0 (10.0.0.2) | A only | FRER overhead, path A |
| **FRER-B** | On, 1 egress (vxlan_b) | teth0 (10.0.0.2) | B only | FRER overhead, path B |
| **FRER-AB** | On, 2 egress | teth0 (10.0.0.2) | A + B | Full FRER redundancy |

## Traffic Parameters

- Packet rate: 1000 pkt/s (1 ms interval, `-d 1000`)
- Packets per run: 10,000 (`-c 10000`)
- Duration per run: ~10 seconds
- Runs per condition: 5
- Payload: UDP with embedded `CLOCK_REALTIME` timestamp

## Data Format

### Receiver CSV (`run{N}_*_recv.csv`)

```
seq,send_epoch_ns,recv_epoch_ns,latency_ns
0,1777525706269395755,1777525706274123456,4727701
1,1777525706270395755,1777525706275234567,4838812
...
```

### Sender CSV (`run{N}_*_send.csv`)

```
seq,send_epoch_ns
0,1777525706269395755
1,1777525706270395755
...
```

## Directory Structure

```
phase-1/
├── README.md
├── analysis.ipynb
├── rawA/          # 5 receiver CSVs + 5 sender CSVs
├── rawB/
├── frerA/
├── frerB/
└── frerAB/
```

## Key Comparisons

1. **RAW vs FRER-single** (RAW-A vs FRER-A, RAW-B vs FRER-B) — quantifies XDP/VXLAN processing overhead
2. **FRER-single vs FRER-AB** — isolates the tail-latency improvement from redundancy
3. **RAW vs FRER-AB** — bottom line: does redundancy benefit outweigh processing cost?

## Expected Result

If link A has spike probability $p_A$ and link B has independent spike probability $p_B$, FRER eliminates packets where *both* links spike simultaneously. For $p_A = p_B = 0.01$, combined spike probability drops to $p_A \times p_B = 0.0001$ — a 100× improvement in the tail.

## Fixes Applied During Testing

1. **Duplicate VXLAN FDB entries** — caused packet duplication; fixed by deleting stale entries
2. **BPF_F_BROADCAST with DEVMAP_HASH** — didn't iterate all entries in SKB mode; switched to `BPF_MAP_TYPE_DEVMAP` (array)
3. **Shared packet data in SKB broadcast clones** — second clone's VLAN translation failed because first clone already modified the VID; fixed by removing the `from` VID check during replication postprocessing
4. **phc2sys not running** — `CLOCK_REALTIME` not synced to PTP; fixed by adding `phc2sys -s /dev/ptp0 -c CLOCK_REALTIME -O 0`
