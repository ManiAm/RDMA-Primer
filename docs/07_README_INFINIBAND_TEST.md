# InfiniBand Link Testing

This document walks through the complete process of validating an InfiniBand connection between two workstations, from initial hardware detection through to bandwidth benchmarking and packet capture. Both workstations have a single ConnectX-4 adapter installed and provisioned (see [Lab Setup](05_README_SETUP.md) and [ConnectX-4 Provisioning](06_README_SETUP_CX_4.md)). The cards are configured with `LINK_TYPE_P1=IB`.

## InfiniBand Port States

Before running diagnostics, it is important to understand the state machine that every InfiniBand port follows. A port transitions through these states in sequence:

| Logical State | Physical State | Meaning |
|---------------|----------------|---------|
| Down          | Disabled       | Port is shut down, no cable or not activated by OS. |
| Down          | Polling        | Cable detected, port is sending training sequences looking for a peer. |
| Initializing  | LinkUp         | Electrical link established, waiting for a Subnet Manager to assign a LID. |
| Active        | LinkUp         | Fully operational — Subnet Manager has configured the port and data can flow. |

The physical state reflects the electrical/hardware condition of the link. The logical state reflects whether the InfiniBand management plane (Subnet Manager) has configured the port for traffic.

## Hardware Diagnostics (`ibstat`)

The `ibstat` command queries the Host Channel Adapter (HCA) and displays the physical and logical status of its ports.

    ibstat

Sample output (card installed, no cable connected):

```text
CA 'ibp3s0'
        CA type: MT4115
        Number of ports: 1
        Firmware version: 12.28.2040
        Hardware version: 0
        Node GUID: 0xec0d9a030044c158
        System image GUID: 0xec0d9a030044c158
        Port 1:
                State: Down
                Physical state: Disabled
                Rate: 10
                Base lid: 65535
                LMC: 0
                SM lid: 0
                Capability mask: 0x2651e848
                Port GUID: 0xec0d9a030044c158
                Link layer: InfiniBand
```

### Field Reference

**Global adapter fields:**

| Field | Description |
|-------|-------------|
| CA 'ibp3s0' | Channel Adapter name. Predictable naming: InfiniBand device on PCI bus 3, slot 0. This is the device name passed to RDMA tools. |
| CA type (MT4115) | Silicon identifier for ConnectX-4. |
| Number of ports | Physical port count (1 for this single-port VPI card). |
| Firmware version | Currently running firmware. |
| Node GUID | Globally unique identifier for the card on the InfiniBand fabric (analogous to a MAC address). |

**Port-specific fields:**

| Field | Description |
|-------|-------------|
| State | Logical port state (see state table above). |
| Physical state | Hardware/electrical state (see state table above). |
| Rate | Negotiated link speed in Gb/s. Defaults to 10 (SDR) when disconnected. |
| Base lid | Local Identifier — the InfiniBand routable address. `65535` (0xFFFF) means unconfigured (no Subnet Manager has assigned an address). |
| LMC | LID Mask Control — allows multiple LIDs per port for multi-path routing. `0` is standard. |
| SM lid | LID of the Subnet Manager controlling this fabric. `0` means no SM detected. |
| Link layer | Confirms InfiniBand protocol (vs Ethernet). |

The output above shows the expected state for a card that is powered on but has no cable attached and no Subnet Manager running.

## Physical Peer-to-Peer Connectivity

With both adapters verified, the next step is to establish a physical link. We connect the two nodes directly with the InfiniBand DAC cable (MCP1600-E001E30) — no switch required.

### Step 1: Cable Inserted on One Side (Polling)

When one end of the DAC cable is inserted into Workstation 1 (the other end is not yet connected), the hardware detects the transceiver and begins sending training sequences:

```text
Port 1:
        State: Down
        Physical state: Polling
        Rate: 10
        Base lid: 65535
```

**Polling** means the card acknowledges the cable and is actively sending electrical pulses searching for a peer. Because the other end is open, the logical state remains Down.

### Step 2: Both Sides Connected (LinkUp)

When the second end is plugged into Workstation 2, the two cards detect each other's training sequences and perform a hardware handshake to negotiate lane width and speed:

```text
Port 1:
        State: Initializing
        Physical state: LinkUp
        Rate: 100
        Base lid: 65535
```

- **Physical state: LinkUp** — The electrical handshake succeeded. QSFP28 connectors are properly seated on both ends.
- **Rate: 100** — The link negotiated EDR (100 Gb/s), the maximum speed supported by the ConnectX-4 and the MCP1600-E001E30 cable.
- **State: Initializing** — The wire is connected, but the port cannot pass data until a Subnet Manager assigns it a routing address (LID).

## Deep-Dive Link Diagnostics (`mlxlink`)

While `ibstat` shows how the Linux kernel sees the interface, `mlxlink` queries the firmware directly for Layer 1 electrical details, FEC status, and cable diagnostics:

    sudo mlxlink -d /dev/mst/mt4115_pciconf0

Sample output:

```text
Operational Info
----------------
State                              : Active
Physical state                     : LinkUp
Speed                              : IB-EDR
Width                              : 4x
FEC                                : No FEC
Loopback Mode                      : No Loopback
Auto Negotiation                   : ON

Supported Info
--------------
Enabled Link Speed                 : 0x0000003f (EDR,FDR,FDR10,QDR,DDR,SDR)
Supported Cable Speed              : 0x0000003f (EDR,FDR,FDR10,QDR,DDR,SDR)

Troubleshooting Info
--------------------
Status Opcode                      : 0
Group Opcode                       : N/A
Recommendation                     : No issue was observed
```

**Key fields:**

- **Speed (IB-EDR):** Confirms the active protocol and negotiated bandwidth (100 Gb/s).
- **Width (4x):** All four electrical lanes in the QSFP28 connector are active.
- **Supported Cable Speed:** The cable declares compliance for all InfiniBand speeds from SDR through EDR.
- **FEC (No FEC):** Over a short 1m DAC cable, signal integrity is high enough that Forward Error Correction is not needed. This means slightly lower latency.
- **Recommendation:** "No issue was observed" confirms a clean physical link.

If you experience bandwidth drops or suspect a cable failure, `mlxlink` is the first diagnostic to run. The Troubleshooting Info section will explicitly report signal integrity problems.

## Setting Up the Subnet Manager

Unlike Ethernet, where two hosts can communicate immediately after plugging in a cable, InfiniBand is a *managed fabric*. A Subnet Manager (SM) must be running to:

1. Discover the network topology.
2. Assign a Local Identifier (LID) to each port.
3. Compute and distribute routing tables.

Without a Subnet Manager, ports remain in the **Initializing** state indefinitely — the physical link is up, but no data can flow.

### Installing and Starting OpenSM

In a P2P setup, one workstation must run the Subnet Manager. Install OpenSM on Workstation 1:

    sudo apt install -y opensm

Start the service:

    sudo systemctl start opensm

OpenSM detects the local HCA, discovers the remote peer across the DAC cable, and assigns LIDs to both ports.

### Verifying Active State

Run `ibstat` on both workstations:

**Workstation 1:**

```text
Port 1:
        State: Active
        Physical state: LinkUp
        Rate: 100
        Base lid: 1
        LMC: 0
        SM lid: 1
```

**Workstation 2:**

```text
Port 1:
        State: Active
        Physical state: LinkUp
        Rate: 100
        Base lid: 2
        LMC: 0
        SM lid: 1
```

- **State: Active** — Both ports are fully operational.
- **Base lid: 1 / 2** — OpenSM assigned LID 1 to the local port and LID 2 to the remote peer.
- **SM lid: 1** — Both ports report that the Subnet Manager lives at LID 1 (Workstation 1).

## Hardware-Level Connectivity (`ibping`)

With the Subnet Manager running and both ports Active, verify end-to-end connectivity using `ibping`. Unlike ICMP ping (which uses TCP/IP), `ibping` operates at Layer 2 using InfiniBand Management Datagrams (MADs) sent directly to a LID.

On **Workstation 2**, start the server (listens for incoming pings):

    sudo ibping -S

On **Workstation 1**, ping the remote LID:

    sudo ibping -L 2

Expected output:

```text
Pong from rdma2.(none) (Lid 2): time 0.025 ms
Pong from rdma2.(none) (Lid 2): time 0.038 ms
Pong from rdma2.(none) (Lid 2): time 0.037 ms
```

A successful response confirms:

- The DAC cable is physically sound.
- The Subnet Manager has correctly mapped the routing path.
- The ConnectX-4 hardware can process management packets end-to-end.

## Fabric Discovery (`ibnodes`)

`ibnodes` scans the fabric via the Subnet Manager and lists all discovered nodes:

```bash
sudo ibnodes
```

```text
Ca      : 0xec0d9a030044c158 ports 1 "rdma2 ibp3s0"
Ca      : 0xec0d9a030044c34c ports 1 "rdma1 ibp3s0"
```

Both workstations appear as Channel Adapters (Ca). This confirms that Workstation 1 can see Workstation 2 across the DAC cable through the Subnet Manager's topology database.

> `ibnodes` requires OpenSM to be running. Without it, the command will fail or only show the local adapter.

## Bandwidth Benchmarking (`ib_send_bw`)

To measure the true hardware throughput of the InfiniBand link, use `ib_send_bw` from the `perftest` package. This tool pushes raw data through the ConnectX-4 via RDMA Send operations, bypassing the Linux kernel networking stack entirely.

### Control Plane vs Data Plane

`ib_send_bw` uses a client-server model with a subtle two-network design:

- **Control Plane (Ethernet/TCP):** Before RDMA traffic can flow, both cards must exchange hardware identifiers (LID, Queue Pair Number, Packet Sequence Number). This handshake happens over your standard Ethernet/IP network — which is why the command takes a hostname or IP address as an argument.

- **Data Plane (InfiniBand):** Once the handshake completes, all test data flows exclusively over the InfiniBand DAC cable at full speed.

### Running the Test

On **Workstation 2** (server):

    ib_send_bw -d ibp3s0

On **Workstation 1** (client):

    ib_send_bw rdma2.home -d ibp3s0 --report_gbits

Sample output:

```text
---------------------------------------------------------------------------------------
                    Send BW Test
 Dual-port       : OFF          Device         : ibp3s0
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : ON
 TX depth        : 128
 CQ Moderation   : 1
 Mtu             : 4096[B]
 Link type       : IB
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0x01 QPN 0x0107 PSN 0x62a2e6
 remote address: LID 0x02 QPN 0x0107 PSN 0x30fb32
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]
Conflicting CPU frequency values detected: 3492.253000 != 1200.000000. CPU Frequency is not max.
 65536      1000             84.02              84.02              0.160259
---------------------------------------------------------------------------------------
```

### Interpreting the Results

The link is rated for EDR (100 Gb/s raw) and the test achieved **84 Gb/s**. The gap is accounted for by:

- **64b/66b encoding overhead:** EDR uses 64b/66b line coding. For every 64 data bits, 2 framing bits are added. Theoretical max: 100 × (64/66) ≈ 97 Gb/s.

- **Protocol headers:** Each packet carries an InfiniBand Local Routing Header (LRH), Base Transport Header (BTH), and CRC — consuming several Gb/s.

- **CPU frequency governor:** The warning shows the CPU fluctuating between 1.2 GHz and 3.5 GHz. While RDMA bypasses the CPU for data transfer, the CPU still handles completion queue polling. Setting the governor to `performance` mode may improve results.

To maximize throughput, set the CPU governor to performance mode before testing:

    sudo cpupower frequency-set -g performance

## Hardware Health Auditing (`perfquery`)

`perfquery` reads the hardware error and performance counters directly from the ConnectX-4 silicon. While `ib_send_bw` tells you *how fast* the link is, `perfquery` tells you *how clean* the link is.

Even at full bandwidth, a degraded cable or poorly seated connector may cause the hardware to silently retransmit frames. `perfquery` exposes these hidden errors.

### The Sandwich Method

Best practice for validating a link:

1. **Clear counters** on both workstations before testing:

        sudo perfquery -C ibp3s0 -R

2. **Run the bandwidth test** (`ib_send_bw`).

3. **Read counters** immediately after:

        sudo perfquery -C ibp3s0

### Interpreting the Output

```text
# Port counters: Lid 1 port 1 (CapMask: 0x5A00)
PortSelect:......................1
CounterSelect:...................0x0000
SymbolErrorCounter:..............0
LinkErrorRecoveryCounter:........0
LinkDownedCounter:...............0
PortRcvErrors:...................0
PortRcvRemotePhysicalErrors:.....0
PortRcvSwitchRelayErrors:........0
PortXmitDiscards:................0
PortXmitConstraintErrors:........0
PortRcvConstraintErrors:.........0
CounterSelect2:..................0x00
LocalLinkIntegrityErrors:........0
ExcessiveBufferOverrunErrors:....0
QP1Dropped:......................0
VL15Dropped:.....................0
PortXmitData:....................16480432
PortRcvData:.....................10932
PortXmitPkts:....................16006
PortRcvPkts:.....................1506
PortXmitWait:....................239508
```

**Data counters** (expected to increase after a test):

| Counter                    | Meaning |
|----------------------------|---------|
| PortXmitData / PortRcvData | Total data words transmitted/received. |
| PortXmitPkts / PortRcvPkts | Total packets transmitted/received. |

**Error counters** (must remain at 0 for a healthy link):

| Counter                      | Indicates |
|------------------------------|-----------|
| SymbolErrorCounter           | Bit-level encoding errors on the wire. |
| LinkErrorRecoveryCounter     | Link retrained due to signal degradation. |
| LinkDownedCounter            | Link went down unexpectedly. |
| PortRcvErrors                | Malformed packets received. |
| LocalLinkIntegrityErrors     | Repeated local link errors. |
| ExcessiveBufferOverrunErrors | Receive buffer overflows. |

If any error counter is non-zero after a test, investigate the DAC cable seating, QSFP28 port cleanliness, or PCIe card seating.

## Capturing InfiniBand Traffic (`ibdump`)

Standard packet sniffers (Wireshark, tcpdump) hook into the Linux kernel networking stack. RDMA traffic bypasses the kernel entirely — the ConnectX-4 moves data directly between application memory and the wire. To capture this traffic, you must command the firmware to mirror packets at the silicon level.

### Installing `ibdump`

`ibdump` is not included in modern MFT packages or Ubuntu's `mstflint` apt package. It must be compiled from the `mstflint` source:

```bash
sudo apt install -y build-essential libibverbs-dev rdma-core ibverbs-utils
git clone https://github.com/Mellanox/mstflint.git
cd mstflint
git checkout v4.35.0-1
cd ibdump
make clean && make
```

### Running a Capture

Ensure MST is running (`sudo mst start`), then start the capture:

```bash
sudo ./ibdump \
    --ib-dev=ibp3s0 \
    --mst-dev=/dev/mst/mt4115_pciconf0 \
    -i 1 \
    -w /tmp/ibdump.pcap
```

| Flag                                 | Purpose |
|--------------------------------------|---------|
| `--ib-dev=ibp3s0`                    | Binds to the logical InfiniBand interface (for memory buffer allocation). |
| `--mst-dev=/dev/mst/mt4115_pciconf0` | Targets the specific PCIe device for firmware-level sniffer commands. |
| `-i 1`                               | Selects physical port 1 on the HCA. |
| `-w /tmp/ibdump.pcap`                | Output file (Wireshark-compatible). |

> For ConnectX-3 cards, add `--a0-mode` to use the legacy sniffer programming path. ConnectX-4 uses the newer ICMD path by default.

Expected output:

```text
Initiating resources ...
searching for IB devices in host
Port active_mtu=4096
 ------------------------------------------------
 Device                         : "ibp3s0"
 Physical port                  : 1
 Link layer                     : Infiniband
 Dump file                      : /tmp/ibdump.pcap
 Sniffer WQEs (max burst size)  : 4096
 ------------------------------------------------

Ready to capture (Press ^c to stop):
Captured:         2 packets,      580 bytes
```

### Analyzing the Capture

Open the `.pcap` file in Wireshark. When captured during idle (no active workload), the traffic consists entirely of Subnet Manager heartbeats:

<img src="../pics/infiniband-capture.png" width="800"/>

Key observations:

- **Permissive LID (65535 / 0xFFFF):** A broadcast-like address used exclusively for Subnet Management Packets (SMPs), allowing the SM to reach ports even before LIDs are fully assigned.
- **QP0:** Queue Pair 0 is hardware-reserved for the Subnet Management Interface. Application data never uses QP0.
- **SubnGet / SubnGetResp:** OpenSM polls the HCA for NodeInfo and PortInfo attributes. The HCA replies with the requested data.
- **10-second sweep interval:** The timestamps (0.000, 10.000, 20.000) show OpenSM's default fabric sweep period — it re-discovers the topology every 10 seconds to detect changes.
