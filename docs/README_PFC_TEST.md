


### Enhanced Transmission Selection (ETS)

ETS, defined by IEEE 802.1Qaz, governs how the switch schedules packets from multiple egress queues onto the wire. In a RoCE deployment, ETS ensures that congestion feedback is never starved by data traffic:

**Strict Priority for CNP (TC6):** The CNP queue is always serviced first. Whenever a CNP packet is waiting in TC6, it is dequeued and transmitted before any data or management packets, regardless of how full the other queues are. This guarantees that the DCQCN feedback loop has the lowest possible latency.

**Weighted Round-Robin (WRR) for Data and Management:** The remaining bandwidth is shared between RoCE data (TC3) and TCP/management (TC0) using configurable weights. A typical allocation is 80% to TC3 and 20% to TC0, ensuring that RoCE traffic receives the majority of the bandwidth while management and control-plane traffic still receives a guaranteed share and is not starved.

The combination of strict-priority CNP and WRR data ensures that the congestion control loop operates with minimal delay, reducing the likelihood that PFC must intervene as a backstop.



### MTU and Jumbo Frames

Standard Ethernet frames carry a maximum payload of 1500 bytes. For RoCE workloads, this default MTU is highly inefficient: the fixed overhead of Ethernet, IP, UDP, and BTH headers represents a significant fraction of each small frame, reducing the effective throughput.

RoCE deployments universally configure jumbo frames with an MTU of 9216 bytes. Jumbo frames reduce per-byte overhead, increase throughput, and reduce the number of packets per second that the NIC and switch must process. All interfaces in the fabric path — NICs, ToR switches, spine switches — must be configured with a consistent jumbo MTU. A single interface with a smaller MTU will silently fragment or drop oversized frames, causing RDMA failures.



### Incast: The Worst-Case Traffic Pattern

Incast occurs when many sources simultaneously transmit to a single destination port — for example, during an all-reduce collective operation where 16 GPUs send their gradient updates to a single aggregator. This many-to-one pattern creates extreme congestion at the destination switch port.

In a 16:1 incast scenario at 800G, the destination port receives 16× its line rate for a brief burst. The switch must absorb this burst entirely in its buffer (via PFC headroom and shared pool) without dropping any lossless packets. Incast is the stress test that validates whether the headroom formula, buffer pool sizing, and ECN thresholds are correctly tuned.

During incast, the expected behavior on a correctly configured lossless fabric is: PFC pause frames fire from the congested egress port toward all 16 ingress ports, halting their transmission until the buffer drains; ECN marks packets in the K_min-to-K_max range, triggering DCQCN rate reduction on all senders; and zero packets are dropped on the lossless queue throughout the event.




## RoCE Deployment Modes

Not every deployment requires the full complexity of a lossless fabric. Depending on the NIC capabilities and operational requirements, RoCE can be deployed in one of three modes:


### Lossless Mode

The standard deployment model, and the only option for NICs that use Go-Back-N retransmission without Selective Retransmission support. Lossless mode activates all three building blocks:

- **PFC** is enabled on the RoCE traffic class (TC3) to guarantee zero packet drops.
- **ECN/WRED** marking is enabled to proactively signal congestion and trigger DCQCN rate reduction before PFC is needed.
- **Lossless buffer pools** are provisioned with full headroom reservations.

This mode provides the highest reliability and is required for GPU training workloads where any packet loss triggers costly GBN retransmissions.


### Semi-Lossless Mode

A hybrid approach that reduces buffer headroom consumption while still providing PFC protection. Semi-lossless mode:

- Enables **PFC** with reduced headroom (smaller xoff/xon thresholds than full lossless mode).
- Configures **WRED** with a non-zero drop probability (P_max < 100%) for deep queues, accepting occasional drops under extreme congestion.
- Trades a small amount of loss tolerance for improved buffer efficiency, freeing shared pool capacity for other ports.

Semi-lossless mode is suitable for deployments where occasional packet loss is acceptable (e.g., storage workloads with upper-layer retransmission) but zero loss under normal conditions is still desired.


### Lossy / Resilient RoCE

Lossy mode eliminates PFC entirely and relies solely on ECN/DCQCN for congestion management. This mode is designed for NICs that support Selective Retransmission (SRS) or Improved RDMA over lossy networks (IRN):

- **PFC is disabled.** No pause frames are generated under any condition.
- **ECN/WRED** marking remains active, providing the same DCQCN rate-reduction feedback as lossless mode.
- **No lossless headroom** is reserved, freeing all buffer capacity for the shared pool.
- Under severe congestion, packets are simply dropped rather than paused. The NIC's SRS engine retransmits only the lost packets (not the entire GBN window), minimizing the throughput penalty.

Lossy mode eliminates the operational risks of PFC — storms, deadlocks, and HOL blocking — at the cost of requiring SRS-capable NICs. It is an emerging deployment model as NIC vendors increasingly adopt selective retransmission in their firmware.


### Packet Trimming: Toward a PFC-Free Future

Packet trimming is a forward-looking mechanism that aims to replace PFC entirely. When a switch buffer overflows, instead of dropping the packet or pausing the sender, the switch trims the packet to a minimal header (stripping the payload) and forwards this "trimmed notification" to the receiver. The receiver detects the trim and requests selective retransmission of just the lost payload.

Packet trimming requires coordinated support in both the switch ASIC and the NIC firmware. It is currently in the standards track through the Ultra Ethernet Consortium (UEC) and is expected to appear in future NIC and switch generations. When widely deployed, it would eliminate the need for PFC headroom, PFC watchdogs, and the entire class of PFC storm and deadlock issues.




## DCB Negotiation: LLDP and DCBX

Configuring the QoS classification, PFC priorities, and ETS scheduling on every switch is only half the problem. The NICs connected to those switches must also be configured with matching parameters — the same DSCP-to-priority mapping, PFC on the same priority, and the same traffic class for RoCE. If the switch expects RoCE on priority 3 but the NIC sends it on priority 0, the traffic will land in a lossy queue and the lossless fabric is useless.

DCBX (Data Center Bridging Capability Exchange) solves this by automating the negotiation. DCBX is a protocol extension carried over LLDP (Link Layer Discovery Protocol) that allows a switch and a directly connected NIC to exchange their DCB configurations:

**Application TLV:** The switch advertises that RoCEv2 (UDP port 4791) should use priority 3. The NIC reads this TLV and auto-configures its DSCP and PFC priority to match.

**PFC TLV:** The switch advertises which priorities have PFC enabled. The NIC enables PFC on those priorities.

**ETS TLV:** The switch advertises bandwidth allocations per traffic class. The NIC applies matching scheduling.

This auto-negotiation is especially important in large deployments where manually configuring hundreds or thousands of NICs is impractical and error-prone. When DCBX is disabled or not supported, operators must manually configure both the switch and the NIC, and any mismatch will silently degrade RDMA performance.




## Interactions with Other Fabric Technologies

RoCE does not operate in isolation. In modern data center fabrics, it interacts with several other technologies that affect its behavior and performance.

### Adaptive Routing

Adaptive routing allows the switch to dynamically reroute individual flows from a congested path to a less-congested equal-cost path, reducing hotspots in the fabric. In RoCE deployments, adaptive routing and the lossless fabric are deeply interdependent:

- Adaptive routing requires the buffer pools to be in dynamic mode (not static), because the alpha-based dynamic thresholds are what allow buffer to shift between ports as flows are rerouted.
- PFC acts as a safety net during adaptive routing transitions: when a flow is rerouted, there is a brief period where packets may arrive out of order or experience transient congestion on the new path. PFC prevents any drops during this transition window.
- The recommended configuration sequence is: first enable RoCE lossless mode (which sets up dynamic buffers, PFC, and ECN), then enable adaptive routing on top of it.

### VXLAN Overlays

When RoCE traffic traverses a VXLAN overlay network, the original RoCEv2 packet is encapsulated inside an outer UDP/IP header. The DSCP field in the inner IP header (which carries the RoCE QoS classification) is not visible to switches that route based on the outer header.

To preserve lossless treatment across VXLAN boundaries, the switch must perform DSCP-to-PCP rewrite: it copies the DSCP-derived priority from the inner header into the PCP/CoS bits of the outer VLAN tag, ensuring that downstream switches classify the VXLAN-encapsulated RoCE traffic into the correct lossless queue. The inner DSCP field is not modified, so the original classification is preserved for the destination NIC.

### Warm Reboot and PFC Continuity

In production AI clusters, switch maintenance must not disrupt ongoing GPU training jobs. During a warm reboot, the switch's control plane (SONiC, routing daemons) restarts while the ASIC forwarding plane continues operating:

- **PFC continues in hardware.** Because PFC pause frame generation and processing are implemented entirely in the switch ASIC silicon, they continue to function during the control plane restart. If congestion occurs during the reboot window, PFC pause frames are still generated and honored, preventing packet drops.
- **Zero traffic disruption.** The ASIC's forwarding tables, buffer configuration, and QoS maps remain programmed throughout the reboot. RoCEv2 traffic continues to flow on TC3 with zero drops.
- **Post-reboot reconciliation.** When the control plane finishes restarting, the RoCE orchestration daemon reconciles its configuration database against the existing ASIC state. If the ASIC programming already matches the saved configuration (which it should, since the ASIC was never reset), no reprogramming occurs, avoiding transient flaps.

This behavior is critical for production deployments where a single switch reboot should not cause NCCL timeouts or MPI job failures across the GPU cluster.




----------------------------


Selective Ack

As we established earlier, Ethernet is inherently lossy. When data centers tried to run RDMA over Ethernet (RoCE) using the legacy Go-Back-N mechanism, they realized that even a tiny amount of congestion-induced packet loss would trigger massive retransmission storms, destroying network throughput.

To prevent this, engineers had to use Priority Flow Control (PFC) to force Ethernet to be lossless. But PFC is notoriously difficult to manage and prone to catastrophic network deadlocks.

Network engineers wanted a way to run RoCE on standard, lossy Ethernet without the nightmare of configuring PFC. To do this, they needed the NIC to stop overreacting to a single dropped packet.

The Solution: NIC vendors developed Selective Retransmission (SRS) (also sometimes called "Resilient RoCE"). With SRS, if Packet #3 is dropped, but #4 and #5 arrive, the receiving NIC does not throw #4 and #5 away. It temporarily buffers them and asks the sender only for Packet #3.

The Result: Ethernet can now occasionally drop packets without destroying RDMA performance.

-------


More recent NIC designs introduce Selective Retransmission (SRS), which retransmits only the lost packet rather than the entire window. SRS dramatically reduces the penalty of packet loss, opening the door to "lossy" or "resilient" RoCE deployments. However, SRS requires NIC firmware support and is not universally available, so the lossless fabric remains the standard deployment model.


--------

Crucially, RoCE does not require InfiniBand switches. It allows you to run RDMA traffic entirely over standard Ethernet switches. However, because the RDMA transport protocol was fundamentally designed for InfiniBand's flawless, lossless environment, bringing it to Ethernet requires bridging a massive architectural gap.













## Building a Lab Environment

Unlike Soft-RoCE (which works on any NIC), testing PFC requires hardware support. A point-to-point DAC cable connecting two Mellanox ConnectX-4 cards is the ideal lab topology. This creates a single-hop environment with zero external variables, allowing you to observe both sides directly. Used CX-4 cards are widely available on the second-hand market, making them the most cost-effective way to build a lossless Ethernet lab.




## What Exactly Can Be Tested

| Test | What it proves |
|---|---|
| PFC PAUSE frame generation | The receiver NIC sends PAUSE when its buffer threshold is crossed |
| PFC PAUSE frame honoring | The sender NIC stops transmitting when it receives PAUSE |
| Zero loss on PFC-enabled priority | Traffic on the lossless priority experiences 0 drops under congestion |
| Drops on non-PFC priority | Traffic on lossy priorities still drops normally, proving PFC is selective |
| PFC counters | Hardware counters increment correctly on both sides |
| Headroom / buffer tuning | Adjusting thresholds changes when PAUSE fires and how much memory is reserved |

## Prerequisites

Make sure both NICs have IP addresses and can ping each other. Your existing Netplan config (`10.20.0.1/24` and `10.20.0.3/24` on `ens4np0`) works fine for this.

Install the Mellanox tools on **both** machines:

    sudo apt install -y mlnx-tools mstflint

If `mlnx_qos` is not available from `mlnx-tools`, you may need the full MLNX_OFED package. Check with:

    which mlnx_qos

## Step 1: Query Current PFC Settings

On **both** machines, query the current DCB (Data Center Bridging) configuration:

    mlnx_qos -i ens4np0

Sample output:

```
Priority trust state: pcp
PFC configuration:
        priority    0   1   2   3   4   5   6   7
        enabled     0   0   0   0   0   0   0   0
tc: 0 ratelimit: unlimited, tsa: vendor
tc: 1 ratelimit: unlimited, tsa: vendor
...
```

Key things to read:

- **Priority trust state** — `pcp` means the NIC classifies traffic based on the VLAN PCP (Priority Code Point) bits. `dscp` means it uses the IP DSCP field instead. For RoCEv2 testing, `dscp` is standard.
- **PFC enabled** — All zeros means PFC is disabled on every priority. Nothing is lossless yet.
- **tc (traffic class)** — Each traffic class maps to a hardware queue with its own buffer.

You can also query PFC counters:

    ethtool -S ens4np0 | grep pfc

Sample output:

```
rx_pfc0_packets: 0
rx_pfc1_packets: 0
...
tx_pfc0_packets: 0
tx_pfc1_packets: 0
...
```

These are hardware counters showing how many PFC PAUSE frames have been sent and received per priority.

## Step 2: Set Trust Mode to DSCP

RoCEv2 traffic carries a DSCP value in the IP header (typically DSCP 26 for CNP, DSCP 48 for RDMA data). For the NIC to classify this traffic into the correct priority, it needs to trust the DSCP field rather than relying on VLAN PCP tags.

On **both** machines:

    sudo mlnx_qos -i ens4np0 --trust dscp

Verify:

    mlnx_qos -i ens4np0 | grep trust

Should show `Priority trust state: dscp`.

## Step 3: Map Traffic Classes

Create a mapping that assigns priority 3 to traffic class 3 (this is a common RoCEv2 convention, but you can use any priority). The remaining priorities share traffic class 0.

On **both** machines:

    sudo mlnx_qos -i ens4np0 --prio_tc 0,0,0,3,0,0,0,0

This means:
- Priorities 0,1,2,4,5,6,7 → traffic class 0 (lossy, shared buffer)
- Priority 3 → traffic class 3 (will become lossless)

## Step 4: Enable PFC on Priority 3

On **both** machines:

    sudo mlnx_qos -i ens4np0 --pfc 0,0,0,1,0,0,0,0

This enables PFC **only** on priority 3. All other priorities remain lossy (best-effort). When the receiver's buffer for priority 3 fills up, it will send a PFC PAUSE frame telling the sender to stop sending priority 3 traffic.

Verify:

    mlnx_qos -i ens4np0

You should now see:

```
PFC configuration:
        priority    0   1   2   3   4   5   6   7
        enabled     0   0   0   1   0   0   0   0
```

## Step 5: Map DSCP to Priority

Tell the NIC which DSCP value maps to priority 3. For RoCEv2 data traffic, DSCP 26 (the default used by Mellanox firmware) is standard:

    sudo echo 26 3 > /sys/class/net/ens4np0/qos/dscp2prio

Or if using `mlnx_qos`:

    sudo mlnx_qos -i ens4np0 --dscp2prio set,26,3

This means: any packet arriving with DSCP 26 in the IP header → assign to priority 3 → goes into traffic class 3 → protected by PFC.

## Step 6: Verify Buffer Configuration

The ConnectX-4 automatically allocates per-traffic-class buffers when PFC is enabled. The NIC reserves **headroom** for traffic class 3 — this is the buffer space needed to absorb in-flight packets between the moment a PAUSE frame is sent and the moment the sender actually stops.

Query the buffer allocation:

    mlnx_qos -i ens4np0 -b

Or check the kernel buffer settings:

    ethtool -g ens4np0

The headroom calculation depends on:

| Factor | Value |
|---|---|
| Link speed | 25 Gbps (or whatever your DAC supports) |
| MTU | 9000 bytes (jumbo frames) |
| Cable delay | Negligible for a 1-2m DAC |
| NIC response time | ~2-3 μs for ConnectX-4 |

For a point-to-point DAC cable, the default buffer allocation is almost always sufficient. You would only need to manually adjust buffers in multi-hop switch topologies where the round-trip PAUSE propagation delay is longer.

## Step 7: Run the PFC Test

Now generate enough traffic to trigger PFC. The strategy is to flood priority 3 traffic fast enough that the receiver's buffer fills up, forcing it to send PAUSE frames.

**Terminal 1 on rdma2** — start the bandwidth server on the hardware RoCE device:

    ib_write_bw -d rocep4s0 --report_gbits -D 10

**Terminal 2 on rdma1** — fire the client with high queue depth to overwhelm the receiver:

    ib_write_bw -d rocep4s0 --report_gbits -D 10 10.20.0.3

The `-D 10` flag runs the test for 10 seconds (duration mode).

While the test is running, **Terminal 3 on rdma2** — watch the PFC counters in real time:

    watch -n 1 'ethtool -S ens4np0 | grep pfc'

If PFC is working, you'll see `tx_pfc3_packets` incrementing on rdma2 (it's sending PAUSE frames) and `rx_pfc3_packets` incrementing on rdma1 (it's receiving and honoring them).

## Step 8: Prove Losslessness

Check for packet drops on the PFC-enabled priority:

    ethtool -S ens4np0 | grep discard
    ethtool -S ens4np0 | grep drop

On the PFC-enabled priority (3), you should see **zero drops** even under full line-rate load. The PFC mechanism prevented any loss by pausing the sender before the buffer overflowed.

To prove PFC is actually doing something, you can compare:

1. **With PFC enabled on priority 3** — run `ib_write_bw`, observe zero drops, PFC counters incrementing
2. **With PFC disabled** — `sudo mlnx_qos -i ens4np0 --pfc 0,0,0,0,0,0,0,0` — run the same test, observe drops under heavy load

The difference is the proof that PFC delivers lossless behavior.

## Step 9: Clean Up / Reset

To revert all DCB settings to defaults on both machines:

    sudo mlnx_qos -i ens4np0 --pfc 0,0,0,0,0,0,0,0
    sudo mlnx_qos -i ens4np0 --prio_tc 0,0,0,0,0,0,0,0
    sudo mlnx_qos -i ens4np0 --trust pcp

## Summary

PFC is a hardware-only feature — the ConnectX-4 firmware generates and processes PAUSE frames in the NIC silicon, not the CPU. This is precisely why you need the CX-4 for this lab and why a cheap Intel I210 cannot do it. The entire test runs at Layer 2 between two directly connected ports, making your point-to-point DAC topology a perfect single-hop PFC testbed.
