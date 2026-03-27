>This revision incorporates the technical prerequisites, environment constraints, and specific kernel requirements necessary for a successful XDP/FRER integration. 

# IEEE 802.1CB FRER: Linux XDP Physical Implementation Guide

_This guide describes the deployment of Frame Replication and Elimination for Reliability (FRER) using XDPFRER from Ericson. It covers the environment requirements, path configuration, and execution of the redundancy logic._

## 1. System Requirements and Prerequisites

Successful XDP integration requires specific kernel features and hardware configurations. Use the following checklist to verify the environment.

### Operating System and Kernel 

- OS: Ubuntu 24.04 LTS or Debian 12 (Recommended)
- Kernel Version: 5.15 or higher. 
- XDP generic mode support and BPF CO-RE (Compile Once – Run Everywhere) require modern kernel headers

- Kernel Configuration: Verify that the following flags are enabled in the kernel config `(usually found in /boot/config-$(uname -r))`:
```
CONFIG_BPF_SYSCALL=y
CONFIG_DEBUG_INFO_BTF=y (Required for CO-RE/Libbpf)
CONFIG_XDP_SOCKETS=y
```

### Hardware and Drivers

- Ethernet: 
    * XDP-compatible NICs are preferred for native performance. 
    * For USB-to-Ethernet adapters (e.g., cdc_ncm), compile the binary in Generic XDP mode

- Wireless: 
    * Standard 802.11 managed mode is used via **VXLAN** 
    * Ensure the wireless driver supports high MTU if large encapsulated frames are expected.

### Development Toolchain
Required for compiling the BPF program and linking against libbpf.

```
sudo apt update && sudo apt install -y \
    clang \ 
    llvm \
    gcc-multilib \
    build-essential \
    libelf-dev \
    libbpf-dev \
    linux-headers-$(uname -r)
```

### Operational Utilities
Required for network configuration, hardware discovery, and frame analysis.

```
sudo apt install -y \
    iproute2 \
    tcpdump \
    bridge-utils \
    arptables \
    arping \
    net-tools
```

## 2. Environment Preparation

### Hardware Address Discovery
> FRER replication requires explicit destination MAC addresses to ensure frames are steered correctly across the Layer 2 segments.

```
# Identify MAC addresses for all physical and virtual interfaces
# Run on both Talker and Listener:
ip link show
```

> VXLAN requires the specific IP addresses of the physical wireless adapters to define the tunnel endpoints. Use the following commands to identify the current IP assignments.
```
# Locate the wireless interface (usually starts with 'wlan' or 'wlx')
# and identify the IPv4 address (inet)
ip -4 addr show scope global
```
Record the IP of the Talker (e.g., 192.168.0.194) and the Listener (e.g., 192.168.0.153). These will be used in the VXLAN local and remote parameters during Path Configuration.

### Connectivity Verification
```
# From the Talker, ping the Listener's wireless IP
ping -c 4 <LISTENER_WIFI_IP>
```

### Memory Locking (RLIMIT_MEMLOCK)
BPF maps require pinned memory. On many systems, the default limit for locked memory is too low for FRER history tables.

Set memlock limit to unlimited for the current session
``` 
ulimit -l unlimited
```
### Disable RP Filter to allow multi-path ingress

```
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
sudo sysctl -w net.ipv4.conf.default.rp_filter=0
```

## 3. Path Configuration 

### Ingress, VLAN, Egress

>This is the interface your application must talk to (for example ping)
```
# Create Ingress Interface (The Replicator's Entrance)
sudo ip link add aeth0 type veth peer name teth0
sudo ip link set dev aeth0 up
sudo ip link set dev teth0 up
```

>This is where the packets get the vlan tag
```
sudo ip link add link teth0 name teth0.100 type vlan id 100
sudo ip link set dev teth0.100 up
sudo ip addr add 10.0.0.1/24 dev teth0.100
```

>This is where your receiving application can listen
```
sudo ip link add elim_out type veth peer name clean_in
sudo ip link set dev elim_out up
sudo ip link set dev clean_in up

# Assign the Listener's test IP to the clean delivery interface
sudo ip addr add 10.0.0.2/24 dev clean_in
```


### Ethernet (Path A)
Direct XDP attachment to USB drivers often fails due to missing ndo_xdp_xmit support. Resolve this via a **veth bridge relay**

#### Egress path
```
sudo ip link add exit_in_eth type veth peer name exit_out_eth
sudo ip link set dev exit_in_eth up
sudo ip link set dev exit_out_eth up
```

#### Create bridge and attach physical interface

```
# Create and assemble the bridge
sudo brctl addbr frer_bridge_eth
sudo brctl addif frer_bridge_eth exit_out_eth
sudo brctl addif frer_bridge_eth <PHYSICAL_ETH_IFACE>

# Disable MAC learning (Hub mode)
sudo brctl setageing frer_bridge_eth 0
sudo brctl stp frer_bridge_eth off

# Force flooding on the member ports
## This ensures the bridge doesn't silently drop non-IP/unknown unicast frames
sudo bridge link set dev exit_out_eth learning off flood on
sudo bridge link set dev <PHYSICAL_ETH_IFACE> learning off flood on

sudo ip link set dev frer_bridge_eth up
```

### Wireless (Path B)
> To integrate the wireless path successfully into an IEEE 802.1CB FRER architecture, the primary challenge is the transparency of the medium. Standard 802.11 Access Points (APs) are designed to bridge Ethernet and IP traffic; they typically discard frames with unknown Ethertypes like 0xf1c1.

VXLAN (Virtual eXtensible LAN) encapsulates the L2 FRER frame inside a standard Layer 3 UDP/IP packet.

#### Talker Configuration:
```
sudo ip link add vxlan0 type vxlan id 100 \
    remote <LISTENER_WIFI_IP> \
    local <TALKER_WIFI_IP> \
    dstport 4789 \
    dev <WIFI_IFACE>

sudo ip link set dev vxlan0 up
```

#### Listener Configuration:
```
sudo ip link add vxlan0 type vxlan id 100 \
    remote <TALKER_WIFI_IP> \
    local <LISTENER_WIFI_IP> \
    dstport 4789 \
    dev <WIFI_IFACE>

sudo ip link set dev vxlan0 up
```

### ARP Table
>Since the interfaces are now initialized and the IP addresses are discovered, we can proceed with the Static ARP Configuration. This step is critical because standard ARP requests (Broadcast) may not behave predictably across the XDP-enabled FRER paths, especially when bypassing the kernel's normal IP stack

#### On the Talker
Target the Listener's IP on the test network (10.0.0.2) and bind it to the Listener's physical MAC address.

```
# Associate the Listener's IP with its hardware MAC on the VLAN interface
sudo arp -s 10.0.0.2 <LISTENER_PHYSICAL_MAC> -i teth0.100
```

#### On the Listener 
Target the Talker's IP on the test network (10.0.0.1) and bind it to the Talker's physical MAC address.

```
# Associate the Talker's IP with its hardware MAC on the VLAN interface
sudo arp -s 10.0.0.1 <TALKER_PHYSICAL_MAC> -i leth0.100
```

#### Verification of Neighbor State
Ensure the entries are marked as PERM (Permanent) or have the CM flag, indicating they will not expire or be overwritten by the kernel.

```
# Display the ARP table for the specific FRER interface
arp -n -i teth0.100
Expected Output:
Address        HWtype  HWaddress           Flags Mask  Iface
10.0.0.2       ether   2c:cf:67:32:4f:9a   CM          teth0.100
```

## FRER Deployment
### Replicator Execution (Talker)
The Replicator sits at the ingress of the pipeline. It intercepts standard traffic arriving from the application interface, clones it, applies the FRER R-TAG and VLAN tag, and egresses the duplicates across Path A and Path B.

```
# Ingress: aeth0 (Intersects traffic from teth0.100)
# Egress 1: exit_in_eth (Path A Relay)
# Egress 2: vxlan0 (Path B Wireless Tunnel)
# Tag: 100
sudo ./xdpfrer -m repl -i aeth0:100 -e exit_in_eth:100 -e vxlan0:100
```

### Eliminator Execution (Listener)
The Eliminator attaches to both physical and virtual ingress points. It maintains a history of sequence numbers; if a sequence number has already been processed, the duplicate is dropped. Valid packets are stripped of FRER headers and sent to the "Clean" interface.

```
# Ingress 1: eth0:100 (Straight from ethernet on path A)
# Ingress 2: vxlan0:100 (Path B wireless tunnel)
# Egress: 
sudo ./xdpfrer -m elim -i eth0:100 -i vxlan0:100 -e elim_out:100 &

```