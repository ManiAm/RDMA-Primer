# Configuring ConnectX-4 for RoCEv2 (100GbE)

This document covers switching a ConnectX-4 VPI adapter from InfiniBand mode to Ethernet mode, configuring it for RoCEv2 traffic, and verifying end-to-end RDMA over Ethernet. The ConnectX-4 is provisioned and MFT is running (see [ConnectX-4 Provisioning](06_README_SETUP_CX_4.md)).

## Background

**Firmware-driven configuration:** Unlike older generations (ConnectX-3) where the OS driver dictated the port protocol on every boot, the ConnectX-4 stores the port mode permanently in NVRAM. The OS driver (`mlx5_core`) simply reads whatever the hardware is set to. Changes require `mlxconfig` and a reboot.

**RoCEv2:** When a ConnectX-4 port is set to Ethernet mode, RDMA traffic is encapsulated inside standard UDP/IP packets (destination port 4791). This makes it fully routable across standard Ethernet switches, unlike native InfiniBand which requires a dedicated fabric.

**Speed:** The same QSFP28 port that delivers EDR InfiniBand (100 Gb/s) supports 100GBASE-CR4 Ethernet at the same speed.



## Changing the Port Protocol

On **both workstations**, query the current link type:

    sudo mlxconfig -d /dev/mst/mt4115_pciconf0 query | grep LINK_TYPE

```text
LINK_TYPE_P1                                IB(1)
```

Switch to Ethernet:

    sudo mlxconfig -y -d /dev/mst/mt4115_pciconf0 set LINK_TYPE_P1=2

Reboot (required — the PCIe bus must re-initialize the silicon):

    sudo reboot



## Verifying the Hardware State

After reboot, confirm the port is now operating as Ethernet:

    ibstat

```text
CA 'rocep3s0'
        CA type: MT4115
        Number of ports: 1
        Firmware version: 12.28.2040
        Hardware version: 0
        Node GUID: 0xec0d9a030044c34c
        System image GUID: 0xec0d9a030044c34c
        Port 1:
                State: Down
                Physical state: Disabled
                Rate: 40
                Base lid: 0
                LMC: 0
                SM lid: 0
                Capability mask: 0x00010000
                Port GUID: 0xee0d9afffe44c34c
                Link layer: Ethernet
```

Key changes from InfiniBand mode:

| Field      | InfiniBand             | Ethernet (RoCEv2) |
|------------|------------------------|-------------------|
| CA name    | `ibp3s0` (prefix `ib`) | `rocep3s0` (prefix `roce`) |
| Link layer | InfiniBand             | Ethernet |
| Base lid   | 65535 (unconfigured)   | 0 (not applicable — Ethernet doesn't use LIDs) |
| Rate       | 100 (EDR)              | 40 (default before cable negotiation) |

The `roce` prefix confirms the OS recognizes the card's RoCEv2 capabilities. Unlike InfiniBand, Ethernet does not require a Subnet Manager — the link becomes active as soon as a cable is connected and the interface is brought up.



## Understanding the Dual Interfaces

When a ConnectX-4 is configured for RoCEv2, the kernel presents two distinct views of the same physical port:

| Interface             | Subsystem                 | Name       | Used By |
|-----------------------|---------------------------|------------|---------|
| OS network device     | Linux netdev (`ip link`)  | `ens4np0`  | Standard networking: IP addressing, MTU, firewall, ping, tcpdump |
| Hardware Verbs device | RDMA/OFED (`ibv_devices`) | `rocep3s0` | RDMA operations: perftest, hardware counters, kernel-bypass traffic |

**OS network interface (`ens4np0`):** Treats the ConnectX-4 like any Ethernet NIC. Traffic goes through the kernel TCP/IP stack. Use this name for `ip addr`, `ip link`, `ping`, `iperf3`, `tcpdump`.

**Hardware Verbs device (`rocep3s0`):** Direct pipeline to the RDMA silicon. Traffic bypasses the kernel entirely. Use this name for `ib_write_bw`, `perfquery`, and RDMA-aware applications.

Verify the Verbs device is present:

```bash
ibv_devices
```

```text
device                 node GUID
------              ----------------
rocep3s0            ec0d9a030044c34c
```


## Inspecting OS Interfaces

    ip link show

```text
3: enp0s25: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP mode DEFAULT group default qlen 1000
    link/ether d8:9e:f3:12:78:91 brd ff:ff:ff:ff:ff:ff
4: ens4np0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN mode DEFAULT group default qlen 1000
    link/ether ec:0d:9a:44:c3:4c brd ff:ff:ff:ff:ff:ff
    altname enp3s0np0
```

| Interface | Type | Role |
|-----------|------|------|
| `enp0s25` | Ethernet (onboard) | Management network (SSH, internet). Standard 1 GbE. |
| `ens4np0` | Ethernet (ConnectX-4) | The RoCEv2 port. Currently DOWN, awaiting configuration. |



## Network Configuration and Jumbo Frames

For RoCEv2 to perform well, the MTU must be increased from the default 1500 to 9000 (Jumbo Frames). This reduces per-packet header overhead and allows the RDMA layer to use 4096-byte payloads efficiently.

On **Workstation 1:**

    sudo ip link set ens4np0 mtu 9000
    sudo ip addr add 10.10.0.1/24 dev ens4np0
    sudo ip link set ens4np0 up

On **Workstation 2:**

    sudo ip link set ens4np0 mtu 9000
    sudo ip addr add 10.10.0.2/24 dev ens4np0
    sudo ip link set ens4np0 up

Before connecting the cable, verify the interface state:

```bash
ip link show ens4np0
```

```text
3: ens4np0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 9000 qdisc mq state DOWN mode DEFAULT group default qlen 1000
    link/ether ec:0d:9a:44:c3:4c brd ff:ff:ff:ff:ff:ff
```

- **`UP`** (in angle brackets): The kernel has administratively enabled the interface.
- **`NO-CARRIER`**: No cable detected — the QSFP28 port is empty.
- **`mtu 9000`**: Jumbo Frames configured successfully.

This is the expected pre-cable state: software ready, waiting for physical connection.



## Physical Connectivity

Insert a QSFP28 DAC cable between the two workstations. Either cable works for RoCEv2 — at 100GbE both negotiate identically (see [Cable Selection](06_README_SETUP_CX_4.md#cable-selection)). This lab uses the MCP1600-C001E30N (Ethernet-labeled cable) for clarity.

Verify the link is up:

    ip addr show ens4np0

```text
3: ens4np0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 9000 qdisc mq state UP mode DEFAULT group default qlen 1000
    link/ether ec:0d:9a:44:c3:4c brd ff:ff:ff:ff:ff:ff
    altname enp3s0np0
```

- **`LOWER_UP`**: Physical layer (Layer 1) negotiated successfully — the cable is connected and both ends agreed on 100GbE.
- **`state UP`**: Link is operational and ready for traffic.

Test basic IP connectivity:

    ping 10.10.0.2



## Generating RoCEv2 Traffic

With the link up and IP addresses assigned, test hardware RDMA over Ethernet using `ib_write_bw`:

On **Workstation 2** (server):

    ib_write_bw -d rocep3s0

On **Workstation 1** (client):

    ib_write_bw -d rocep3s0 10.10.0.2 --report_gbits

Sample output:

```text
---------------------------------------------------------------------------------------
                    RDMA_Write BW Test
 Dual-port       : OFF          Device         : rocep3s0
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : ON
 TX depth        : 128
 CQ Moderation   : 1
 Mtu             : 4096[B]
 Link type       : Ethernet
 GID index       : 3
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0000 QPN 0x0106 PSN 0xda34b6 RKey 0x17feed VAddr 0x00761423c8a000
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:10:00:01
 remote address: LID 0000 QPN 0x0106 PSN 0x2a7a40 RKey 0x17feed VAddr 0x007eb7460dd000
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:10:00:02
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]
Conflicting CPU frequency values detected: 1200.000000 != 3491.633000. CPU Frequency is not max.
 65536      5000             84.42              84.42              0.161010
---------------------------------------------------------------------------------------
```

**Key fields:**

| Field          | Value              | Meaning |
|----------------|--------------------|---------|
| Transport type | IB                 | Using InfiniBand Verbs API (kernel bypass). |
| Link type      | Ethernet           | Physical transport is Ethernet (RoCEv2). |
| GID            | `::ffff:10.10.0.1` | IPv4 address mapped into IPv6 GID format. |
| LID            | 0000               | Not applicable — Ethernet doesn't use LIDs. |
| Mtu            | 4096[B]            | RDMA path MTU (maximum per-packet RDMA payload). |
| BW average     | 84.42 Gb/sec       | Achieved throughput. |

> **Note:** The CPU frequency warning (`Conflicting CPU frequency values detected`) indicates the governor is not set to `performance`. Setting it to `performance` on both nodes would likely push throughput closer to line rate (~93 Gb/s after protocol overhead).



## Capturing Hardware RoCEv2 Traffic

RoCEv2 encapsulates RDMA inside UDP/IP packets, but the hardware bypasses the kernel during transfer. This creates a capture challenge.

### Why Standard `tcpdump` Fails

**Attempt 1 — Capture on the Verbs device:**

```bash
sudo tcpdump -i rocep3s0
# Error: rocep3s0: No such device exists
```

`tcpdump` uses libpcap, which only hooks into kernel netdev interfaces. The Verbs device is invisible to the kernel networking stack.

**Attempt 2 — Capture on the OS interface:**

```bash
sudo tcpdump -i ens4np0 -w rocev2.pcap
```

This captures successfully, but only shows a brief TCP handshake (the control plane exchange on port 18515). Once the RDMA data transfer begins, the capture goes silent — the HCA is pulling data directly from user-space memory to the wire, bypassing the kernel entirely.

<img src="../pics/roce-initial.png" width="850"/>

The TCP packets visible are the initial connection management handshake where the nodes exchange Queue Pair Numbers, GIDs, and Packet Sequence Numbers. After this, all data moves through the hardware bypass path.

### Solution: Offloaded Traffic Sniffer (Docker)

To capture kernel-bypassed RoCEv2 packets, the NIC must be told to mirror traffic back to the OS. This requires a version of libpcap compiled with RDMA support (`--enable-rdma`). The simplest approach is the official Mellanox Docker container:

Install Docker (if not already present):

```bash
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
newgrp docker
```

Pull the RDMA-capable tcpdump image:

    docker pull mellanox/tcpdump-rdma

Run the container with hardware access:

```bash
docker run -it --net=host --privileged \
    -v /dev/infiniband:/dev/infiniband \
    -v /tmp/traces:/tmp/traces \
    mellanox/tcpdump-rdma bash
```

Inside the container, verify the Verbs devices are accessible:

    ibv_devices

Start the capture on the RoCEv2 device:

    tcpdump -i rocep3s0 -s 0 -w /tmp/traces/rocev2_capture.pcap

Generate RDMA traffic from another terminal (`ib_write_bw`), then stop the capture with Ctrl+C.

<img src="../pics/rocev2-capture.png" width="850"/>

The capture now shows the full RoCEv2 packet structure: Ethernet → IP → UDP (port 4791) → InfiniBand BTH → RDMA payload.
