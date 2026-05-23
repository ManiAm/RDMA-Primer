# IP over InfiniBand (IPoIB)

This document covers how to run standard TCP/IP applications over the InfiniBand fabric using IPoIB. While native RDMA applications use the Verbs API to bypass the kernel entirely, legacy applications (SSH, rsync, iperf3, NFS) only understand TCP/IP sockets. IPoIB bridges this gap by providing standard IP interfaces over InfiniBand hardware.

> Both workstations have an active InfiniBand link (State: Active) with a Subnet Manager running (see [InfiniBand Link Testing](07_README_INFINIBAND_TEST.md)).

## What IPoIB Does

IPoIB (IP over InfiniBand) is a kernel protocol driver (`ib_ipoib`) that encapsulates standard TCP/IP packets inside InfiniBand transport frames. It creates virtual network interfaces that look like standard Ethernet NICs to the operating system — you can assign IPv4 addresses, apply firewall rules, and bind sockets to them.

IPoIB routes traffic through the Linux kernel's TCP/IP stack. This means it does *not* achieve kernel bypass and will consume CPU cycles for every packet. Native RDMA applications (using the Verbs API) bypass the kernel entirely and achieve full line-rate with near-zero CPU usage. IPoIB exists for compatibility with applications that cannot be rewritten to use RDMA Verbs.

### How IPoIB Works

1. **Application sends a packet:** A standard program (e.g., `ping 10.0.0.2`) generates an IP packet and hands it to the kernel's TCP/IP stack.
2. **Routing:** The kernel looks at its routing table and forwards the packet to the IPoIB interface.
3. **Address resolution:** IPoIB uses ARP over the InfiniBand fabric. Instead of resolving to a 6-byte MAC address, it resolves the destination IP to a 20-byte InfiniBand hardware address (incorporating the port's GUID) and queries the Subnet Manager for routing parameters.
4. **Encapsulation:** The `ib_ipoib` driver wraps the IP payload inside an InfiniBand transport frame (adding a 4-byte IPoIB header).
5. **Transmission:** The HCA sends the InfiniBand frame across the physical cable.

### Architecture

<img src="../pics/ipoib.jpg" width="400"/>

The diagram shows two data paths:

- **Control Path:** During initialization, IPoIB uses the generic IB Core layer for fabric management — joining multicast groups, broadcasting ARP requests, and querying the Subnet Manager.

- **Fast Path:** For data transfer, the IPoIB driver bypasses the generic IB Core and pushes payloads directly into the hardware driver (`mlx5_core`), reducing per-packet latency.

## Configuring IPoIB

### Loading the Module

The `ib_ipoib` kernel module may not be loaded by default. Load it on both workstations:

    sudo modprobe ib_ipoib

### Identifying the Interface

Once loaded, the kernel creates an IPoIB interface for each InfiniBand port. Check with `ip link show`:

```text
3: ibs2: <BROADCAST,MULTICAST> mtu 4092 qdisc noop state DOWN mode DEFAULT group default qlen 256
    link/infiniband 00:00:00:67:fe:80:00:00:00:00:00:00:50:6b:4b:03:00:ee:ae:06 brd 00:ff:ff:ff:ff:12:40:1b:ff:ff:00:00:00:00:00:00:ff:ff:ff:ff
    altname ibp3s0
```

Key observations:

| Field | Value | Meaning |
|-------|-------|---------|
| Protocol | `link/infiniband` | Not Ethernet — this is an InfiniBand interface. |
| Hardware address | 20 bytes | The InfiniBand hardware address (incorporates the port GUID). |
| MTU | 4092 | Standard InfiniBand payload (4096) minus the 4-byte IPoIB header. |
| State | DOWN | The interface must be manually configured and brought up. |
| altname | ibp3s0 | Predictable name (same as `ibstat` device name). |

### Assigning IP Addresses

Create a private subnet for the InfiniBand link:

On **Workstation 1:**

    sudo ip addr add 10.0.0.1/24 dev ibs2
    sudo ip link set ibs2 up

On **Workstation 2:**

    sudo ip addr add 10.0.0.2/24 dev ibs2
    sudo ip link set ibs2 up

### Verifying Connectivity

    ping 10.0.0.2

If OpenSM is running and the physical link is active, the `ib_ipoib` driver encapsulates the ICMP packets into InfiniBand frames and you should see replies immediately.

## Performance Characteristics

IPoIB is constrained by the Linux kernel's packet processing pipeline. For every packet, the CPU must handle hardware interrupts, allocate memory buffers (`sk_buff`), and perform memory copies between kernel space and user space.

| Method | Throughput | CPU Usage | Kernel Involved? |
|--------|-----------|-----------|-----------------|
| Native RDMA (`ib_send_bw`) | ~84-97 Gb/s | Near zero | No (kernel bypass) |
| IPoIB (`iperf3`) | ~15-25 Gb/s (untuned) | Single core at 100% | Yes |

The bottleneck is not the hardware — the ConnectX-4 can push 100 Gb/s. The bottleneck is the CPU processing packets through the kernel's TCP/IP stack. A single CPU core saturates long before the link does.

## Capturing IPoIB Traffic

Because IPoIB routes traffic through the kernel networking stack, standard capture tools work:

    sudo tcpdump -i ibs2 -w ipoib_capture.pcap

<img src="../pics/ipoinb.png" width="800"/>

### What You See in Wireshark

- **Linux Cooked Header (SLL):** Because the interface is not standard Ethernet, libpcap uses a synthetic "cooked" header. Inside it, Wireshark identifies `Link-layer address type: InfiniBand (32)`.

- **No InfiniBand headers (LRH, BTH):** By the time `tcpdump` intercepts the packet inside the kernel, the HCA has already stripped the native InfiniBand encapsulation. You only see the IP payload.

- **20-byte ARP addresses:** ARP replies contain the full InfiniBand hardware address (e.g., `80000208fe80000000000000f452140300280931`) instead of a 6-byte MAC address.

### Limitation: Native RDMA Is Invisible

`tcpdump` hooks into the kernel networking stack. Native RDMA traffic (Verbs API) bypasses the kernel entirely — the HCA moves data directly from user-space memory to the wire. This traffic is invisible to `tcpdump`. Capturing native RDMA requires firmware-level mirroring with `ibdump` (see [InfiniBand Link Testing](07_README_INFINIBAND_TEST.md)).

## eIPoIB (Ethernet over IPoIB)

Standard IPoIB interfaces are not true Layer 2 Ethernet devices — they use 20-byte InfiniBand addresses instead of 6-byte MAC addresses. This means they cannot be attached to a standard Linux bridge (Open vSwitch) or presented to virtual machines that expect Ethernet NICs.

**eIPoIB** solves this by creating a virtual Ethernet interface on top of the IPoIB interface:

<img src="../pics/eIPOIB.png" width="500"/>

The data flow through eIPoIB:

1. **VM/Container sends an Ethernet frame** with standard 6-byte MAC addresses.
2. **eIPoIB shim intercepts** the frame, strips the Ethernet header, and extracts the raw IP payload.
3. **IPoIB driver** attaches a 4-byte IPoIB header and passes it to the HCA.
4. **HCA** encapsulates everything in native InfiniBand headers (20-byte addresses, LID routing, CRC) and transmits.

On the receiving side, the process reverses: IB headers are stripped, IPoIB header is removed, a synthetic Ethernet header is added, and the frame is delivered to the VM. This allows InfiniBand to be used transparently in bridged VM/container networking (KVM, Docker, Kubernetes) without any guest OS awareness.

## Enhanced IPoIB

Standard IPoIB processes traffic in a single kernel queue, causing a single CPU core to bottleneck at high packet rates. Enhanced IPoIB (enabled by default on ConnectX-4 and newer) offloads to the hardware driver (`mlx5_core`) with modern multi-queue acceleration:

- **RSS (Receive Side Scaling):** Distributes incoming traffic across multiple CPU cores.
- **TSS (Transmit Side Scaling):** Distributes outgoing traffic across multiple transmit queues.
- **Interrupt Moderation:** Batches hardware interrupts to prevent CPU exhaustion.

Enhanced IPoIB only supports Datagram mode (MTU limited to the physical InfiniBand link MTU, typically 4092 bytes). Legacy Connected Mode allowed MTUs up to 65,520 bytes but could not scale across multiple CPU cores. With RSS/TSS, the per-core efficiency far exceeds what a large MTU on a single core could achieve.
