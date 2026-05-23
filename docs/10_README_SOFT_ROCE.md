
# Software RoCE (Soft-RoCE)

Software RoCE (referred to in Linux as RXE or `rdma_rxe`) is a software implementation of the RoCEv2 protocol. It allows any standard Ethernet NIC to expose the same RDMA Verbs API that hardware RNICs provide, making it useful for development, testing, or running RDMA-dependent applications without specialized hardware.

<img src="../pics/soft-roce-2.png" width="450"/>

**Hardware RoCE Path (Right):** Traffic moves from the application through the hardware RoCE drivers directly to the RoCE HCA. This path bypasses the kernel networking stack entirely.

**Soft-RoCE Path (Center, red ovals):** The application's RDMA call is handled by the `rdma_rxe` kernel module, which implements all RoCE transport logic (Verbs, UDP/IP encapsulation) in software. The resulting packets are handed to the standard kernel network stack and transmitted through a regular Ethernet NIC.

| Feature           | Hardware RoCEv2 (ConnectX-4)                               | Software RoCE (rdma_rxe)                               |
| ----------------- | ---------------------------------------------------------- | ------------------------------------------------------ |
| Data Path         | NIC hardware directly accesses application memory (RDMA).  | Application → `rdma_rxe` → kernel network stack → NIC. |
| Kernel Bypass     | Yes                                                        | No (CPU and kernel process all traffic)                |
| Capture Interface | RDMA device (requires special tooling)                     | Standard Ethernet interface (native tcpdump)           |
| Performance       | Line-rate (84+ Gb/s at 100GbE)                             | CPU-bound (~110 MB/s on 1GbE)                          |

## Prerequisites

Soft-RoCE requires:
- A Linux kernel with the `rdma_rxe` module (mainline since 4.9)
- Any standard Ethernet NIC with IP connectivity between nodes
- RDMA user-space tools (`rdma-core`, `ibverbs-utils`, `perftest`)

This lab uses an Intel I210-AT PCIe x1 NIC (1GbE) on each workstation, connected via a standard Cat6 Ethernet cable. See [Hardware Setup](05_README_SETUP.md) for NIC placement details.

## Configuration

### Target Setup

| Machine | Interface | Soft-RoCE Device | IP Address |
| ------- | --------- | ---------------- | ---------- |
| rdma1   | enp7s0    | rxe0             | 10.20.0.1  |
| rdma2   | enp7s0    | rxe0             | 10.20.0.3  |

### Installing User-Space Tools

On both workstations:

    sudo apt update
    sudo apt install -y rdma-core ibverbs-utils perftest libibverbs-dev rdmacm-utils

### Loading the Kernel Module

Load `rdma_rxe` and make it persistent across reboots:

    sudo modprobe rdma_rxe
    echo "rdma_rxe" | sudo tee /etc/modules-load.d/rdma_rxe.conf

### Persistent IP Addressing (Netplan)

The `ip addr add` command is temporary. On Ubuntu, use Netplan to make addresses persist across reboots.

On **rdma1**, create `/etc/netplan/99-rdma.yaml`:

```yaml
network:
  version: 2
  ethernets:
    enp7s0:
      addresses:
        - 10.20.0.1/24
```

On **rdma2**, create `/etc/netplan/99-rdma.yaml`:

```yaml
network:
  version: 2
  ethernets:
    enp7s0:
      addresses:
        - 10.20.0.3/24
```

Apply on both (the `99-` prefix ensures these settings take priority over default Netplan files):

    sudo chmod 600 /etc/netplan/99-rdma.yaml
    sudo netplan apply

Verify connectivity:

    ping 10.20.0.3   # from rdma1

### Binding Soft-RoCE to the Interface

With IP connectivity confirmed, bind the RXE device to the physical interface.

On **rdma1:**

    sudo rdma link add rxe0 type rxe netdev enp7s0

On **rdma2:**

    sudo rdma link add rxe0 type rxe netdev enp7s0

### Making RXE Binding Persistent

The `rdma link add` command does not survive reboots. Create a systemd service on both workstations:

Create `/etc/systemd/system/soft-roce.service`:

```text
[Unit]
Description=Initialize Soft-RoCE Device
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/bin/rdma link add rxe0 type rxe netdev enp7s0
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Enable and start:

    sudo systemctl daemon-reload
    sudo systemctl enable soft-roce.service
    sudo systemctl start soft-roce.service

### Verification

Check that the RDMA subsystem sees the new device:

    rdma link show

```text
link rocep3s0/1 state ACTIVE physical_state LINK_UP
link rxe0/1 state ACTIVE physical_state LINK_UP netdev enp7s0
```

Two RDMA devices are visible:

- **rocep3s0** — The ConnectX-4 hardware RDMA device (configured for RoCEv2). All transport logic is offloaded to the NIC silicon.
- **rxe0** — The Soft-RoCE device bound to `enp7s0`. All transport logic runs on the CPU via the `rdma_rxe` kernel module.

Confirm with `ibv_devices`:

    ibv_devices

```text
device                 node GUID
------              ----------------
rocep3s0            ec0d9a030044c34c
rxe0                c66237fffe09dc78
```

## Capturing Soft-RoCE Traffic

Because Soft-RoCE processes all RDMA packets through the standard Linux network stack, capturing traffic requires only native `tcpdump` on the physical interface — no specialized containers or OFED-aware tools needed.

Filter for UDP port 4791 (the standard RoCEv2 port):

    sudo tcpdump -i enp7s0 udp port 4791 -w soft_roce_capture.pcap

## Running the Bandwidth Test

On **rdma2** (server):

    ib_write_bw -d rxe0

On **rdma1** (client):

    ib_write_bw -d rxe0 10.20.0.3

Sample output:

```text
---------------------------------------------------------------------------------------
                    RDMA_Write BW Test
 Dual-port       : OFF          Device         : rxe0
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : OFF
 TX depth        : 128
 CQ Moderation   : 1
 Mtu             : 1024[B]
 Link type       : Ethernet
 GID index       : 1
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0000 QPN 0x0013 PSN 0x3d508a RKey 0x0004a6 VAddr 0x007dc6b256a000
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:20:00:01
 remote address: LID 0000 QPN 0x0013 PSN 0xc25646 RKey 0x0004b2 VAddr 0x007ad3a2c4e000
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:20:00:03
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
Conflicting CPU frequency values detected: 1200.000000 != 3591.635000. CPU Frequency is not max.
 65536      5000             110.31             110.28             0.001764
---------------------------------------------------------------------------------------
```

Key differences from hardware RoCEv2:

| Field       | Soft-RoCE                     | Hardware RoCEv2 |
|-------------|-------------------------------|-----------------|
| ibv_wr* API | OFF (not supported by RXE)    | ON |
| Mtu         | 1024[B] (limited by 1GbE MTU) | 4096[B] |
| BW average  | 110.28 MB/sec (~0.88 Gb/s)    | 84.42 Gb/sec |

The ~110 MB/s result is close to the 1GbE wire limit (~125 MB/s). The gap is due to CPU processing overhead and the UDP/IP/Ethernet encapsulation headers consuming bandwidth on a 1 Gb/s link.

Stop the tcpdump capture (Ctrl+C). The resulting `.pcap` file contains every RDMA payload in standard format, ready for analysis in Wireshark.
