
# Custom RDMA Application Development

This project is focused on writing custom RDMA (Remote Direct Memory Access) applications in C. The goal is to progressively build hands-on experience with kernel-bypass networking from discovering hardware and setting up queue pairs, to executing one-sided reads, writes, and atomic operations across a dedicated high-speed fabric.

## Documentation and Learning Path

Because the technologies involved in RDMA networking can be complex, this project includes a structured set of guides designed to build knowledge progressively. The following documents introduce the underlying concepts step by step, beginning with RDMA fundamentals and gradually moving through hands-on testing.

- [RDMA Fundamentals](docs/01_README_RDMA.md): Introduction to Remote Direct Memory Access - kernel bypass, zero-copy transfers, and the verbs programming model.
- [InfiniBand Architecture](docs/02_README_INFINIBAND.md): Deep dive into the InfiniBand protocol - subnet management, queue pairs, transport services, and hardware-driven reliability.
- [RoCE (RDMA over Converged Ethernet)](docs/03_README_ROCE.md): How RDMA is carried over standard Ethernet using RoCEv1 and RoCEv2, and the lossless fabric requirements this introduces.
- [Data Center Network Adapters](docs/04_README_NIC_ECOSYSTEM.md): The evolution of network offload — from standard NICs and SmartNICs through HCAs and DPUs to the AI-era SuperNICs that implement MRC and multipath transport in hardware.

## Testbed Overview

We have built a dedicated testbed using two identical workstations. Each workstation is equipped with:

- **Two Mellanox CX-4 VPI adapters**: each capable of 100 Gb/s EDR InfiniBand or 100 GbE via a single QSFP28 port.
- **One standard Ethernet NIC**: used for software-based RDMA experimentation.

The NICs on both workstations are connected **point-to-point** in matched pairs:

| Workstation A | Workstation B | Cable                           | Purpose                             |
|---------------|---------------|---------------------------------|-------------------------------------|
| CX-4 #1       | CX-4 #1       | QSFP28 DAC (MCP1600-E001E30)    | InfiniBand traffic (native RDMA)    |
| CX-4 #2       | CX-4 #2       | QSFP28 DAC (MCP1600-C001E30N)   | RoCEv2 traffic (RDMA over Ethernet) |
| Ethernet NIC  | Ethernet NIC  | Cat6 Ethernet                   | Soft-RoCE (software-emulated RDMA)  |

This symmetric topology allows us to benchmark and compare all three RDMA transport paths on the same physical testbed, under controlled point-to-point conditions with no switching fabric in the path.

- [Lab Setup](docs/05_README_SETUP.md): Hardware specifications, OS installation, and OFED driver deployment for both workstations.
- [ConnectX-4 Configuration](docs/06_README_SETUP_CX_4.md): Detailed guide to the Mellanox MCX455A-ECAT - firmware configuration, VPI mode switching, and port provisioning.

The VPI (Virtual Protocol Interconnect) designation means each ConnectX-4 port can be configured at the firmware level to operate in either InfiniBand mode or Ethernet mode. This is a per-port setting burned into the card's non-volatile memory. Because each link is point-to-point, both ends of a given pair **must** be configured to the same protocol: an InfiniBand port cannot negotiate a link with an Ethernet port. In our testbed, CX-4 #1 on both workstations is set to InfiniBand mode for native RDMA, while CX-4 #2 on both workstations is set to Ethernet mode for RoCEv2.

## Testing the Testbed

Once the testbed is up and running, and before writing our own custom RDMA applications, it is useful to perform a series of connectivity and performance tests across each link. These exercises help verify that the hardware, drivers, and protocols are functioning correctly, and build practical familiarity with the tools and workflows used throughout the rest of the project.

- [InfiniBand Testing](docs/07_README_INFINIBAND_TEST.md): Verifying HCA state with `ibstat`, running diagnostics, and validating point-to-point InfiniBand connectivity.
- [IP over InfiniBand](docs/08_README_IP_Over_INFINIBAND.md): Configuring IPoIB interfaces and understanding how the Linux networking stack exposes InfiniBand ports.
- [RoCEv2 Testing](docs/09_README_ROCEv2_TEST.md): Switching CX-4 to Ethernet mode, configuring RoCEv2, and running RDMA traffic over 100 GbE.
- [Soft-RoCE](docs/10_README_SOFT_ROCE.md): Setting up software-emulated RoCEv2 (RXE) on a standard Ethernet NIC for development and testing without RDMA hardware.

## Language Choice

RDMA applications interact with the network hardware through `libibverbs`, a C library that exposes the InfiniBand Verbs API. This API gives user-space programs direct access to the NIC's queues and memory registration hardware, bypassing the kernel entirely. Because the programming model (pinning memory, posting work requests, polling completion queues) is designed around explicit, low-level control, the choice of language has a direct impact on whether you benefit from RDMA's performance characteristics.

**C** is the native language of `libibverbs` and the only way to fully exploit zero-copy data paths and microsecond-level latencies. It allows direct memory pinning, avoids garbage collection pauses, and maps naturally onto the hardware's asynchronous queue model. All programs in this project are written in C.

**C++** is equally viable. It links against the same `libibverbs` C API and adds no runtime overhead, while offering conveniences like RAII for resource cleanup.

**Rust** has mature bindings (`rdma-sys`, `rust-ibverbs`) and matches C in performance. It is a strong alternative if memory safety guarantees are a priority.

**Python** bindings exist (`pyverbs`), but Python is unsuitable for the data plane. The Global Interpreter Lock (GIL) and managed memory model introduce millisecond-scale overhead that negates the microsecond latencies RDMA is designed to deliver. Python is acceptable only for control-plane tasks such as connection setup or orchestration.

## Prerequisites and Compilation

RDMA user-space programming relies on two libraries, both provided by the `rdma-core` package in Linux:

- **libibverbs**: the core Verbs API. This library is used to register memory regions, create queue pairs, post send/receive work requests, and poll completion queues. It is the interface through which your application communicates directly with the NIC hardware. All programs in this project link against `libibverbs`.

- **librdmacm** (RDMA Communication Manager): a connection management library that handles the initial setup between two RDMA endpoints. It provides a socket-like workflow (bind, listen, connect, accept) to exchange the addressing information that both sides need before data transfer can begin. The programs in this project use raw TCP sockets for connection setup instead of `librdmacm`, but installing it is recommended since most production RDMA applications depend on it.

Install the development headers on both machines:

    sudo apt-get update
    sudo apt-get install libibverbs-dev librdmacm-dev rdma-core

When compiling, link against `libibverbs` using `-l ibverbs`. For development and debugging, compile with debug symbols (`-g`) and no optimization (`-O0`) so that tools like `gdb` can map execution back to source lines:

    gcc -Wall -g -O0 -o my_rdma_app my_rdma_app.c -libverbs

For release builds, enable optimizations:

    gcc -Wall -O3 -o my_rdma_app my_rdma_app.c -libverbs

## Programs

The programs below are designed to be followed in order. Each one isolates a single layer of the RDMA programming model, starting from hardware discovery and building toward actual data transfer. Programs that create a Queue Pair use the Reliable Connection (RC) transport unless otherwise noted. RDMA operations fall into three categories.

- **Two-sided operations** (Send and Receive) require both endpoints to participate: the receiver must pre-post a buffer before the sender transmits.
- **One-sided operations** (RDMA Read and RDMA Write) allow one machine to directly access the other's memory without any involvement from the remote CPU. The remote application is entirely unaware that its memory was read or written.
- **Atomic operations** extend one-sided access with hardware-guaranteed atomicity for tasks like distributed locking.

### Program 1: Device Discovery

Before you can allocate memory or send data, your program must find the local ConnectX-4 card, open a direct user-space channel to it (the Context), and verify the physical link is active. This is the equivalent of confirming the hardware is present and the cable is connected.

File: [code/step1_discovery.c](code/step1_discovery.c)

### Program 2: Resource Allocation (PD, MR, CQ, QP)

Every RDMA program must allocate four core objects before any data can move:

- **Protection Domain (PD)**: an isolation boundary that ensures only authorized queue pairs can access specific memory regions.
- **Memory Region (MR)**: a buffer of application memory explicitly registered with the NIC. Registration pins the memory in RAM and provides the NIC with the virtual-to-physical address translation it needs for DMA.
- **Completion Queue (CQ)**: a hardware-managed queue where the NIC places notifications when it finishes processing a work request.
- **Queue Pair (QP)**: the actual communication channel, consisting of a Send Queue and a Receive Queue. Work requests are posted to these queues, and the hardware executes them asynchronously.

This program allocates all four objects and tears them down in the correct reverse order.

File: [code/step2_setup.c](code/step2_setup.c)

### Program 3: QP State Machine and Out-of-Band Exchange

A newly created QP is in the `RESET` state and cannot send or receive data. To activate a connection, both machines must exchange their QP coordinates (QP number, LID) - typically over a standard TCP socket - and then transition their QPs through the state machine: `INIT` → `RTR` (Ready to Receive) → `RTS` (Ready to Send).

This program demonstrates the three state transition functions. To keep the program self-contained and compilable without a TCP server/client, the remote machine's data is simulated with hardcoded values.

File: [code/step3_states.c](code/step3_states.c)

### Program 4: Two-Sided Operations (Send and Receive)

A two-sided transfer requires cooperation from both endpoints. The receiver must post a buffer to its Receive Queue *before* the sender posts the Send work request. If no receive buffer is waiting when the data arrives, the connection will fault with a Receiver Not Ready (RNR) error.

This is the fundamental contract of two-sided RDMA: the sender pushes data, but the receiver decides *where* in its memory that data lands by pre-posting a buffer. This model is conceptually similar to traditional socket programming (one side calls `send()`, the other calls `recv()`), but with a critical difference: in RDMA, the receive buffer must be posted to hardware *in advance*, not at the moment the data arrives.

This is a fully self-contained, runnable program that performs an end-to-end two-sided data transfer between two machines. It handles device discovery, resource allocation (PD, MR, CQ, QP), QP state transitions (`INIT` → `RTR` → `RTS`), out-of-band connection exchange over TCP, and the actual Send/Receive transfer - all in a single binary. Run it with no arguments to start the receiver, or pass the server's IP address to start the sender.

    Receiver (server):  ./step4_two_sided
    Sender (client):  ./step4_two_sided <server_ip>

> The receiver must be started first because it opens a TCP listening socket and waits for the sender to connect. If the sender runs first, the TCP `connect()` will fail with "Connection refused" since no one is listening yet.

File: [code/step4_two_sided.c](code/step4_two_sided.c)

### Program 5: One-Sided Operations (RDMA Read and Write)

Unlike two-sided Send/Receive, one-sided operations do not require any participation from the remote endpoint. The initiating machine's NIC reads from or writes to the remote machine's registered memory directly. The remote CPU is never interrupted and never aware the transfer occurred.

- **RDMA Write**: pushes data from local memory into the remote machine's memory region.
- **RDMA Read**: pulls data from the remote machine's memory region into local memory.

Both operations require the remote machine's memory address and remote key (`rkey`), which are exchanged during the out-of-band TCP setup alongside the QP connection data.

This is a fully self-contained, runnable program. The target exposes its memory with a pre-filled buffer, and the initiator first overwrites it via RDMA Write, then reads it back via RDMA Read - demonstrating both directions of one-sided access. TCP barriers synchronize the two sides so the target can observe its buffer being silently modified.

    Target (start first):  ./step5_one_sided
    Initiator:             ./step5_one_sided <target_ip>

File: [code/step5_one_sided.c](code/step5_one_sided.c)

### Program 6: Atomic Operations (Compare-and-Swap)

Atomic operations extend one-sided access with hardware-guaranteed atomicity. The NIC executes a `read-modify-write` cycle on a remote 64-bit value in a single, indivisible operation. No lock is needed, and the remote CPU is not involved.

- **Compare-and-Swap**: reads a remote 64-bit value; if it matches an expected value, replaces it with a new value. The original value is returned to the caller regardless of whether the swap occurred.
- **Fetch-and-Add**: atomically adds a value to a remote 64-bit integer and returns the original.

These primitives are the building blocks for distributed locks, reference counters, and lock-free data structures used in systems like `HERD`, `FaRM`, and `Pilaf`.

This is a fully self-contained, runnable program. The target initializes a 64-bit counter to 1000. The initiator then performs three operations in sequence: a Fetch-and-Add (+10), a Compare-and-Swap that succeeds (1010 → 9999), and a Compare-and-Swap that intentionally fails (expects 1010 but finds 9999) - demonstrating both the success and failure semantics of CAS.

    Target (start first):  ./step6_atomics
    Initiator:             ./step6_atomics <target_ip>

File: [code/step6_atomics.c](code/step6_atomics.c)

### Program 7: Unreliable Datagram (UD) Transport

All previous programs use the RC (Reliable Connection) transport, which provides in-order, guaranteed delivery with hardware retransmissions (analogous to TCP). InfiniBand also supports two other transport services:

- **UC (Unreliable Connection)**: a connected transport like RC, but without acknowledgments or retransmissions. Useful when the application handles its own reliability and wants lower overhead.
- **UD (Unreliable Datagram)**: a connectionless transport analogous to UDP. A single QP can communicate with any number of remote QPs, but because there is no connection state, every Send must include an **Address Handle (AH)** that specifies the destination. UD only supports two-sided operations (Send/Receive). RDMA Read, Write, and atomics are not available.

This program demonstrates how to create a UD queue pair, build an Address Handle, and attach it to a Send work request. Because UD has no connection state, the Address Handle serves as a per-message routing label. It encodes the destination's LID, service level, and port number so the NIC knows where to deliver each individual datagram. To keep the program self-contained and compilable without a live peer, remote addressing data (destination LID, QP number, Q_Key) is simulated with hardcoded values.

File: [code/step7_transports.c](code/step7_transports.c)
