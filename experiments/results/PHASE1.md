# Phase 1: Tail-Latency CDF Analysis — Proving FRER Smooths Wireless Jitter

## Goal

Demonstrate that xdpfrer's packet replication and elimination removes the long-tail latency spikes inherent to individual Wi-Fi links, by comparing the Cumulative Distribution Function (CDF) of one-way delay across five configurations.

## Metric

Per-packet One-Way Delay (OWD), nanosecond precision, clocks PTP-synced over Ethernet.

## Tool

Custom `sender`/`receiver` — pure unidirectional UDP, no handshake, compatible with xdpfrer's forward-only path.

## Traffic

`sender -d 2000` (2ms interval → 500 pkt/s), 10,000 packets per run for statistical significance.

---

## Experimental Conditions

| Run | Label      | xdpfrer               | Sender target                    | Path                                                                      |
|-----|------------|------------------------|----------------------------------|---------------------------------------------------------------------------|
| 1   | **RAW-A**  | Off                    | ELIM's wlan0 IP (192.168.5.x)   | REPL wlan0 → AP → ELIM wlan0 → receiver                                  |
| 2   | **RAW-B**  | Off                    | ELIM's wlx1 IP (192.168.4.x)    | REPL wlx1 → AP → ELIM wlx1 → receiver                                   |
| 3   | **FRER-A** | On, 1 egress (wlan0)   | teth0 endpoint                   | REPL teth0 → xdpfrer → wlan0 → AP → ELIM → xdpfrer → receiver           |
| 4   | **FRER-B** | On, 1 egress (wlx1)    | teth0 endpoint                   | REPL teth0 → xdpfrer → wlx1 → AP → ELIM → xdpfrer → receiver            |
| 5   | **FRER-AB**| On, 2 egress           | teth0 endpoint                   | REPL teth0 → xdpfrer → wlan0+wlx1 → APs → ELIM → xdpfrer → receiver     |

**RAW-A/RAW-B** are zero-overhead baselines: xdpfrer stopped on both Pis, sender/receiver talk directly over each Wi-Fi subnet. Linux handles routing naturally — sender targets the ELIM's IP on the 192.168.5.0/24 or 192.168.4.0/24 subnet, which routes out the corresponding WLAN interface.

**FRER-A/FRER-B** isolate the processing overhead of the full xdpfrer+VXLAN+VLAN stack on a single link.

**FRER-AB** is the full redundancy case.

---

## Execution

```bash
# ── RAW-A ──────────────────────────────────────────────
# Stop xdpfrer on REPL and ELIM
# ELIM:
./receiver -p 9999 -c 10000 -o rawA
# REPL:
./sender -i 192.168.5.<elim> -p 9999 -c 10000 -d 2000 -o rawA

# ── RAW-B ──────────────────────────────────────────────
# ELIM:
./receiver -p 9999 -c 10000 -o rawB
# REPL:
./sender -i 192.168.4.<elim> -p 9999 -c 10000 -d 2000 -o rawB

# ── FRER-A (start xdpfrer with 1 egress: wlan0) ──────
# ELIM:
./receiver -p 9999 -c 10000 -o frerA
# REPL:
./sender -i <teth0_endpoint> -p 9999 -c 10000 -d 2000 -o frerA

# ── FRER-B (reconfigure xdpfrer: 1 egress: wlx1) ────
# ELIM:
./receiver -p 9999 -c 10000 -o frerB
# REPL:
./sender -i <teth0_endpoint> -p 9999 -c 10000 -d 2000 -o frerB

# ── FRER-AB (reconfigure xdpfrer: 2 egress) ──────────
# ELIM:
./receiver -p 9999 -c 10000 -o frerAB
# REPL:
./sender -i <teth0_endpoint> -p 9999 -c 10000 -d 2000 -o frerAB
```

---

## Analysis

1. Load all 5 CSVs in a Jupyter notebook
2. Plot overlaid CDFs of `latency_ns` for all runs
3. Extract percentile table:

| | P50 | P95 | P99 | P99.9 | P99.99 |
|--|-----|-----|-----|-------|--------|
| RAW-A | | | | | |
| RAW-B | | | | | |
| FRER-A | | | | | |
| FRER-B | | | | | |
| FRER-AB | | | | | |

### Three comparisons to make

1. **RAW vs FRER-single** (RAW-A vs FRER-A, RAW-B vs FRER-B) — quantifies the XDP/VXLAN/VLAN processing overhead per link
2. **FRER-single vs FRER-AB** — proves the tail-latency improvement from redundancy alone
3. **RAW vs FRER-AB** — the bottom line: does the redundancy benefit outweigh the processing cost?

### Key claim

If Link A has spike probability $p_A$ and Link B has independent spike probability $p_B$, the probability of both spiking simultaneously is $p_A \times p_B$. For $p_A = p_B = 0.01$: combined spike probability drops to $0.0001$ — a 100× improvement in the tail. FRER-AB's P99.9 should be significantly lower than all single-link runs.
