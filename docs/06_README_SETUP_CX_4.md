# Mellanox MCX455A-ECAT ConnectX-4 VPI

<img src="../pics/cx-4.jpg" width="400"/>

The Mellanox MCX455A-ECAT is a single-port QSFP28 network adapter based on the ConnectX-4 VPI (Virtual Protocol Interconnect) architecture. It connects to the host through a PCIe 3.0 x16 interface and operates at speeds up to 100 Gb/s.

The defining feature of this card is its VPI capability: the same physical port can operate in either InfiniBand or Ethernet mode depending on a firmware setting (`LINK_TYPE`). In InfiniBand mode it supports EDR (100 Gb/s); in Ethernet mode it supports 100GbE, 40GbE, 25GbE, and 10GbE.

The ConnectX-4 integrates hardware offload engines for RDMA, allowing applications to move data directly between memory regions on different hosts without involving the CPU or operating system. It also supports SR-IOV, hardware congestion control (DCQCN), and encapsulation offloads (VXLAN, NVGRE).

## Initial Provisioning

Before configuring the adapter, verify that the system detects the card on the PCIe bus.

### PCI Device Detection

```bash
lspci | grep Mellanox
```

Expected output:

```
03:00.0 Infiniband controller: Mellanox Technologies MT27700 Family [ConnectX-4]
```

The device class shown (`Ethernet controller` vs `Infiniband controller`) reflects the current `LINK_TYPE` firmware setting on each card. This is reconfigurable via `mlxconfig`.

### Firmware Inventory (`mlxfwmanager`)

Query the installed firmware on the adapter:

```bash
sudo mlxfwmanager --query
```

Sample output:

```text
Device #1:
----------

  Device Type:      ConnectX4
  Part Number:      MCX455A-ECA_Ax
  Description:      ConnectX-4 VPI adapter card; EDR IB (100Gb/s) and 100GbE; single-port QSFP28; PCIe3.0 x16; ROHS R6
  PSID:             MT_2180110032
  PCI Device Name:  /dev/mst/mt4115_pciconf0
  Base GUID:        ec0d9a030044c158
  Versions:         Current        Available
     FW             12.28.2040     N/A
     PXE            3.6.0102       N/A
     UEFI           14.21.0017     N/A

  Status:           No matching image found
```

**Key observations:**

- **Base GUID:** The adapter's globally unique identifier. In InfiniBand mode the GUID functions like a MAC address, used by the Subnet Manager for routing.
- **PXE & UEFI (Expansion ROMs):** These modules enable network booting. If missing, the previous owner flashed a minimal image that omitted them. This does not affect data-plane performance.
- **"No matching image found":** This means `mlxfwmanager` has no local firmware file to compare against. It is not an error.

Ensure the adapter is running the latest firmware for maximum stability.

### Updating Firmware

Update online (the tool fetches the latest firmware from NVIDIA servers):

    sudo mlxfwmanager -d /dev/mst/mt4115_pciconf0 --online -u

Or download a firmware binary from [NVIDIA Firmware Downloads](https://network.nvidia.com/support/firmware/firmware-downloads/) and flash manually:

    sudo mlxfwmanager \
        -d /dev/mst/mt4115_pciconf0 \
        -i ./fw-ConnectX4-rel-12_28_2040-MCX455A-ECA_Ax-UEFI-14.21.17-FlexBoot-3.6.102.bin \
        -u

A reboot is required after flashing for the new firmware to take effect.

## Firmware Configuration (`mlxconfig`)

While `mlxfwmanager` manages firmware versions, `mlxconfig` manages firmware *settings*. The ConnectX-4 stores dozens of persistent NVRAM parameters that control protocol mode, virtualization, congestion control, and boot behavior. Changes made with `mlxconfig` take effect on the next boot.

### Querying the Full Configuration

    sudo mlxconfig -d /dev/mst/mt4115_pciconf0 query

This produces a full parameter dump. The most important fields for this lab are grouped below.

### Protocol Mode (`LINK_TYPE`)

This is the single most critical setting on a VPI card. It determines the Layer 2 protocol for the port:

| Value | Mode | Protocol Stack |
| ----- | ---- | -------------- |
| `IB(1)` | InfiniBand | IBTA packets, Subnet Manager required |
| `ETH(2)` | Ethernet | IEEE 802.3 frames, standard IP networking |

To switch a port from InfiniBand to Ethernet:

```bash
sudo mlxconfig -d /dev/mst/mt4115_pciconf0 set LINK_TYPE_P1=ETH
sudo reboot
```

### Virtualization & PCIe Settings

| Parameter | Default | Description |
| --------- | ------- | ----------- |
| `SRIOV_EN` | False(0) | SR-IOV. Enable to pass the NIC to multiple VMs. |
| `NUM_OF_VFS` | 0 | Number of Virtual Functions when SR-IOV is enabled. |
| `CQE_COMPRESSION` | BALANCED(0) | Compresses Completion Queue Elements on PCIe bus to save bandwidth at high packet rates. |
| `PCI_ATOMIC_MODE` | Disabled/ExtAtomic | Controls RDMA Atomic operations (Fetch-and-Add, Compare-and-Swap). |

### RoCE and Congestion Control Settings

These parameters are active when the port is in Ethernet mode with RoCEv2:

| Parameter | Description |
| --------- | ----------- |
| `DCBX_IEEE_P1` | Enable IEEE Data Center Bridging eXchange for PFC negotiation with switches. |
| `DCBX_WILLING_P1` | Allow the switch to dictate PFC/ETS configuration. |
| `ROCE_CC_PRIO_MASK_P1` | Bitmask of priorities on which DCQCN congestion control is active. |
| `CNP_802P_PRIO_P1` | 802.1p priority value for Congestion Notification Packets. |
| `CNP_DSCP_P1` | DSCP value for CNP packets. |
| `RPG_AI_RATE_P1` / `RPG_HAI_RATE_P1` | Additive Increase / Hyper-Additive Increase rate recovery parameters. |
| `RPG_MIN_RATE_P1` | Minimum rate the sender will throttle down to. |
| `CLAMP_TGT_RATE_P1` | Whether to clamp to target rate after congestion recovery. |

### Pre-Boot Settings

| Parameter | Default | Description |
| --------- | ------- | ----------- |
| `EXP_ROM_PXE_ENABLE` | True(1) | Enable PXE network boot ROM. |
| `EXP_ROM_UEFI_x86_ENABLE` | True(1) | Enable UEFI network boot ROM. |
| `LEGACY_BOOT_PROTOCOL` | PXE(1) | Boot protocol used during POST. |
| `IP_VER` | IPv4(0) | IP version for network boot. |

These control whether the card attempts network boot during POST. For a lab environment where the OS is installed locally, these can be left at defaults or disabled to speed up boot time.



## Cabling and Link Configuration

Now that `LINK_TYPE` is understood, we can discuss how the physical cabling relates to protocol selection.

### Topology

This lab uses a Point-to-Point (P2P) topology, connecting two nodes (each with one CX-4) directly with a passive DAC (Direct Attach Copper) cable. No switch is required. Passive DAC is the natural choice for short-distance home lab setups: zero optical alignment, near-zero power draw, and no fiber cleaning. Direct copper connection also eliminates switch-hop latency, establishing the lowest-possible-latency baseline for evaluating both InfiniBand and RoCEv2 performance.

### QSFP28 Physical Layer

Both 100GBASE-CR4 (Ethernet) and EDR InfiniBand use identical physical-layer signaling over the same QSFP28 connector:

- **Q** (Quad): four independent electrical lanes for TX and four for RX
- **SFP** (Small Form-factor Pluggable): standard hot-swappable transceiver form factor
- **28**: each lane signals at up to 28 Gbaud (25.78125 Gb/s line rate)
- **Encoding**: 64b/66b, yielding ~25 Gb/s effective data rate per lane
- **Aggregate**: 4 x 25 = 100 Gb/s

The protocols diverge above the physical layer — Ethernet uses IEEE 802.3 frames while InfiniBand uses IBTA packets — but the electrical signals on the copper are identical.

### Cable Selection

The protocol running on the link is determined entirely by the `LINK_TYPE` firmware setting, not by the cable. However, each cable carries an internal EEPROM (SFF-8636) that declares protocol compliance metadata. The firmware reads this EEPROM for cable qualification and will restrict the negotiated speed if compliance codes are missing.

This lab uses two cables — one for each protocol mode:

| Part Number      | Rated Speed | Length | Use When              |
|------------------|-------------|--------|-----------------------|
| MCP1600-E001E30  | 100 Gb/s    | 1 m    | `LINK_TYPE_P1=IB(1)`  |
| MCP1600-C001E30N | 100 Gb/s    | 1 m    | `LINK_TYPE_P1=ETH(2)` |

Both cables are physically identical (same 30 AWG twinaxial copper, same QSFP28 connector, same attenuation). The only difference is how the EEPROM is programmed.

### EEPROM Comparison

Inspect cable EEPROM data with:

```bash
sudo mlxlink -d /dev/mst/mt4115_pciconf0 -m
```

| Field                            | MCP1600-E001E30 (IB cable)           | MCP1600-C001E30N (ETH cable)         |
|----------------------------------|--------------------------------------|--------------------------------------|
| Identifier                       | QSFP+ (0x0D)                         | QSFP28 (0x11)                        |
| Cable Technology                 | Copper cable, passive, unequalized   | Copper cable, passive, unequalized   |
| Transfer Distance                | 1 m                                  | 1 m                                  |
| Attenuation (5g,7g,12g)          | 4,6,9 dB                             | 4,6,9 dB                             |
| Compliance                       | `100GBASE-CR4, CA-25G-L with RS FEC` | `40GBASE-CR4, CA-25G-N with no FEC`  |
| Supported Cable Speed (ETH mode) | 100G,56G,50G,40G,25G,20G,10G,1G      | 100G,50G,40G,25G,20G,10G,1G          |
| Supported Cable Speed (IB mode)  | EDR, FDR, FDR10, QDR, DDR, SDR       | SDR only                             |

The Identifier difference (QSFP+ vs QSFP28) is cosmetic — it reflects the SFF-8024 identifier byte programmed at manufacture time. The IB cable (Rev A2) uses the older QSFP+ code; the ETH cable (Rev A3) uses the newer QSFP28 code. Both operate identically at 100 Gb/s.

**Key EEPROM differences that affect behavior:**

- **InfiniBand cable:** Declares all IB speeds (SDR through EDR) plus 100GBASE-CR4 Ethernet compliance and CA-25G-L (RS-FEC at 25G/50G).

- **Ethernet cable:** No InfiniBand speed codes. Declares 40GBASE-CR4 and CA-25G-N (no FEC at 25G/50G). Does not declare 100GBASE-CR4, yet the firmware still negotiates 100GbE based on electrical capabilities.

### FEC Behavior at 100GbE

At 100GBASE-CR4, RS-FEC (Clause 91) is **mandatory** per IEEE 802.3bj. The CA-L vs CA-N distinction only affects FEC negotiation at 25G and 50G speeds. At 100GbE, both cables negotiate identically:

```text
FEC: Standard RS-FEC - RS(528,514)
```

### Cable Qualification Failures

When the Ethernet cable is plugged in while the card is in **IB mode**, the firmware cannot find EDR compliance in the EEPROM and falls back to SDR (the lowest speed). The troubleshooting output reports `Cable speed not enabled`. This is a cable qualification restriction, not a physical limitation — the copper can carry EDR signals just fine. In **Ethernet mode**, both cables link at full 100GbE without issues.

### Why Use Separate Cables

1. **InfiniBand mode requires it:** The Ethernet cable lacks EDR compliance codes and falls back to SDR. Only the IB cable achieves full EDR (100 Gb/s).
2. **Debugging:** Eliminates cable qualification as a variable when troubleshooting link issues.
3. **Labeling:** Clearly identifies which physical link is running which protocol.
