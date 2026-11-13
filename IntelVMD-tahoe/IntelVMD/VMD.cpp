

#include "VMD.hpp"

#include <libkern/libkern.h>   /* snprintf (Apple's SetNVMeProperties uses it) */
#include <kern/clock.h>        /* Uptime stamps for the demux trace */
#include <pexpert/pexpert.h>   /* PE_parse_boot_argn (vmdaspm opt-in) */

/* Exported by com.apple.kpi.libkern (Libkern.exports); declared
 * here in case the SDK header set does not expose machine_routines.h. */
extern "C" boolean_t ml_get_interrupts_enabled(void);

OSDefineMetaClassAndStructors(VMDDriver, IOPCI2PCIBridge)
OSDefineMetaClassAndStructors(VMDChildDevice, IOPCIDevice)

VMDDriver * pvmd;

/* Milliseconds since boot, for registry trace stamps. */
static unsigned long long vmdUptimeMS(void)
{
    uint64_t abs = 0, ns = 0;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return (unsigned long long)(ns / 1000000ULL);
}

/* Full fence.  OSMemoryBarrier() is user-space only (not in
 * Kernel.framework); the compiler builtin emits mfence on x86_64. */
#define VMD_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)

static IOACPIPlatformDevice * VMDACPIDevice(OSObject * object)
{
    IORegistryEntry * entry = OSDynamicCast(IORegistryEntry, object);
    return entry ? (IOACPIPlatformDevice *) entry : NULL;
}

// --------------------------------------------------------------------------
// kVMDDevices — Linux vmd_ids[] (14 Intel VMD device IDs, verbatim flags).
// --------------------------------------------------------------------------
struct VMDDeviceEntry {
    uint16_t    device;
    const char *name;
    unsigned long features;
};

static const VMDDeviceEntry kVMDDevices[] = {
    /* PCI_VDEVICE(INTEL, VMD_201D), SHADOW_VSCAP */
    { 0x201d, "VMD 201d (Skylake-X/Xeon)",  VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP },
    /* PCI_VDEVICE(INTEL, VMD_28C0), SHADOW | BUS_RESTRICT | CAN_BYPASS_MSI_REMAP */
    { 0x28c0, "VMD 28c0 (Comet Lake)",      VMD_FEAT_HAS_MEMBAR_SHADOW
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_CAN_BYPASS_MSI_REMAP },
    /* PCI_VDEVICE(INTEL, VMD_28C1), SHADOW | CAN_BYPASS_MSI_REMAP | USE_BIOS_INFO */
    { 0x28c1, "VMD 28c1 (BIOS-info)",       VMD_FEAT_HAS_MEMBAR_SHADOW
                                          | VMD_FEAT_CAN_BYPASS_MSI_REMAP
                                          | VMD_FEAT_USE_BIOS_INFO },
    /* VMD_FEATS_CLIENT = SHADOW_VSCAP | BUS_RESTRICT | OFFSET_FIRST_VECTOR
                            | BIOS_PM_QUIRK */
    { 0x467f, "VMD 467f (Alder Lake-N)",    VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0x4c3d, "VMD 4c3d (Rocket Lake-S)",   VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0x9a0b, "VMD 9a0b (Alder Lake-P)",    VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xa77f, "VMD a77f (Raptor Lake)",     VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0x7d0b, "VMD 7d0b (Meteor Lake-P)",   VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xad0b, "VMD ad0b (Raptor Lake-S)",   VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xb60b, "VMD b60b (Meteor Lake)",     VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xb06f, "VMD b06f (Meteor Lake)",     VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xb07f, "VMD b07f (Meteor Lake)",     VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xd70b, "VMD d70b (Lunar Lake)",      VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0xd73b, "VMD d73b (Lunar Lake)",      VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP
                                          | VMD_FEAT_HAS_BUS_RESTRICTIONS
                                          | VMD_FEAT_OFFSET_FIRST_VECTOR
                                          | VMD_FEAT_BIOS_PM_QUIRK },
    { 0, NULL, 0 },
};

static const VMDDeviceEntry * vmdLookupDevice(uint16_t deviceId)
{
    for (int i = 0; kVMDDevices[i].device != 0; i++)
        if (kVMDDevices[i].device == deviceId)
            return &kVMDDevices[i];
    return NULL;
}

// --------------------------------------------------------------------------
// BAR window decoding.
//
// Linux gets struct resource for VMD_CFGBAR / VMD_MEMBAR1 / VMD_MEMBAR2
// from the PCI core.  On macOS the provider IOPCIDevice nub already carries
// the BIOS-assigned windows in its "reg"/"assigned-addresses" properties;
// we parse them instead of hardcoding addresses.
//
// reg entry layout (5 cells of UInt32): physHi, physMid, physLo,
//                                       lengthHi, lengthLo
// physHi is an IOPCIAddressSpace: s.registerNum identifies the config
// register (0x10/0x14/... for BARs), s.space the address space type.
// --------------------------------------------------------------------------





// --------------------------------------------------------------------------
// ecamOffset — Linux vmd_bus_to_ecam() + vmd_cfg_addr().
//
// IOPCIAddressSpace carries the 12-bit config register offset split across
// two fields: s.registerNum (bits 0..7) and es.registerNumExtended (bits
// 24..27 of the raw space word, holding offset bits 8..11).  Family code
// sets the extended bits for extended config accesses.
// --------------------------------------------------------------------------
static inline uint32_t ecamFullOffset(IOPCIAddressSpace space, UInt8 offset)
{
    return ((((uint32_t)space.bits) >> 24) & 0xF) << 8 | offset;
}

static inline uint32_t ecamOffset(uint8_t busnStart, uint64_t cfgBarSize,
                                  IOPCIAddressSpace space, UInt8 offset,
                                  uint32_t len)
{
    uint32_t busnrEcam = space.s.busNum - busnStart;
    uint32_t addr = PCIE_ECAM_OFFSET(busnrEcam,
                                     PCI_DEVFN(space.s.deviceNum,
                                               space.s.functionNum),
                                     ecamFullOffset(space, offset));
    if ((uint64_t)addr + len >= cfgBarSize) return 0xFFFFFFFF;
    return addr;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Linux vmd_ops.read / vmd_ops.write — ECAM config access.
 *
 * "CPU may deadlock if config space is not serialized on some versions of
 *  this hardware, so all config space access is done under a spinlock."
 *   — Linux vmd_pci_read().  fCfgLock is the vmd->cfg_lock port.
 *
 * Writes read the register back: "VMD h/w converts non-posted config writes
 * to posted memory writes. The read-back forces the completion." (Linux
 * vmd_pci_write()).
 *
 * Spaces on the host bus (bus < busn_start) are forwarded to the stock
 * IOPCI2PCIBridge implementation.  Because this kext does NOT define
 * IOPCI2PCIBridge::configRead32, the qualified call below binds to the real
 * IOPCIFamily export at link time.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

UInt32 VMDDriver::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configRead32(space, offset);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 4);
    if (addr == 0xFFFFFFFF) return 0xFFFFFFFF;

    IOInterruptState cfgIs = IOSimpleLockLockDisableInterrupt(fCfgLock);  // Linux: raw_spin_lock_irqsave
    UInt32 value = OSReadLittleInt32(fCfgBar, addr);
    IOSimpleLockUnlockEnableInterrupt(fCfgLock, cfgIs);
    return value;
}

void VMDDriver::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configWrite32(space, offset, data);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 4);
    if (addr == 0xFFFFFFFF) return;

    IOInterruptState cfgIs = IOSimpleLockLockDisableInterrupt(fCfgLock);  // Linux: raw_spin_lock_irqsave
    OSWriteLittleInt32(fCfgBar, addr, data);
    OSReadLittleInt32(fCfgBar, addr);   // force completion (posted writes)
    IOSimpleLockUnlockEnableInterrupt(fCfgLock, cfgIs);
}

UInt16 VMDDriver::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configRead16(space, offset);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 2);
    if (addr == 0xFFFFFFFF) return 0xFFFF;

    IOInterruptState cfgIs = IOSimpleLockLockDisableInterrupt(fCfgLock);  // Linux: raw_spin_lock_irqsave
    UInt16 value = OSReadLittleInt16(fCfgBar, addr);
    IOSimpleLockUnlockEnableInterrupt(fCfgLock, cfgIs);
    return value;
}

void VMDDriver::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configWrite16(space, offset, data);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 2);
    if (addr == 0xFFFFFFFF) return;

    IOInterruptState cfgIs = IOSimpleLockLockDisableInterrupt(fCfgLock);  // Linux: raw_spin_lock_irqsave
    OSWriteLittleInt16(fCfgBar, addr, data);
    OSReadLittleInt16(fCfgBar, addr);
    IOSimpleLockUnlockEnableInterrupt(fCfgLock, cfgIs);
}

UInt8 VMDDriver::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configRead8(space, offset);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 1);
    if (addr == 0xFFFFFFFF) return 0xFF;

    IOInterruptState cfgIs = IOSimpleLockLockDisableInterrupt(fCfgLock);  // Linux: raw_spin_lock_irqsave
    UInt8 value = *(volatile uint8_t *)(fCfgBar + addr);
    IOSimpleLockUnlockEnableInterrupt(fCfgLock, cfgIs);
    return value;
}

void VMDDriver::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configWrite8(space, offset, data);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 1);
    if (addr == 0xFFFFFFFF) return;

    IOInterruptState cfgIs = IOSimpleLockLockDisableInterrupt(fCfgLock);  // Linux: raw_spin_lock_irqsave
    *(volatile uint8_t *)(fCfgBar + addr) = data;
    *(volatile uint8_t *)(fCfgBar + addr);
    IOSimpleLockUnlockEnableInterrupt(fCfgLock, cfgIs);
}

/* VMDChildDevice — same ECAM, always through the owning VMDDriver.
 * A VMDChildDevice never lives on the host bus; if it somehow appears to,
 * fail the access rather than touching hardware that is not ours. */

UInt32 VMDChildDevice::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
    return pvmd->configRead32(space, offset);
}

void VMDChildDevice::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
    if (pvmd) pvmd->configWrite32(space, offset, data);
}

UInt16 VMDChildDevice::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
    return pvmd->configRead16(space, offset);
}

void VMDChildDevice::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
    if (pvmd) pvmd->configWrite16(space, offset, data);
}

UInt8 VMDChildDevice::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
    return pvmd->configRead8(space, offset);
}

void VMDChildDevice::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
    if (pvmd) pvmd->configWrite8(space, offset, data);
}


UInt32 VMDDriver::findPCICapability(IOPCIAddressSpace space, UInt8 capabilityID,
                                    UInt8 *found)
{
    uint32_t data = 0;
    uint32_t offset;

    if (found) *found = 0;

    if (0 == ((kIOPCIStatusCapabilities << 16)
              & configRead32(space, kIOPCIConfigCommand)))
        return 0;

    offset = (0xff & configRead32(space, kIOPCIConfigCapabilitiesPtr));
    if (offset & 3) return 0;

    while (offset) {
        data = configRead32(space, offset);
        if (capabilityID == (data & 0xff)) {
            if (found) *found = offset;
            break;
        }
        offset = (data >> 8) & 0xff;
        if (offset & 3) offset = 0;
    }

    return (offset ? data : 0);
}

UInt32 VMDDriver::extendedFindPCICapability(IOPCIAddressSpace space,
                                            UInt32 capabilityID,
                                            IOByteCount *found)
{
    uint32_t data = 0;
    uint32_t offset;

    if (found) *found = 0;

    if (capabilityID >= 0x100) capabilityID = -capabilityID;

    if (0 == ((kIOPCIStatusCapabilities << 16)
              & configRead32(space, kIOPCIConfigCommand)))
        return 0;

    offset = (0xff & configRead32(space, kIOPCIConfigCapabilitiesPtr));
    if (offset & 3) return 0;

    // normal capability list first — a PCIe cap is needed to anchor the
    // extended chain but vendors sometimes mirror standard caps there too
    while (offset) {
        data = configRead32(space, offset);
        if (capabilityID == (data & 0xff)) {
            if (found) *found = offset;
            return data;
        }
        offset = (data >> 8) & 0xff;
        if (offset & 3) offset = 0;
    }

    // extended capability chain (offsets 0x100..0xfff)
    offset = 0x100;
    while (offset) {
        IOPCIAddressSpace es = space;
        es.bits &= ~0x0F000000;                    // clear s.space/resv/t bits
        es.es.registerNumExtended = (offset >> 8) & 0xF;
        data = configRead32(es, offset & 0xff);

        if ((data & 0xffff) == capabilityID) {
            if (found) *found = offset;
            return data;
        }
        offset = (data >> 20) & 0xfff;
        if ((offset < 0x100) || (offset & 3)) offset = 0;
    }

    return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_get_bus_number_start() — VMCONFIG bus restrictions.
 *
 * "Certain VMD devices may have a root port configuration option which
 *  limits the bus range to between 0-127, 128-255, or 224-255."
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::getBusNumberStart(void)
{
    uint16_t reg = fPCIDev->configRead16(PCI_REG_VMCAP);
    if (!BUS_RESTRICT_CAP(reg)) return true;

    reg = fPCIDev->configRead16(PCI_REG_VMCONFIG);
    if (reg == 0xFFFF) {                       // PCI_POSSIBLE_ERROR()
        IOLog("IntelVMD: VMCONFIG read failed\n");
        return false;
    }

    switch (BUS_RESTRICT_CFG(reg)) {
        case 0:  fBusnStart = 0;   break;
        case 1:  fBusnStart = 128; break;
        case 2:
        case 3:  fBusnStart = 224; break;
        default:
            IOLog("IntelVMD: Unknown Bus Offset Setting (%d)\n",
                  BUS_RESTRICT_CFG(reg));
            return false;
    }
    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_get_phys_offsets() — shadow registers describing the physical
 * location of the membars (used for guest address translation).
 *
 *   nativeHint = true : VMLOCK bit says BAR4 shadow registers are live
 *   nativeHint = false: hypervisor-emulated vendor-specific "SHDW" cap
 *
 * The offsets only matter for resource assignment in guest passthrough;
 * a native macOS host computes them for logging/future use.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::getPhysOffsets(bool nativeHint)
{
    uint64_t phys1 = 0, phys2 = 0;

    if (nativeHint) {
        uint32_t vmlock = fPCIDev->configRead32(PCI_REG_VMLOCK);
        if (vmlock == 0xFFFFFFFF) return false;
        if (!MB2_SHADOW_EN(vmlock)) return true;

        IOMemoryMap *map =
            fPCIDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
        if (!map) return false;
        volatile uint8_t *bar4 = (volatile uint8_t *)map->getVirtualAddress();
        phys1 = OSReadLittleInt64(bar4, MB2_SHADOW_OFFSET);
        phys2 = OSReadLittleInt64(bar4, MB2_SHADOW_OFFSET + 8);
        map->release();
    } else {
        // Hypervisor-Emulated Vendor-Specific Capability ("SHDW")
        uint8_t pos = 0;
        if (!fPCIDev->findPCICapability(PCI_CAP_ID_VNDR, &pos) || !pos)
            return true;
        uint32_t sig = fPCIDev->configRead32(pos + 4);
        if (sig != VMD_SHDW_SIGNATURE) return true;
        phys1 = ((uint64_t)fPCIDev->configRead32(pos + 12) << 32)
              | fPCIDev->configRead32(pos + 8);
        phys2 = ((uint64_t)fPCIDev->configRead32(pos + 20) << 32)
              | fPCIDev->configRead32(pos + 16);
    }

    // *offset1 = resource[MEMBAR1].start - (phys1 & PCI_BASE_ADDRESS_MEM_MASK)
    IODeviceMemory *m1 = fPCIDev->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0 + VMD_MEMBAR1 * 4);
    IODeviceMemory *m2 = fPCIDev->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0 + VMD_MEMBAR2 * 4);
    if (m1) fOffset1 = m1->getPhysicalSegment(0, NULL, kIOMemoryMapperNone)
                    - (phys1 & PCI_BASE_ADDRESS_MEM_MASK);
    if (m2) fOffset2 = m2->getPhysicalSegment(0, NULL, kIOMemoryMapperNone)
                    - (phys2 & PCI_BASE_ADDRESS_MEM_MASK);

    if (fOffset1 || fOffset2)
        IOLog("IntelVMD: guest phys offsets 1=0x%llx 2=0x%llx\n",
              (unsigned long long)fOffset1, (unsigned long long)fOffset2);
    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_get_bus_info_from_bar4() — device 0x28C1 BIOS-info path.
 *
 * "Newer VMD with device ID 0x28C1 ... BIOS enumerates the entire VMD
 *  device tree and stores respective configurations including bus start
 *  range and shadow registers in VMD MMIO space in VMD BAR4/BAR5."
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::getBusInfoFromBar4(void)
{
    IOMemoryMap *map =
        fPCIDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
    if (!map) return false;

    volatile uint8_t *bar4 = (volatile uint8_t *)map->getVirtualAddress();
    uint64_t phys1 = OSReadLittleInt64(bar4, SHADOW_MEMBAR1_28C1);
    uint64_t phys2 = OSReadLittleInt64(bar4, SHADOW_MEMBAR2_28C1);
    uint64_t bar4_2840 = OSReadLittleInt64(bar4, BASE_ID_REG_28C1);

    // 24-bit Base ID: segment/bus/device/function; bus = bits 8..15
    uint32_t baseId  = bar4_2840 & 0xFFFFFF;
    fBusnStart = (baseId >> 8) & 0xFF;

    if (phys1 || phys2) {
        IODeviceMemory *m1 = fPCIDev->getDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0 + VMD_MEMBAR1 * 4);
        IODeviceMemory *m2 = fPCIDev->getDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0 + VMD_MEMBAR2 * 4);
        if (phys1 && m1)
            fOffset1 = m1->getPhysicalSegment(0, NULL, kIOMemoryMapperNone)
                     - (phys1 & PCI_BASE_ADDRESS_MEM_MASK);
        if (phys2 && m2)
            fOffset2 = m2->getPhysicalSegment(0, NULL, kIOMemoryMapperNone)
                     - (phys2 & PCI_BASE_ADDRESS_MEM_MASK);
    }

    map->release();
    IOLog("IntelVMD: BIOS-info busn_start = 0x%02x\n", fBusnStart);
    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_set_msi_remapping() — VMCONFIG_MSI_REMAP bit.
 *
 * enable = true  -> remapping ACTIVE  (bit cleared): child MSIs are
 *                   multiplexed into the VMD MSI-X table, i.e. the demux
 *                   model this driver uses (Linux calls this before
 *                   creating the irq_domain).
 * enable = false -> remapping bypassed (bit set).
 * Skipped for VMD_FEAT_USE_BIOS_INFO devices, like Linux.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::setMSIRemapping(bool enable)
{
    if (fFeatures & VMD_FEAT_USE_BIOS_INFO) return;

    uint16_t reg = fPCIDev->configRead16(PCI_REG_VMCONFIG);
    if (reg == 0xFFFF) return;
    reg = enable ? (reg & ~VMCONFIG_MSI_REMAP) : (reg | VMCONFIG_MSI_REMAP);
    fPCIDev->configWrite16(PCI_REG_VMCONFIG, reg);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_alloc_irqs() — give the VMD device itself its MSI-X vectors.
 *
 * IOPCIFamily programs the VMD MSI-X table (the equivalent of Linux
 * request_irq() for each vmd_irq_list) once the messaged-interrupt
 * controller exists.  Here we only:
 *   - size the MSI-X table (pci_msix_vec_count()),
 *   - ask IOPCIDevice to configure MSI-X with that many vectors,
 *   - reserve vector 0 with an idle event source when
 *     VMD_FEAT_OFFSET_FIRST_VECTOR is set (Linux: vmd->first_vec = 1;
 *     vector 0 is for software triggering, never used for MSI remapping),
 *   - enable MSI-X and disable legacy INTx on the VMD device.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::setupMSIX(void)
{
    uint8_t cap = 0;
    if (!fPCIDev->findPCICapability(kIOPCICapabilityIDMSIX, &cap) || !cap) {
        IOLog("IntelVMD: VMD device has no MSI-X capability\n");
        fPCIDev->setProperty("VMD-MSIX-NoCap", true);
        return false;
    }

    uint16_t ctrl = fPCIDev->configRead16(cap + 2);
    fMsixCount = (ctrl & 0x7FF) + 1;

    IOReturn ret = fPCIDev->configureInterrupts(kIOInterruptTypePCIMessagedX,
                                                fMsixCount, fMsixCount);
    // registry-only breadcrumbs on the VMD node: the MSI-X table is the
    // backbone of child interrupt delivery — every setup stage observable.
    fPCIDev->setProperty("VMD-MSIX-Cap", cap, 32);
    fPCIDev->setProperty("VMD-MSIX-Count", fMsixCount, 32);
    fPCIDev->setProperty("VMD-MSIX-FirstVec",
                         (unsigned long long)fFirstVec, 32);
    fPCIDev->setProperty("VMD-MSIX-Cfg-Stat", ret, 32);
    if (ret != kIOReturnSuccess)
        IOLog("IntelVMD: configureInterrupts(MSIX x%u) -> 0x%x\n",
              fMsixCount, ret);

    // Demux mode owns EVERY VMD vector (vector 0 included) with a
    // primary-context forwarder; the idle vector-0 event source is only
    // kept for the bypass model.
    if (fDemuxMode) {
        // Fail closed: without working VMD vectors remap mode would strand
        // every child MSI, and bypass is the model that already failed on
        // 9A0B.  Refuse to bind instead (VMD0 is then left as without the
        // kext; the reason is in the registry).
        if (ret != kIOReturnSuccess || !registerDemuxVectors()) {
            fPCIDev->setProperty("VMD-Demux-Abort", true);
            IOLog("IntelVMD: demux: no usable VMD vectors - not binding\n");
            return false;
        }
    } else
    // reserve vector 0 (vmd->first_vec)
    if (fVmdWorkLoop) {
        fBridgeES = IOInterruptEventSource::interruptEventSource(
            this,
            OSMemberFunctionCast(IOInterruptEventAction, this,
                                 &VMDDriver::handleBridgeInterrupt),
            fPCIDev, 0);
        if (fBridgeES) {
            fVmdWorkLoop->addEventSource(fBridgeES);
            fBridgeES->enable();
        }
    }

    ctrl = fPCIDev->configRead16(cap + 2);
    ctrl |= 0x8000;                             // PCI_MSIX_FLAGS_ENABLE
    // Also clear Function Mask (bit 14).  Left set by firmware or a
    // previous OS, it silently masks every VMD vector, and in demux mode
    // every child interrupt goes through them.
    ctrl &= (uint16_t)~0x4000;
    fPCIDev->configWrite16(cap + 2, ctrl);
    fPCIDev->setProperty("VMD-MSIX-Ctrl-RB",
                         (unsigned long long)fPCIDev->configRead16(cap + 2), 16);

    uint16_t cmd = fPCIDev->configRead16(kIOPCIConfigCommand);
    cmd |= kIOPCICommandInterruptDisable | kIOPCICommandBusLead;
    fPCIDev->configWrite16(kIOPCIConfigCommand, cmd);
    fPCIDev->setProperty("IOPCIMSIMode", kOSBooleanTrue);

    IOLog("IntelVMD: MSI-X %u vectors, first_vec = %u\n",
          fMsixCount, fFirstVec);
    return true;
}


void VMDDriver::configurePCISpace(const char *propName, IOService *pciDevice,
                       IOPCIAddressSpace addrSpace,
                       UInt64 address,
                       UInt64 length)
{
    bool isExtended = addrSpace.s.t;
    UInt8 spaceType = addrSpace.s.space;
    UInt32 regOffset;

    
    if(isExtended) {
        addrSpace.es.registerNumExtended=(uint32_t)address;
        regOffset = addrSpace.es.registerNum;
    } else {
        regOffset = addrSpace.s.registerNum;
    }
    
    addrSpace.s.reloc=1;
    if (!address ) addrSpace.s.reloc=0;
    if (!strcmp(propName,"ranges")) addrSpace.s.reloc=1;
    
    if (address) {
        if(spaceType != kIOPCIConfigSpace) {
            if(spaceType == kIOPCIIOSpace) {
                UInt32 ioValue = (static_cast<UInt32>(address) & 0xFFFFFFFC) | 0x1;
               // pciDevice->configWrite32(regOffset, (ioValue));
            } else {
                UInt32 addressLo = static_cast<UInt32>(address & 0xFFFFFFFF);
                UInt32 addressHi = static_cast<UInt32>(address >> 32);
                
                addressLo &= ~0xF;
                
                if(addrSpace.s.prefetch) {
                    addressLo |= 0x8;
                }
                
                switch(spaceType) {
                    case kIOPCI32BitMemorySpace:
                        addressLo |= 0x0;
                        break;
                    case kIOPCI64BitMemorySpace:
                        addressLo |= 0x4;
                        break;
                    default:
                        break;
                }
                
                
               // pciDevice->configWrite32(regOffset, (addressLo));
                if (spaceType == kIOPCI64BitMemorySpace) {
                  //  pciDevice->configWrite32(regOffset + 4,0xFFFFFFFF);
                   // pciDevice->configWrite32(regOffset + 4, (addressHi));
                }
            }
        }
    }

    
    if(spaceType == kIOPCI64BitMemorySpace) addrSpace.s.space =kIOPCI32BitMemorySpace;
    
    UInt32 configEntry[5];
    configEntry[0] = (addrSpace.bits);
    
    if(spaceType == kIOPCIIOSpace && address==0) {
        configEntry[1] = 0;
        configEntry[2] = 0;
    } else {
        configEntry[1] = (static_cast<UInt32>(address >> 32));
        configEntry[2] = (static_cast<UInt32>(address & 0xFFFFFFFF));
    }
    configEntry[3] = (static_cast<UInt32>(length >> 32));
    configEntry[4] = (static_cast<UInt32>(length & 0xFFFFFFFF));

    OSData *existing = OSDynamicCast(OSData, pciDevice->getProperty(propName));
    OSData *updated = existing ? OSData::withData(existing) : OSData::withCapacity(0);
    
    if(updated) {
        updated->appendBytes(configEntry, sizeof(configEntry));
        pciDevice->setProperty(propName, updated);
        updated->release();
    }

}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * configure1 — live BAR discovery (tested on a Tiger Lake client VMD, 8086:9A0B).
 *
 * The previous code wrote BAR0-low = 0 and BAR4-low = 0x03100000 and
 * derived the windows from that fixed layout (originally CFGBAR 0x6000000000,
 * MEMBAR1 0x50000000, MEMBAR2 0x6003100000).  That only works on a board whose
 * firmware put CFGBAR/MEMBAR2 at 0x60_xxxxxxxx.  On a board whose BARs sit below 4 GB
 * (high dwords 0) the two writes would move CFGBAR to 0x0 and MEMBAR2 to
 * 0x03100000 (system RAM), and the firmware's own ACPI methods
 * (\_SB.PC00.VMD0.CBAR reads BAR0 at runtime) would then touch RAM.
 *
 * This version:
 *   - reads the live BAR registers (whatever the firmware / IOPCIFamily
 *     left programmed) and writes NOTHING to the VMD BARs,
 *   - takes each BAR size from the provider's IOPCIFamily-generated
 *     "assigned-addresses" / "reg" (sized at boot); the 9A0B client sizes
 *     32 MB / 32 MB / 1 MB are only a last resort,
 *   - validates base != 0, size power of two, base size-aligned, and
 *     aborts the probe (returns false) before any hardware write otherwise,
 *   - publishes the same reg / assigned-addresses / IODeviceMemory layout
 *     the driver published before, built from the live values.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
static uint64_t vmdLiveBar(IOPCIDevice *dev, uint8_t reg, bool *is64)
{
    uint32_t lo  = dev->configRead32(reg);
    bool     w64 = ((lo & 0x6) == 0x4);                 // type 10b = 64-bit
    uint32_t hi  = w64 ? dev->configRead32(reg + 4) : 0;
    if (is64) *is64 = w64;
    if (lo == 0xFFFFFFFFU || (lo & 0x1)) return 0;      // absent / I/O BAR
    return ((((uint64_t)hi) << 32) | lo) & ~0xFULL;
}

static uint64_t vmdPropBar(IORegistryEntry *prov, const char *key,
                           uint8_t reg, uint64_t *baseOut)
{
    OSData *d = OSDynamicCast(OSData, prov->getProperty(key));
    if (!d) return 0;
    const uint32_t *c = (const uint32_t *)d->getBytesNoCopy();
    unsigned int    n = d->getLength() / (5 * sizeof(uint32_t));
    for (unsigned int i = 0; i < n; i++, c += 5) {
        IOPCIAddressSpace s;
        s.bits = c[0];
        if (s.s.space == kIOPCIConfigSpace || s.s.registerNum != reg)
            continue;
        if (baseOut) *baseOut = (((uint64_t)c[1]) << 32) | c[2];
        return (((uint64_t)c[3]) << 32) | c[4];
    }
    return 0;
}

bool VMDDriver::configure1( IOService * provider )
{
    IOPCIDevice *pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) return false;

    static const uint8_t  kReg[3]  = { kIOPCIConfigBaseAddress0,    // CFGBAR
                                       kIOPCIConfigBaseAddress2,    // MEMBAR1
                                       kIOPCIConfigBaseAddress4 };  // MEMBAR2
    static const uint64_t kDflt[3] = { 0x2000000, 0x2000000, 0x100000 };
    static const char    *kName[3] = { "CFGBAR", "MEMBAR1", "MEMBAR2" };
    bool is64[3] = { false, false, false };

    for (int i = 0; i < 3; i++) {
        uint64_t famBase = 0;
        uint64_t live = vmdLiveBar(pciDevice, kReg[i], &is64[i]);
        uint64_t size = vmdPropBar(provider, "assigned-addresses", kReg[i],
                                   &famBase);
        if (!size) size = vmdPropBar(provider, "reg", kReg[i], NULL);
        if (!size) size = kDflt[i];

        IOLog("IntelVMD: live BAR %s reg 0x%02x = 0x%llx (%s-bit) size 0x%llx"
              " family 0x%llx\n", kName[i], kReg[i], (unsigned long long)live,
              is64[i] ? "64" : "32", (unsigned long long)size,
              (unsigned long long)famBase);
        if (famBase && famBase != live)
            IOLog("IntelVMD: %s family/live mismatch - using live register\n",
                  kName[i]);

        bars[i].base  = live;
        bars[i].size  = size;
        bars[i].valid = live && size && !(size & (size - 1))
                     && !(live & (size - 1));
        if (!bars[i].valid) {
            IOLog("IntelVMD: %s unusable (base 0x%llx size 0x%llx) - "
                  "aborting, nothing written\n", kName[i],
                  (unsigned long long)live, (unsigned long long)size);
            return false;
        }
    }

    pciDevice->setProperty("VMD-BAR-Source", "live-config");
    pciDevice->setProperty("VMD-Live-CFGBAR",
                           (unsigned long long)bars[0].base, 64);
    pciDevice->setProperty("VMD-Live-MEMBAR1",
                           (unsigned long long)bars[1].base, 64);
    pciDevice->setProperty("VMD-Live-MEMBAR2",
                           (unsigned long long)bars[2].base, 64);

    // Same property layout as before, filled with the live values.
    provider->removeProperty("reg");
    provider->removeProperty("assigned-addresses");
    provider->removeProperty("IODeviceMemory");

    IOPCIAddressSpace addrSpace = pciDevice->space;
    addrSpace.s.space       = kIOPCIConfigSpace;
    addrSpace.s.registerNum = 0x0;
    configurePCISpace("reg", provider, addrSpace, 0, 0);

    for (int i = 0; i < 3; i++) {
        addrSpace = pciDevice->space;
        addrSpace.s.space = is64[i] ? kIOPCI64BitMemorySpace
                                    : kIOPCI32BitMemorySpace;
        addrSpace.s.registerNum = kReg[i];
        configurePCISpace("reg", provider, addrSpace, 0, bars[i].size);
    }

    IODeviceMemory::InitElement rangeList[3];
    for (int i = 0; i < 3; i++) {
        addrSpace = pciDevice->space;
        addrSpace.s.space = is64[i] ? kIOPCI64BitMemorySpace
                                    : kIOPCI32BitMemorySpace;
        addrSpace.s.registerNum = kReg[i];
        configurePCISpace("assigned-addresses", provider, addrSpace,
                          bars[i].base, bars[i].size);
        rangeList[i].start  = bars[i].base;
        rangeList[i].length = bars[i].size;
        rangeList[i].tag    = addrSpace.bits;
    }

    OSArray *memoryArray = IODeviceMemory::arrayFromList(rangeList, 3);
    if (!memoryArray) return false;
    provider->setDeviceMemory(memoryArray);
    memoryArray->release();
    return true;
}

bool VMDDriver::vmdProbe(IOService *provider)
{
    fPCIDev = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDev) {
        IOLog("IntelVMD: provider is not an IOPCIDevice\n");
        return false;
    }

    if (!configure1(provider)) {
        IOLog("IntelVMD: BAR discovery failed - VMD0 left untouched\n");
        fPCIDev = NULL;   // not retained/opened yet: keep free() off it
        return false;
    }

    fPCIDev->retain();
    fPCIDev->open(this);
    fPCIDev->setMemoryEnable(true);
    fPCIDev->setBusMasterEnable(true);

    uint16_t deviceId = fPCIDev->configRead16(kIOPCIConfigDeviceID);
    const VMDDeviceEntry *ent = vmdLookupDevice(deviceId);
    fFeatures = ent ? ent->features : 0;
    IOLog("IntelVMD: 8086:%04x — %s (%s)\n", deviceId,
          ent ? ent->name : "not in vmd_ids[]",
          ent ? "matched" : "continuing anyway");

    fVmdWorkLoop = IOWorkLoop::workLoop();
    fCfgLock     = IOSimpleLockAlloc();

    // pcim_iomap(VMD_CFGBAR)
    if (!decodeAndPublishBars()) {
        IOLog("IntelVMD: cannot decode/map the VMD BARs\n");
        return false;
    }
    fCfgBar = (volatile uint8_t *)fCfgMap->getVirtualAddress();

    // Linux: resource_size(&dev->resource[VMD_CFGBAR]) < (1 << 20) -> -ENOMEM
    if (fCfgBarSize < (1ULL << 20)) {
        IOLog("IntelVMD: CFGBAR smaller than 1 MB\n");
        return false;
    }

    // vmd_prepare_offsets_and_bus()
    uint64_t membar2Offset = MB2_SHADOW_OFFSET;
    if (fFeatures & VMD_FEAT_USE_BIOS_INFO) {
        membar2Offset = MEMBAR2_OFFSET_28C1;
        if (!getBusInfoFromBar4()) return false;
    } else if (fFeatures & VMD_FEAT_HAS_MEMBAR_SHADOW) {
        membar2Offset = MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE;
        if (!getPhysOffsets(true)) return false;
    } else if (fFeatures & VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP) {
        if (!getPhysOffsets(false)) return false;
    }
    fMembar2Offset = membar2Offset;

    if ((fFeatures & VMD_FEAT_HAS_BUS_RESTRICTIONS) && !getBusNumberStart())
        return false;

    // busn_end = busn_start + (resource_size(cfgbar) >> 20) - 1, capped 0xff
    // (Linux vmd_enable_domain: "Do not let resource[0] end go out of bounds")
    uint64_t busnEnd = (uint64_t)fBusnStart + (fCfgBarSize >> 20) - 1;
    if (busnEnd > 0xFF) busnEnd = 0xFF;
    fBusnEnd = (uint8_t)busnEnd;

    if (fFeatures & VMD_FEAT_OFFSET_FIRST_VECTOR) fFirstVec = 1;
    fNextFreeVec = fFirstVec + 1;   /* vmd_next_irq(): private NVMe vectors
                                     * start above the shared "slow" vector */

    // ---- interrupt model (Linux vmd_probe(), linuxvmd.c:991) ------------
    //
    //   demux : VMCONFIG.MSI_REMAP clear (remap ACTIVE) — child MSIs are
    //           multiplexed into the VMD's own MSI-X table and demuxed by
    //           this driver (Linux: vmd_alloc_irqs + vmd_create_irq_domain).
    //   bypass: VMCONFIG.MSI_REMAP set   (remap BYPASSED) — child MSIs
    //           egress the VMD untranslated and the STOCK IOPCIFamily
    //           messaged-interrupt controller owns them end to end (Linux
    //           creates no irq_domain at all for CAN_BYPASS devices).
    //

    // Linux semantics.  Client VMDs (9A0B) lack
    // VMD_FEAT_CAN_BYPASS_MSI_REMAP, so they run remap-active with a
    // VMD-vector -> child handler demux (registerDemuxVectors() +
    // VMDChildDevice interrupt overrides).  The forced bypass used before
    // left child MSIs undeliverable on a Tiger Lake laptop: the VMD accepted the
    // VMCONFIG bit but Apple's NVMe driver never saw an interrupt.
    fDemuxMode = !(fFeatures & VMD_FEAT_CAN_BYPASS_MSI_REMAP);
    fPCIDev->setProperty("VMD-Start-MS", vmdUptimeMS(), 64);

    fPCIDev->setProperty("VMD-MSI-Model", fDemuxMode ? "demux" : "bypass");

    if (!setupMSIX()) return false;
    
    setMSIRemapping(fDemuxMode);
    {
        // read-back: bit 1 set = MSI remapping bypassed (what we asked for)
        uint16_t vmcfg = fPCIDev->configRead16(PCI_REG_VMCONFIG);
        fPCIDev->setProperty("VMD-VMCONFIG", (unsigned long long)vmcfg, 16);
        fPCIDev->setProperty("VMD-MSI-Bypass-Active",
                             (vmcfg & VMCONFIG_MSI_REMAP) != 0);
        IOLog("IntelVMD: VMCONFIG=0x%04x (MSI remap %s)\n", vmcfg,
              (vmcfg & VMCONFIG_MSI_REMAP) ? "BYPASSED" : "ACTIVE");
    }

    // pci_create_root_bus(..., vmd->busn_start, ...): the "bridge space" is
    // the root port at bus 0 / device 0 / function 0 of the VMD domain.
    IOPCIAddressSpace space;
    space.bits = 0;
    space.s.busNum = fBusnStart;
    fBridgeSpace = space;

    // pci_add_resource_offset(): MEMBAR1 -> 32-bit memory window,
    // MEMBAR2 (+membar2_offset) -> prefetchable window for the children.
    for (uint32_t i = 0; i < fWindowCount; i++) {
        if (fWindows[i].type == kIOPCIResourceTypeMemory) {
            addBridgeMemoryRange((IOPhysicalAddress)fWindows[i].start,
                                 (IOPhysicalLength)fWindows[i].size, true);
        } else if (fWindows[i].type == kIOPCIResourceTypePrefetchMemory) {
            uint64_t start = fWindows[i].start + fMembar2Offset;
            uint64_t size  = (fWindows[i].size > fMembar2Offset)
                           ? fWindows[i].size - fMembar2Offset : 0;
            if (size) {
                addBridgePrefetchableMemoryRange(start, size);
                fWindows[i].start = start;
                fWindows[i].size  = size;
            }
        }
    }

    IOLog("IntelVMD: bound to VMD domain, buses 0x%02x-0x%02x\n",
          fBusnStart, fBusnEnd);
    return true;
}


bool VMDDriver::decodeAndPublishBars(void)
{

    fCfgBarSize = bars[0].size;

    fWindowCount = 0;
    if (bars[1].valid)
        fWindows[fWindowCount++] = { kIOPCIResourceTypeMemory,
                                     bars[1].base, bars[1].size };
    if (bars[2].valid)
        fWindows[fWindowCount++] = { kIOPCIResourceTypePrefetchMemory,
                                     bars[2].base, bars[2].size };

    IOLog("IntelVMD: CFGBAR 0x%llx/0x%llx MEMBAR1 0x%llx/0x%llx "
          "MEMBAR2 0x%llx/0x%llx\n",
          (unsigned long long)bars[0].base, (unsigned long long)bars[0].size,
          (unsigned long long)bars[1].base, (unsigned long long)bars[1].size,
          (unsigned long long)bars[2].base, (unsigned long long)bars[2].size);

    
    fCfgMap = fPCIDev->mapDeviceMemoryWithIndex(0);
    
    //fMap1= fPCIDev->mapDeviceMemoryWithIndex(2);
    //fMap2= fPCIDev->mapDeviceMemoryWithIndex(4);
    
    if (!fCfgMap) {
        IOLog("IntelVMD: cannot map CFGBAR\n");
        return false;
    }
    return true;
}


bool VMDDriver::start(IOService *provider)
{
    // Demux state, set before anything can fail (free() reads it)
    for (uint32_t i = 0; i < kVMDMaxDemuxKids; i++) fDemuxKids[i] = NULL;
    fDemuxKidCount = 0;
    for (uint32_t i = 0; i < kVMDMaxIrqStat; i++) { fIrqHits[i] = 0; fIrqOrphan[i] = 0; }
    fIrqVectorsRegistered = 0;
    fRegMask = 0;
    fIrqStatTimer = NULL;
    fIrqStatTicks = 0;

    pvmd=this;
    
    if (!vmdProbe(provider)) {
        IOLog("IntelVMD: probe failed\n");
        pvmd = NULL;
        return false;
    }
    
    //fPCIDev->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    //fPCIDev->setPowerState(kIOPCIDeviceOnState, this);
    
    fPCIDev->setProperty("VMD-Stage-Probe", true);
    if (!seedBridgeEntry()) {
        IOLog("IntelVMD: cannot seed the bridge config entry\n");
        return false;
    }
    fPCIDev->setProperty("VMD-Stage-Seed", true);

    bool ok = IOPCI2PCIBridge::start(provider);
    if (!ok) {
        IOLog("IntelVMD: IOPCI2PCIBridge::start failed\n");
        return false;
    }
    fPCIDev->setProperty("VMD-Stage-FamilyStart", true);
    
    // pci_scan_child_bus()
    configOpParams cp;
    cp.device = fPCIDev;
    cp.op     = kConfigOpScan;
    cp.result = NULL;
    configOp(&cp);
    
    // scan census: how many entries the configurator hung off the root
    // complex.  0 means the scan found nothing — check the ECAM accessors.
    uint32_t scanKids = 0;
    for (IOPCIConfigEntry *c = fBridgeEntry->child; c; c = c->peer)
        scanKids++;
    fPCIDev->setProperty("VMD-Stage-Scan-Children", scanKids, 32);
    
    postScanConfigure();
    fPCIDev->setProperty("VMD-Stage-PostScan-Done", true);

    // last-moment decode re-assert before driver matching begins: anything
    // between postScanConfigure() and matching (family bridge init,
    // checkLink on power paths) may have zeroed the windows again.
    //vmdReassertAll();
    fPCIDev->setProperty("VMD-Stage-PreRegister-Reassert", true);

    // 1 s timer - publishes the demux counters (VMD-IRQ-Hits/Orphan,
    // VMD-INT-Dispatched/Synthetic/Latched) every tick for 10 minutes, then
    // every 10 s, and runs the memory-only lost-interrupt safety net.
    if (fDemuxMode && fVmdWorkLoop) {
        fIrqStatTimer = IOTimerEventSource::timerEventSource(this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                 &VMDDriver::irqStatTick));
        if (fIrqStatTimer
            && fVmdWorkLoop->addEventSource(fIrqStatTimer) == kIOReturnSuccess) {
            fIrqStatTimer->setTimeoutMS(1000);
        } else if (fIrqStatTimer) {
            fIrqStatTimer->release();
            fIrqStatTimer = NULL;
        }
    }

    // The domain is configured (reset, windows, MSI wiring, demux
    // claim).  Only now register the endpoints that were created with
    // "pci-deferred-registration", so a client driver (IONVMeController)
    // can match.  An endpoint not attached yet loses the key and is then
    // registered by IOPCIBridge::publishNub() itself.
    // one-time read-only snapshot of the VMD's own MSI-X table, taken while
    // no client driver can be doing I/O yet (the endpoints are still
    // deferred)
    snapshotVmdMsix();

    fPCIDev->setProperty("VMD-PostScan-Done-MS", vmdUptimeMS(), 64);
    {
        uint32_t released = 0, pending = 0, late = 0;
        for (UInt32 i = 0; i < fDemuxKidCount && i < kVMDMaxDemuxKids; i++) {
            VMDChildDevice *kid = fDemuxKids[i];
            if (!kid || !kid->fRegDeferred) continue;
            if (kid->fCfgPending) { late++; continue; }   // irqStatTick finishes it
            if (releaseDeferredKid(kid)) released++; else pending++;
        }
        fPCIDev->setProperty("VMD-Reg-Released", released, 32);
        fPCIDev->setProperty("VMD-Reg-Pending", pending, 32);
        fPCIDev->setProperty("VMD-Reg-Late", late, 32);
    }

    registerService();

    
    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * seedBridgeEntry — describe the VMD root complex to the IOPCIFamily
 * configurator.  Linux's pci_create_root_bus() does all of this from the
 * PCI core; on macOS the VMD device has a type-0 header (it is not a real
 * bridge), so the config entry that the configurator built for it must be
 * turned into a host-bridge entry by hand.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::seedBridgeEntry(void)
{
    fBridgeEntry = VMDChildDevice::configEntryOf(fPCIDev);
    if (!fBridgeEntry) {
        IOLog("IntelVMD: VMD device has no IOPCIConfigEntry\n");
        return false;
    }
    IOPCIConfigEntry *bridge = fBridgeEntry;

    bridge->vendorProduct = fPCIDev->configRead32(kIOPCIConfigVendorID);
    bridge->classCode     = 0x060000;              // host bridge
    bridge->headerType    = kPCIHeaderType1;
    bridge->secBusNum     = firstBusNum();
    bridge->subBusNum     = lastBusNum();
    bridge->subDeviceNum  = 0;
    bridge->endDeviceNum  = 31;
    bridge->space         = getBridgeSpace();
    bridge->isHostBridge  = true;
    bridge->isBridge      = true;
    bridge->supportsHotPlug = kPCIStatic;
    bridge->hostBridge    = (IOPCIHostBridge *)this;
    bridge->hostBridgeEntry = bridge;
    bridge->rootPortEntry = bridge;
    bridge->expressEndpointMaxReadRequestSize = -1;
    bridge->expressCapBlock = 0;

    if (bridge->dtEntry) {
        bridge->dtEntry->setProperty(kIOPCIIgnoreLinkStatusKey, true);
        bridge->dtEntry->setProperty(kIOPCIExpressASPMDefaultKey, 2, 32);
    }

    // Bus-number range (Linux: vmd->resources[0] IORESOURCE_BUS).
    uint64_t busStart = bridge->secBusNum;
    uint64_t busSize  = bridge->subBusNum - bridge->secBusNum + 1;
    IOPCIRange *range = new IOPCIRange;
    if (range) {
        bzero(range, sizeof(*range));
        range->type         = kIOPCIResourceTypeBusNumber;
        range->start        = busStart;
        range->proposedSize = busSize;
        range->totalSize    = busSize;
        range->end          = busStart;
        range->alignment    = kPCIBridgeBusNumberAlignment;
        range->maxAddress   = 0xFFFFFFFF;
        range->allocations  = (IOPCIRange *)&range->end;
        range->size         = range->proposedSize;
        range->end          = range->start + range->size;
        bridge->ranges[kIOPCIRangeBridgeBusNumber] = range;
    }

    // Memory windows behind the bridge (Linux: vmd->resources[1]/[2]).
    for (uint32_t i = 0; i < fWindowCount; i++) {
        uint32_t rangeIndex = (fWindows[i].type == kIOPCIResourceTypeMemory)
                            ? kIOPCIRangeBridgeMemory
                            : kIOPCIRangeBridgePFMemory;
        IOPCIRange *wr = new IOPCIRange;
        if (!wr) continue;
        bzero(wr, sizeof(*wr));
        wr->type         = (uint8_t)fWindows[i].type;
        wr->start        = fWindows[i].start;
        wr->proposedSize = fWindows[i].size;
        wr->totalSize    = fWindows[i].size;
        wr->end          = fWindows[i].start;
        wr->alignment    = kPCIBridgeMemoryAlignment;
        wr->maxAddress   = 0xFFFFFFFF;
        wr->allocations  = (IOPCIRange *)&wr->end;
        wr->size         = wr->proposedSize;
        wr->end          = wr->start + wr->size;
        bridge->ranges[rangeIndex] = wr;
    }

    // The VMD device's own BARs as entry ranges (window decode for the
    // configurator).
    OSArray *deviceMemory = fPCIDev->getDeviceMemory();
    for (uint32_t idx = 0; deviceMemory; idx++) {
        IOMemoryDescriptor *mem =
            (IOMemoryDescriptor *)deviceMemory->getObject(idx);
        if (!mem) break;
        IOPCIAddressSpace tag;
        tag.bits = mem->getTag();
        if (tag.s.registerNum < kIOPCIConfigBaseAddress0) continue;
        uint32_t barIdx = (tag.s.registerNum - kIOPCIConfigBaseAddress0) / 4;
        if (barIdx >= kIOPCIRangeExpansionROM) continue;

        IOPCIRange *br = new IOPCIRange;
        if (!br) continue;
        bzero(br, sizeof(*br));
        br->type         = kIOPCIResourceTypeMemory;
        br->start        = mem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
        br->proposedSize = mem->getLength();
        br->totalSize    = br->proposedSize;
        br->end          = br->start;
        br->alignment    = kPCIBridgeMemoryAlignment;
        br->maxAddress   = 0xFFFFFFFF;
        br->allocations  = (IOPCIRange *)&br->end;
        br->size         = br->proposedSize;
        br->end          = br->start + br->size;
        bridge->ranges[barIdx] = br;
    }

    // Publish the VMD device's own ACPI path on the bridge DT entry as a
    // last resort for IONVMeController::CopyACPIParent() parent-plane walks.
    IOACPIPlatformDevice *selfAcpi = VMDACPIDevice(bridge->acpiDevice);
    if (!selfAcpi)
        selfAcpi = VMDACPIDevice(fPCIDev->getProperty("acpi-device"));
    if (selfAcpi && bridge->dtEntry) {
        char path[512];
        bzero(path, sizeof(path));
        int s=sizeof(path);
        if (selfAcpi->getPath(path, &s, gIOServicePlane) && path[0]) {
            /* Memory leak fix: withCString() returns a retained string and
             * setProperty() takes its own reference — the old inline form
             * orphaned the withCString() reference on every boot. */
            OSString *pathStr = OSString::withCString(path);
            if (pathStr) {
                bridge->dtEntry->setProperty(kACPIDevicePathKey, pathStr);
                pathStr->release();
            }
        }
        bridge->dtEntry->setProperty("acpi-device", selfAcpi);
    }

    // Intel VMD _DSM function 5 (VD3A) — keep D3 power management enabled.
   /* if (selfAcpi) {
        static const uint8_t uuidBytes[16] = {
            0xdd, 0xcd, 0xd9, 0x9c, 0x45, 0x88, 0xfd, 0x4a,
            0x83, 0x92, 0x31, 0xc4, 0xeb, 0x87, 0xca, 0xbd };
        OSData   *uuid    = OSData::withBytes(uuidBytes, sizeof(uuidBytes));
        OSNumber *rev     = OSNumber::withNumber((uint32_t)1, 32);
        OSNumber *funcIdx = OSNumber::withNumber((uint32_t)5, 32);
        OSNumber *one     = OSNumber::withNumber((uint32_t)1, 32);
        OSArray  *package = OSArray::withCapacity(1);
        if (package && one) package->setObject(one);
        if (uuid && rev && funcIdx && package) {
            OSObject *args[4] = { uuid, rev, funcIdx, package };
            IOReturn ret = selfAcpi->evaluateObject("_DSM", NULL, args, 4);
            IOLog("IntelVMD: _DSM VD3A -> 0x%x\n", ret);
        }
        if (one) one->release();
        if (package) package->release();
        if (funcIdx) funcIdx->release();
        if (rev) rev->release();
        if (uuid) uuid->release();
    } else {
        IOLog("IntelVMD: no ACPI node for the VMD device; skipping _DSM\n");
    }*/

    // Save the VMD device's config registers into the IOPCIFamily shadow
    // (used by saveDeviceState/restoreDeviceState across sleep).
    IOPCIConfigShadow *shadow = configShadow(fPCIDev);
    if (shadow)                              // safety fix: was deref'd unchecked
        for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
            shadow->configSave.savedConfig[i] = fPCIDev->configRead32(i * 4);

    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * postScanConfigure — per-device work after pci_scan_child_bus(), applied
 * to EVERY device in the VMD domain (root ports, the mirrored AHCI,
 * endpoints — any depth), not just one bridge + NVMe pair:
 *
 *   1. vmd_acpi_find_companion() for every device, so drivers (Apple's
 *      IONVMeFamily included) find acpiRootDevice.
 *   2. vmd_pm_enable_quirk() — driver-default LTR values, every device.
 *   3. D0 + command.MEM/BUS-MASTER on every device.
 *   4. EVERY type-1 header gets its memory / prefetchable windows
 *      reprogrammed over ALL downstream BARs (any depth) — resetDomain()
 *      closed them all, so fixing one bridge only left every other child
 *      master-aborting.
 *   5. IOPCIConfigEntry sync — the configurator's shadow of every device.
 *      entry->ranges[] (BARs, kIOPCIRangeBridgeMemory/PFMemory) are the
 *      source of truth for the configurator's apply/restore passes
 *      (sleep-wake, kConfigOpRealloc) and IOPCIDevice::restoreDeviceState()
 *      writes back configSave.savedConfig[].  Writing the fixed windows to
 *      hardware only — without refreshing the entry — made the next
 *      restore pass write the STALE windows back, forcing every consumer
 *      to remap.  Here the entry and the shadow are synced to the FIXED
 *      hardware state, so restore passes write the fixed values back and
 *      nothing needs to remap.
 *   6. child MSI wiring onto the VMD's own MSI-X vectors (irq demux) for
 *      every device.
 *   7. every endpoint behind a root port: ASPM off on both link partners
 *      + DLLLA wait (endpointLinkPrep); NVMe-class endpoints additionally
 *      get vmdBridgeCheck() + stabilizeNVMeForCAP() + appleNVMeInitDebug().
 *
 * All diagnostics are registry properties: VMD-Cfg-* on each device's own
 * nub, VMD-Dbg-* on the NVMe endpoint.  No IOLog from this pass.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Window helpers — definitions live with vmdBridgeCheck below. */
static bool vmdWindowCovers(uint32_t reg32, uint64_t lo, uint64_t hi);
static void vmdProgramWindow(IOPCIDevice *rp, uint8_t offset,
                             uint64_t lo, uint64_t hi);

/* Device-property boolean: OSBoolean or 1-byte OSData (config.plist
 * DeviceProperties inject booleans as OSData). */
static bool vmdBoolProperty(IOService *dev, const char *key, bool dflt)
{
    OSObject *obj = dev ? dev->getProperty(key) : NULL;
    if (OSBoolean *b = OSDynamicCast(OSBoolean, obj)) return b->getValue();
    if (OSData *d = OSDynamicCast(OSData, obj))
        if (d->getLength() >= 1)
            return (((const uint8_t *)d->getBytesNoCopy())[0] != 0);
    return dflt;
}

static IOPCIDevice * vmdEntryNub(IOPCIConfigEntry *entry)
{
    return entry ? OSDynamicCast(IOPCIDevice, entry->dtNub) : NULL;
}

/* vmdBarLength — length of one BAR from the nub's "IODeviceMemory"
 * descriptors (built from assigned-addresses at scan time).  0 = unknown. */
static uint64_t vmdBarLength(IOPCIDevice *nub, uint32_t regOffset)
{
    OSArray *deviceMemory = nub->getDeviceMemory();
    for (uint32_t idx = 0; deviceMemory; idx++) {
        IOMemoryDescriptor *mem =
            (IOMemoryDescriptor *)deviceMemory->getObject(idx);
        if (!mem) break;
        IOPCIAddressSpace tag;
        tag.bits = mem->getTag();
        if (tag.s.registerNum == regOffset) return mem->getLength();
    }
    return 0;
}

/* How many BAR slots this header type exposes (type-1 bridges: 2 — reading
 * BAR2+ on a type-1 header returns the bridge window registers instead). */
static uint32_t vmdMaxBars(IOPCIDevice *nub)
{
    uint8_t hdr = nub->configRead8(kIOPCIConfigHeaderType) & 0x7f;
    if (hdr == kPCIHeaderType0) return 6;
    if (hdr == kPCIHeaderType1) return 2;
    return 0;
}

static void vmdAddDeviceBars(IOPCIDevice *nub, IOPCIConfigEntry *entry,
                             uint64_t *memMin, uint64_t *memMax, bool *haveMem,
                             uint64_t *pfMin, uint64_t *pfMax, bool *havePF)
{
    uint32_t maxBars = vmdMaxBars(nub);
    for (uint32_t barIdx = 0; barIdx < maxBars; ) {
        uint32_t lo = nub->configRead32(kIOPCIConfigBaseAddress0 + 4 * barIdx);
        if (lo == 0xFFFFFFFFU) break;
        if (lo & 0x1) { barIdx++; continue; }          // I/O BAR — n/a
        bool is64 = (((lo >> 1) & 0x3) == 0x2);
        uint32_t hi = is64
            ? nub->configRead32(kIOPCIConfigBaseAddress0 + 4 * barIdx + 4) : 0;
        uint64_t base = (((uint64_t)hi << 32) | lo) & ~0xFULL;
        if (base) {
            uint64_t len = vmdBarLength(nub, kIOPCIConfigBaseAddress0
                                              + 4 * barIdx);
            if (!len && entry && entry->ranges[kIOPCIRangeBAR0 + barIdx])
                len = entry->ranges[kIOPCIRangeBAR0 + barIdx]->size;
            uint64_t end = base + ((len) ? (len - 1) : 0xFFFFFULL);
            if (lo & 0x8) {                            // prefetchable
                *pfMin = (base < *pfMin) ? base : *pfMin;
                *pfMax = (end  > *pfMax) ? end  : *pfMax;
                *havePF = true;
            } else {
                *memMin = (base < *memMin) ? base : *memMin;
                *memMax = (end  > *memMax) ? end  : *memMax;
                *haveMem = true;
            }
        }
        barIdx += is64 ? 2 : 1;
    }
}

/* vmdCollectSubtreeBars — all BARs in a bridge's subtree, any depth. */
static void vmdCollectSubtreeBars(IOPCIConfigEntry *bridge,
                                  uint64_t *memMin, uint64_t *memMax,
                                  bool *haveMem,
                                  uint64_t *pfMin, uint64_t *pfMax,
                                  bool *havePF, int depth)
{
    if (!bridge || depth > 4) return;
    for (IOPCIConfigEntry *child = bridge->child; child; child = child->peer) {
        IOPCIDevice *nub = vmdEntryNub(child);
        if (!nub) continue;
        vmdAddDeviceBars(nub, child, memMin, memMax, haveMem,
                         pfMin, pfMax, havePF);
        if (child->isBridge)
            vmdCollectSubtreeBars(child, memMin, memMax, haveMem,
                                  pfMin, pfMax, havePF, depth + 1);
    }
}

/* vmdSyncWindowRange — keep ONE bridge-window IOPCIRange of the
 * configurator entry equal to the programmed hardware window.  ranges[]
 * are what the configurator's apply/restore passes write back; leaving
 * them stale is what made later restores undo the window fix. */
static void vmdSyncWindowRange(IOPCIConfigEntry *bridge, IOPCIDevice *nub,
                               uint32_t rangeIndex,
                               uint64_t wLo, uint64_t wHiEx)
{
    IOPCIRange *range = bridge->ranges[rangeIndex];
    if (!range) {
        // configurator never created this window range — flag it so ioreg
        // shows why the entry could not be synced
        if (nub) nub->setProperty("VMD-Cfg-Range-Missing", rangeIndex, 32);
        return;
    }
    if (wHiEx <= wLo) return;

    uint64_t span = wHiEx - wLo;
    range->start        = wLo;
    range->end          = wHiEx;
    range->size         = span;
    range->totalSize    = span;
    range->proposedSize = span;
}

/* vmdSyncConfigEntry — sync one device's IOPCIConfigEntry + IOPCIDevice
 * save/restore shadow to the live (fixed) hardware state:
 *   - BAR ranges: entry->ranges[BARn].start := live BAR base (the BARs are
 *     not touched by resetDomain(), but keep the entry honest if anything
 *     clobbered them),
 *   - configSave.savedConfig[]: what IOPCIDevice::restoreDeviceState()
 *     writes back on the next D3 -> D0 / sleep-wake pass.
 */
static void vmdSyncConfigEntry(IOPCIDevice *nub, IOPCIConfigEntry *entry,
                               bool isBridge)
{
    if (!nub || !entry) return;

    uint32_t synced  = 0;
    uint32_t maxBars = vmdMaxBars(nub);
    for (uint32_t barIdx = 0; barIdx < maxBars; ) {
        uint32_t lo = nub->configRead32(kIOPCIConfigBaseAddress0 + 4 * barIdx);
        if (lo == 0xFFFFFFFFU) break;
        bool is64 = (((lo >> 1) & 0x3) == 0x2);
        if (!(lo & 0x1)) {
            uint32_t hi = is64
                ? nub->configRead32(kIOPCIConfigBaseAddress0 + 4 * barIdx + 4)
                : 0;
            uint64_t base = (((uint64_t)hi << 32) | lo) & ~0xFULL;
            IOPCIRange *range = entry->ranges[kIOPCIRangeBAR0 + barIdx];
            if (range && base && range->start != base) {
                range->start = base;
                range->end   = base + range->size;
                synced++;
            }
        }
        barIdx += is64 ? 2 : 1;
    }

    // Save/restore shadow — bridge headers keep 32 dwords, endpoints 16
    // (the same split the family uses for save/restoreDeviceState()).
    IOPCIConfigShadow *shadow = configShadow(nub);
    if (shadow) {
        uint32_t regs = isBridge ? kIOPCIConfigBridgeShadowRegs
                                 : kIOPCIConfigEPShadowRegs;
        for (uint32_t i = 0; i < regs; i++)
            shadow->configSave.savedConfig[i] = nub->configRead32(i * 4);
    }

    nub->setProperty("VMD-Cfg-Ranges-Synced", synced, 32);
    nub->setProperty("VMD-Cfg-Shadow", shadow != NULL);
}

/* vmdPowerUpDecode — D0 + command.MEM/BUS-MASTER for ONE device.  MEM
 * decode must be on for MMIO to answer at all, bus master for MSI/DMA. */
static void vmdPowerUpDecode(IOPCIDevice *nub)
{
    UInt8 pm = 0;
    if (nub->findPCICapability(kIOPCIPowerManagementCapability, &pm) && pm) {
        UInt16 pmcsr = nub->configRead16(pm + 4);
        if (pmcsr & 0x3) {                         // D1/D2/D3 -> D0
            nub->configWrite16(pm + 4, pmcsr & ~0x0003);
            IOSleep(10);
            nub->setProperty("VMD-Cfg-D0-Fixed", true);
        }
    }

    UInt16 cmd = nub->configRead16(kIOPCIConfigCommand);
    nub->setProperty("VMD-Cfg-CMD-Pre", cmd, 16);
    if ((cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
                != (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) {
        nub->configWrite16(kIOPCIConfigCommand,
                           cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        IOSleep(2);
        nub->setProperty("VMD-Cfg-CMD-Fixed", true);
    }
}

/* vmdFixBridgeWindows — reprogram ONE bridge's memory / prefetchable
 * windows over ALL downstream BARs (any depth), sync the configurator
 * entry's window ranges to the programmed values, and pin the bridge I/O
 * window to the DISABLED encoding (a VMD domain has no I/O space; Linux
 * receipts in the body comment). */
static void vmdFixBridgeWindows(IOPCIConfigEntry *bridge,
                                IOPCIDevice *bridgeNub)
{
    uint64_t memMin = ~0ULL, memMax = 0, pfMin = ~0ULL, pfMax = 0;
    bool haveMem = false, havePF = false;

    vmdCollectSubtreeBars(bridge, &memMin, &memMax, &haveMem,
                          &pfMin, &pfMax, &havePF, 0);

    if (haveMem) {
        bridgeNub->setProperty("VMD-Cfg-Sub-MEM-Base",
                               (unsigned long long)memMin, 64);
        bridgeNub->setProperty("VMD-Cfg-Sub-MEM-End",
                               (unsigned long long)memMax, 64);
        uint32_t memReg = bridgeNub->configRead32(PCI_MEMORY_BASE);
        bridgeNub->setProperty("VMD-Cfg-Bridge-MEM-REG", memReg, 32);
        if (!vmdWindowCovers(memReg, memMin, memMax)) {
            vmdProgramWindow(bridgeNub, PCI_MEMORY_BASE, memMin, memMax);
            memReg = bridgeNub->configRead32(PCI_MEMORY_BASE);
            // "Fixed" only when the window really covers now — BARs above
            // 4 GB cannot be represented in the 32-bit window registers
            bridgeNub->setProperty("VMD-Cfg-Bridge-MEM-Fixed",
                                   vmdWindowCovers(memReg, memMin, memMax));
            if (!vmdWindowCovers(memReg, memMin, memMax))
                bridgeNub->setProperty("VMD-Cfg-Bridge-MEM-CannotFix", true);
        }
        bridgeNub->setProperty("VMD-Cfg-Bridge-MEM-Win-OK",
                               vmdWindowCovers(memReg, memMin, memMax));
        uint64_t wLo = ((uint64_t)(memReg & 0xFFF0)) << 16;
        uint64_t wHi = ((((uint64_t)(memReg >> 16)) & 0xFFF0) << 16) | 0xFFFFF;
        if (wLo <= wHi)
            vmdSyncWindowRange(bridge, bridgeNub,
                               kIOPCIRangeBridgeMemory, wLo, wHi + 1);
    }

    if (havePF) {
        bridgeNub->setProperty("VMD-Cfg-Sub-PF-Base",
                               (unsigned long long)pfMin, 64);
        bridgeNub->setProperty("VMD-Cfg-Sub-PF-End",
                               (unsigned long long)pfMax, 64);
        uint32_t pfReg = bridgeNub->configRead32(PCI_PREF_MEMORY_BASE);
        bridgeNub->setProperty("VMD-Cfg-Bridge-PF-REG", pfReg, 32);
        if (!vmdWindowCovers(pfReg, pfMin, pfMax)) {
            vmdProgramWindow(bridgeNub, PCI_PREF_MEMORY_BASE, pfMin, pfMax);
            // 64-bit pref window upper halves — VMD windows live below 4 GB
            bridgeNub->configWrite32(PCI_PREF_BASE_UPPER32, 0);
            bridgeNub->configWrite32(PCI_PREF_LIMIT_UPPER32, 0);
            pfReg = bridgeNub->configRead32(PCI_PREF_MEMORY_BASE);
            // "Fixed" only when the window really covers now — PF BARs
            // above 4 GB cannot be routed by this 32-bit window fix
            bridgeNub->setProperty("VMD-Cfg-Bridge-PF-Fixed",
                                   vmdWindowCovers(pfReg, pfMin, pfMax));
            if (!vmdWindowCovers(pfReg, pfMin, pfMax))
                bridgeNub->setProperty("VMD-Cfg-Bridge-PF-CannotFix", true);
        }
        bridgeNub->setProperty("VMD-Cfg-Bridge-PF-Win-OK",
                               vmdWindowCovers(pfReg, pfMin, pfMax));
        uint64_t wLo = ((uint64_t)(pfReg & 0xFFF0)) << 16;
        uint64_t wHi = ((((uint64_t)(pfReg >> 16)) & 0xFFF0) << 16) | 0xFFFFF;
        if (wLo <= wHi)
            vmdSyncWindowRange(bridge, bridgeNub,
                               kIOPCIRangeBridgePFMemory, wLo, wHi + 1);
    }

    // ---- I/O window: explicitly DISABLED, exactly like Linux ---------------
    //
    // A VMD domain has NO I/O port space at all:
    //   - linuxvmd.c vmd_enable_domain():1005 adds exactly three domain
    //     resources — IORESOURCE_BUS + MEMBAR1 + MEMBAR2 (both
    //     IORESOURCE_MEM).  Zero IORESOURCE_IO in the whole driver, so the
    //     PCI core can never assign a bridge I/O window ("BAR 13: no space
    //     for [io size 0x1000]" / "failed to assign"), and
    //     pci_setup_bridge_io() ends by writing the disabled encoding
    //     (base 0xF0 > limit 0x00, upper16 = 0) without printing anything.
    //   - The dmesg line "bridge window [io 0x0000-0x0fff]" is the
    //     READ-BACK of an unassigned window: pci_read_bridge_io() turns
    //     base=0/limit=0 into the range 0x0000-0x0fff and logs it during
    //     enumeration.  It is not an enabled window.
    //   - VMD silicon has no I/O aperture either — CFGBAR/MEMBAR1/MEMBAR2
    //     only.  Apple's IONVMeFamily does zero I/O-port accesses (BAR0
    //     MMIO + IODMACommand/PRP DMA; the decompile has no ioMap/ioRead/
    //     ioWrite references), and the NVMe endpoint itself declares no
    //     I/O BAR.  DMA never traverses I/O port space: it needs the
    //     bridge MEM window (fixed above) + COMMAND.BUS-MASTER (set in
    //     vmdPowerUpDecode).
    //
    // resetDomain() already wrote the Linux disabled encoding over the raw
    // ECAM, but the configurator entry's kIOPCIRangeBridgeIO was sized by
    // the scan from the PRE-reset BIOS values and nothing updated it — the
    // same stale-IOPCIConfigEntry class of bug as the mem windows.  Re-
    // assert the disabled encoding through the nub config path and empty
    // the entry range so no restore/reassign pass can resurrect a window
    // that points into the void.
    uint32_t ioPre = bridgeNub->configRead32(PCI_IO_BASE);
    bridgeNub->setProperty("VMD-Cfg-Bridge-IO-REG", ioPre & 0xFFFF, 32);
    bridgeNub->configWrite32(PCI_IO_BASE_UPPER16, 0x0000ffff);  // temp disable
    bridgeNub->configWrite16(PCI_IO_BASE, 0x00f0);              // base > limit
    bridgeNub->configWrite32(PCI_IO_BASE_UPPER16, 0);           // upper16 = 0
    bridgeNub->setProperty("VMD-Cfg-Bridge-IO-DIS", true);
    bridgeNub->setProperty("VMD-Cfg-Bridge-IO-Post",
                           bridgeNub->configRead32(PCI_IO_BASE) & 0xFFFF, 32);

    IOPCIRange *ioRange = bridge->ranges[kIOPCIRangeBridgeIO];
    if (ioRange) {
        ioRange->start        = 0;
        ioRange->end          = 0;
        ioRange->size         = 0;
        ioRange->totalSize    = 0;
        ioRange->extendSize   = 0;
        ioRange->proposedSize = 0;
        bridgeNub->setProperty("VMD-Cfg-Bridge-IO-Range-Cleared", true);
    } else {
        bridgeNub->setProperty("VMD-Cfg-Bridge-IO-Range-Missing", true);
    }
}


void VMDDriver::postScanConfigure(void)
{
    if (!fBridgeEntry) {

        fPCIDev->setProperty("VMD-Stage-PostScan-Fail", "no-bridge-entry");
        return;
    }
    fPCIDev->setProperty("VMD-Stage-PostScan-Start", true);

    // Endpoints below a root port are initialised asynchronously by
    // Apple's IOPCI2PCIBridge on that port.  Wait (bounded) until every nub
    // of the domain is config-accessible BEFORE the reset/rewrite pass, so
    // the whole pass sees real hardware.  With deferred registration no
    // client driver can be running on them meanwhile.
    if (fDemuxMode) {
        uint32_t w = waitDomainAccessible(3000);
        fPCIDev->setProperty("VMD-Domain-Attach-Wait-MS", w, 32);
    }

    bool anyBridge = false;
    for (IOPCIConfigEntry *child = fBridgeEntry->child; child;
         child = child->peer) {
        if (child->isBridge) { anyBridge = true; break; }
    }
    if (anyBridge) {
        resetDomain();
        IOSleep(100);
    }

    uint32_t configured = 0;
    for (IOPCIConfigEntry *child = fBridgeEntry->child; child;
         child = child->peer) {
        configureChildDevice(child, NULL, 0);
        configured++;
    }
    fPCIDev->setProperty("VMD-Stage-PostScan-Children", configured, 32);
}


IOReturn VMDDriver::setDevicePowerState(IOPCIDevice *device,
                                        unsigned long whatToDo)
{
    IOReturn ret = IOPCI2PCIBridge::setDevicePowerState(device, whatToDo);

    if (device && (device != fPCIDev) && fBridgeEntry
        && !(whatToDo & kIOPCIConfigShadowPermanent)) {
        device->setProperty("VMD-PM-Hook", (unsigned long long)whatToDo, 32);
        vmdReassertAll();
    }
    return ret;
}


void VMDDriver::vmdReassertAll(void)
{
    for (IOPCIConfigEntry *child = fBridgeEntry->child; child;
         child = child->peer)
        vmdReassertSubtree(child, 0);
}

void VMDDriver::vmdReassertSubtree(IOPCIConfigEntry *entry, int depth)
{
    if (!entry || depth > 4) return;
    IOPCIDevice *nub = vmdEntryNub(entry);
    if (!nub) return;

    UInt16 vid = nub->configRead16(kIOPCIConfigVendorID);
    if (vid == 0xFFFF) return;

    vmdPowerUpDecode(nub);

    bool isBridge = ((nub->configRead8(kIOPCIConfigHeaderType) & 0x7f)
                     == kPCIHeaderType1);
    if (isBridge) {
        vmdFixBridgeWindows(entry, nub);
        vmdSyncConfigEntry(nub, entry, true);
    } else {
        vmdSyncConfigEntry(nub, entry, false);
        // A power transition may have restored a stock (CPU-format)
        // MSI message; re-aim it at the child's VMD vector.
        VMDChildDevice *kid = OSDynamicCast(VMDChildDevice, nub);
        if (fDemuxMode && kid && kid->fDemux) composeChildMSI(nub);
    }
    nub->setProperty("VMD-PM-Reassert", depth, 32);

    for (IOPCIConfigEntry *c = entry->child; c; c = c->peer)
        vmdReassertSubtree(c, depth + 1);
}


void VMDDriver::configureChildDevice(IOPCIConfigEntry *entry,
                                     IOPCIDevice *parentNub, int depth)
{
    if (!entry || depth > 4) return;
    IOPCIDevice *nub = vmdEntryNub(entry);
    if (!nub) {
        // the entry exists but no nub was built for it (scan/creation
        // mismatch) — record it on the parent so the gap is visible.
        if (parentNub) {
            parentNub->setProperty("VMD-Cfg-NoNub", true);
            parentNub->setProperty("VMD-Cfg-NoNub-Space",
                                   (unsigned long long)entry->space.bits, 32);
        }
        return;
    }
    nub->setProperty("VMD-Cfg-Visited", depth, 32);

    // Nubs below a root port are attached asynchronously by Apple's
    // IOPCI2PCIBridge on that port (its probeBus -> publishNub).  Until then
    // IOPCIDevice::configAccess() fails (no parent): reads return all-ones,
    // writes and capability lookups are dropped.  Wait for the attach
    // (bounded) so the fix-ups below really reach the device.
    // postScanConfigure() already waited for the whole domain; this is the
    // per-node backstop.
    {
        VMDChildDevice *vkid = OSDynamicCast(VMDChildDevice, nub);
        if (vkid && !vkid->vmdConfigAccessible()) {
            uint32_t waited = 0;
            while (!vkid->vmdConfigAccessible() && waited < 1000) {
                IOSleep(10);
                waited += 10;
            }
            nub->setProperty("VMD-Cfg-Attach-Wait-MS", waited, 32);
            if (!vkid->vmdConfigAccessible()) {
                // Never run the fix-ups on an un-initialised nub (config
                // access would go nowhere).  A deferred endpoint stays
                // unregistered; irqStatTick() configures and releases it
                // once Apple's bridge has initialised it.
                nub->setProperty("VMD-Cfg-Attach-Timeout", true);
                if (vkid->fRegDeferred) {
                    vkid->fCfgEntry  = entry;
                    vkid->fCfgParent = parentNub;
                    vkid->fCfgDepth  = depth;
                    vkid->fCfgPending = true;
                }
                return;
            }
        }
    }

    // Dead in ECAM — skip the whole subtree.
    UInt16 vid = nub->configRead16(kIOPCIConfigCommand);
    if (vid == 0) {
        nub->setProperty("VMD-Cfg-Dead", true);
        return;
    }

    //nub->retain();
    //nub->open(this);
    
    // 1. ACPI companion (vmd_acpi_find_companion)
    associateACPI(entry, parentNub);

    // 2. vmd_pm_enable_quirk()
    enableLTRQuirk(nub);

    // 3. D0 + command decode
    vmdPowerUpDecode(nub);

    bool isBridge = ((nub->configRead8(kIOPCIConfigHeaderType) & 0x7f)
                     == kPCIHeaderType1);

    // 4. bridge windows over ALL downstream BARs
    if (isBridge) vmdFixBridgeWindows(entry, nub);

    // 5. IOPCIConfigEntry + save/restore shadow := fixed hardware
    vmdSyncConfigEntry(nub, entry, isBridge);

    // 6. child MSI wiring onto the VMD's own MSI-X vectors (irq demux)
    if (!isBridge) wireChildMSI(nub);

    // 7. endpoint behind a root port: link prep; NVMe: + controller probe
    if (!isBridge && parentNub) {
        endpointLinkPrep(parentNub, nub);
    }

    // 8. recurse — every child of this bridge gets the same treatment
    for (IOPCIConfigEntry *child = entry->child; child; child = child->peer)
        configureChildDevice(child, nub, depth + 1);
}


/* Opt-in link power management behind VMD.
 *
 * The BIOS leaves ASPM and LTR off on links behind VMD (both LNKCTL = 0x40
 * on the test laptop) and the driver clears ASPM explicitly during bring-up.  Linux
 * re-enables link PM for client VMD (vmd_pm_enable_quirk ->
 * pci_enable_link_state_locked(PCIE_LINK_STATE_ALL)) because otherwise the
 * package cannot reach deep C-states.
 *
 * boot-arg vmdaspm=<mask>; absent, 0, or not a number 0..7 = default
 * behaviour (one PE_parse_boot_argn and one setProperty, nothing written).
 * A bare "vmdaspm" (no '=') parses as 1.
 *   bit0  ASPM L1 on both partners, only when both LNKCAPs advertise L1 and
 *         the L1 exit latency fits the endpoint's DEVCAP L1 acceptable
 *         latency (Linux pcie_aspm_check_latency).  L0s is never enabled.
 *   bit1  endpoint Clock PM (LNKCTL.ECPM, CLKREQ#) when LNKCAP.CPM is set.
 *   bit2  (with bit0) LTR_EN on the path (root port first) and ASPM
 *         L1.1/L1.2 through the L1 PM Substates capability, programmed while
 *         ASPM L1 is off, in the order and with the T_POWER_ON /
 *         Common_Mode_Restore / LTR_L1.2_THRESHOLD values of Linux
 *         aspm_calc_l12_info() + pcie_config_aspm_l1ss().  L1.2 only with
 *         LTR on both ends; PCI-PM L1.x bits stay off.  Before LTR_EN
 *         the endpoint's LTR max snoop/no-snoop latency is set to the
 *         Linux vmd_pm_enable_quirk value when it still reads 0 (in case
 *         enableLTRQuirk() did not program it); without a usable LTR
 *         max, LTR_EN and L1.2 stay off.  bit0 without
 *         bit2 clears any L1.x enable the firmware left set (plain L1).
 *   vmdaspm=4 (bit2 alone) writes nothing: a read-only probe that
 *         publishes LNKCAP/DEVCAP/DEVCAP2/LNKCTL/L1SS/LTR/AER snapshots.
 * Every read-modify-write refuses an all-ones read.
 * Only an NVMe function directly below a Root Port is touched.  ASPM L1 is
 * disabled endpoint first and enabled root port first (PCIe 7.5.3.7).
 * Runs after the DLLLA wait and the NVMe bring-up probes.  Every value read
 * or written is published as VMD-LinkPM-* on the endpoint nub; snapshots
 * "Pre"/"Set" here and "Open"/"Close" when IONVMe opens/closes the nub
 * (opt-in only).  Extended registers go through the VMD's own config
 * accessors exactly like enableLTRQuirk(). */
static uint32_t gVmdLinkPMMask = 0;   // non-zero only once the opt-in acted

#define VMD_EXT_CAP_AER   0x0001
#define VMD_EXT_CAP_L1SS  0x001E
#define VMD_L1SS_CAP      0x04
#define VMD_L1SS_CTL1     0x08
#define VMD_L1SS_CTL2     0x0C

static IOPCIAddressSpace vmdExtSpace(IOPCIDevice *d, uint32_t pos)
{
    IOPCIAddressSpace es = d->space;
    es.bits &= ~0x0F000000;
    es.es.registerNumExtended = (pos >> 8) & 0xF;
    return es;
}

static uint32_t vmdExtRead32(VMDDriver *v, IOPCIDevice *d, uint32_t pos)
{
    return v->configRead32(vmdExtSpace(d, pos), (UInt8)(pos & 0xFF));
}

static void vmdExtWrite32(VMDDriver *v, IOPCIDevice *d, uint32_t pos, uint32_t val)
{
    v->configWrite32(vmdExtSpace(d, pos), (UInt8)(pos & 0xFF), val);
}

/* Walk only the extended chain (0x100..0xFFF).  VMDDriver's finder searches
 * the standard list first, where AER's extended ID 0x0001 would match the
 * standard PM capability ID 0x01. */
static uint32_t vmdExtFind(VMDDriver *v, IOPCIDevice *d, uint32_t id)
{
    uint32_t off = 0x100;
    for (int n = 0; off && n < 64; n++) {
        const uint32_t hdr = vmdExtRead32(v, d, off);
        if (hdr == 0 || hdr == 0xFFFFFFFFu) return 0;
        if ((hdr & 0xFFFF) == id) return off;
        off = (hdr >> 20) & 0xFFF;
        if (off < 0x100 || (off & 3)) return 0;
    }
    return 0;
}

/* Read-modify-write of a 16-bit standard register through the nub; refuses
 * an all-ones read so a failed access can never be written back as 0xFFFx
 * (on a Root Port LNKCTL that sets Link Disable + Retrain Link). */
static bool vmdRmw16(IOPCIDevice *d, uint32_t off, UInt16 clr, UInt16 set)
{
    const UInt16 cur = d->configRead16(off);
    if (cur == 0xFFFF) return false;
    d->configWrite16(off, (UInt16)((cur & ~clr) | set));
    return true;
}

static void vmdLinkPMSnapshot(VMDDriver *v, IOPCIDevice *rp, IOPCIDevice *ep, const char *tag)
{
    if (!v || !rp || !ep) return;
    char key[64];
    UInt8 rc = 0, ec = 0;
    if (rp->findPCICapability(kIOPCIPCIExpressCapability, &rc) && rc) {
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-LNKCTL", tag);
        ep->setProperty(key, (unsigned long long)rp->configRead16(rc + 0x10), 16);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-DEVCTL2", tag);
        ep->setProperty(key, (unsigned long long)rp->configRead16(rc + 0x28), 16);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-DEVCAP2", tag);
        ep->setProperty(key, (unsigned long long)rp->configRead32(rc + 0x24), 32);
    }
    if (ep->findPCICapability(kIOPCIPCIExpressCapability, &ec) && ec) {
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-LNKCTL", tag);
        ep->setProperty(key, (unsigned long long)ep->configRead16(ec + 0x10), 16);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-LNKSTA", tag);
        ep->setProperty(key, (unsigned long long)ep->configRead16(ec + 0x12), 16);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-DEVCTL2", tag);
        ep->setProperty(key, (unsigned long long)ep->configRead16(ec + 0x28), 16);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-DEVCAP2", tag);
        ep->setProperty(key, (unsigned long long)ep->configRead32(ec + 0x24), 32);
    }
    uint32_t a;
    if ((a = vmdExtFind(v, rp, VMD_EXT_CAP_AER))) {
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-AER-UNC", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, rp, a + 0x04), 32);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-AER-COR", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, rp, a + 0x10), 32);
    }
    if ((a = vmdExtFind(v, ep, VMD_EXT_CAP_AER))) {
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-AER-UNC", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, ep, a + 0x04), 32);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-AER-COR", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, ep, a + 0x10), 32);
    }
    if ((a = vmdExtFind(v, rp, VMD_EXT_CAP_L1SS))) {
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-L1SSCAP", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, rp, a + VMD_L1SS_CAP), 32);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-L1SSCTL1", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, rp, a + VMD_L1SS_CTL1), 32);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-RP-L1SSCTL2", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, rp, a + VMD_L1SS_CTL2), 32);
    }
    if ((a = vmdExtFind(v, ep, VMD_EXT_CAP_L1SS))) {
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-L1SSCAP", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, ep, a + VMD_L1SS_CAP), 32);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-L1SSCTL1", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, ep, a + VMD_L1SS_CTL1), 32);
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-L1SSCTL2", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, ep, a + VMD_L1SS_CTL2), 32);
    }
    if ((a = vmdExtFind(v, ep, 0x0018))) {           // LTR ext cap: max snoop/no-snoop
        snprintf(key, sizeof(key), "VMD-LinkPM-%s-EP-LTRMAX", tag);
        ep->setProperty(key, (unsigned long long)vmdExtRead32(v, ep, a + 0x04), 32);
    }
}

/* Linux calc_l12_pwron(): T_POWER_ON in us; scale 3 is invalid -> 0. */
static uint32_t vmdL12PwrOnUs(uint32_t cap)
{
    static const uint32_t unit[4] = { 2, 10, 100, 0 };
    return ((cap >> 19) & 0x1F) * unit[(cap >> 16) & 3];
}

/* Linux encode_l12_threshold(). */
static void vmdEncodeL12Th(uint32_t us, uint32_t *scale, uint32_t *value)
{
    static const uint64_t m[6] = { 1, 32, 1024, 32768, 1048576, 33554432 };
    const uint64_t ns = (uint64_t)us * 1000;
    for (uint32_t s = 0; s < 6; s++) {
        if (ns <= m[s] * 0x3FF) {
            *scale = s;
            *value = (uint32_t)((ns + m[s] - 1) / m[s]);
            return;
        }
    }
    *scale = 5;
    *value = 0x3FF;
}

/* bit2: called with ASPM L1 OFF on both partners.  Returns 4 if any ASPM
 * L1.x was enabled. */
static uint32_t vmdEnableL1SS(VMDDriver *v, IOPCIDevice *rp, IOPCIDevice *ep, UInt8 rc, UInt8 ec)
{
    const uint32_t rs = vmdExtFind(v, rp, VMD_EXT_CAP_L1SS);
    const uint32_t es = vmdExtFind(v, ep, VMD_EXT_CAP_L1SS);
    if (!rs || !es) {
        ep->setProperty("VMD-LinkPM-L1SS-Skipped", "no L1 PM Substates capability");
        return 0;
    }
    uint32_t rcap = vmdExtRead32(v, rp, rs + VMD_L1SS_CAP);
    uint32_t ecap = vmdExtRead32(v, ep, es + VMD_L1SS_CAP);
    ep->setProperty("VMD-LinkPM-RP-L1SSCAP", (unsigned long long)rcap, 32);
    ep->setProperty("VMD-LinkPM-EP-L1SSCAP", (unsigned long long)ecap, 32);
    if (rcap == 0xFFFFFFFFu || ecap == 0xFFFFFFFFu) {
        ep->setProperty("VMD-LinkPM-L1SS-Skipped", "config read all-ones");
        return 0;
    }
    if (!(rcap & 0x10)) rcap = 0;                    // L1 PM Substates Supported
    if (!(ecap & 0x10)) ecap = 0;

    // LTR on the whole path, root port first (Linux pci_configure_ltr).
    // DEVCAP2/DEVCTL2 exist from PCIe capability version 2 on (Apple's
    // IOPCIDevice::enableLTR checks expressV2 too); an all-ones read never
    // counts as "supported" or "enabled".
    const uint32_t rdc2 = ((rp->configRead16(rc + 0x02) & 0xF) >= 2) ? rp->configRead32(rc + 0x24) : 0;
    const uint32_t edc2 = ((ep->configRead16(ec + 0x02) & 0xF) >= 2) ? ep->configRead32(ec + 0x24) : 0;
    const bool ltrCap = (rdc2 != 0xFFFFFFFFu) && (edc2 != 0xFFFFFFFFu) &&
                        (rdc2 & (1u << 11)) && (edc2 & (1u << 11));   // DEVCAP2.LTR
    // The endpoint must carry a non-zero LTR max before it may send LTR: a 0
    // max means it reports zero latency tolerance, which keeps the link out of
    // L1.2 and the package out of deep states.  Program the Linux quirk value
    // (VMD_BIOS_PM_QUIRK_LTR, 3145728 ns, snoop and no-snoop) when it is 0.
    bool ltrMaxOK = false;
    if (ltrCap) {
        const uint32_t lp = vmdExtFind(v, ep, PCI_EXT_CAP_ID_LTR);
        if (lp) {
            const uint32_t lmask = PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK;
            uint32_t lm = vmdExtRead32(v, ep, lp + PCI_LTR_MAX_SNOOP_LAT);
            if (lm != 0xFFFFFFFFu && !(lm & lmask)) {
                vmdExtWrite32(v, ep, lp + PCI_LTR_MAX_SNOOP_LAT,
                              ((uint32_t)VMD_BIOS_PM_QUIRK_LTR << 16) | VMD_BIOS_PM_QUIRK_LTR);
                lm = vmdExtRead32(v, ep, lp + PCI_LTR_MAX_SNOOP_LAT);
                ep->setProperty("VMD-LinkPM-EP-LTRMAX-Set", (unsigned long long)lm, 32);
            }
            ltrMaxOK = (lm != 0xFFFFFFFFu) && (lm & lmask) && ((lm >> 16) & lmask);
        }
        if (!ltrMaxOK)
            ep->setProperty("VMD-LinkPM-L1SS-Note", "no usable endpoint LTR max: LTR_EN and L1.2 left off");
    }
    bool ltr = false;
    if (ltrCap && ltrMaxOK && vmdRmw16(rp, rc + 0x28, 0, 0x0400) && vmdRmw16(ep, ec + 0x28, 0, 0x0400)) {
        const UInt16 r2 = rp->configRead16(rc + 0x28), e2 = ep->configRead16(ec + 0x28);
        ltr = (r2 != 0xFFFF) && (e2 != 0xFFFF) && (r2 & 0x0400) && (e2 & 0x0400);
    }
    ep->setProperty("VMD-LinkPM-LTR-EN", ltr);
    if (!ltr) ecap &= ~0x4u;                         // ASPM L1.2 needs LTR (aspm_l1ss_init)

    const uint32_t en = rcap & ecap & 0xC;           // bit3 ASPM L1.1, bit2 ASPM L1.2
    if (!en) {
        ep->setProperty("VMD-LinkPM-L1SS-Skipped", "no common ASPM L1.x");
        return 0;
    }
    // Every read-modify-write below starts from a sane control register.
    if (vmdExtRead32(v, ep, es + VMD_L1SS_CTL1) == 0xFFFFFFFFu ||
        vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1) == 0xFFFFFFFFu ||
        vmdExtRead32(v, ep, es + VMD_L1SS_CTL2) == 0xFFFFFFFFu ||
        vmdExtRead32(v, rp, rs + VMD_L1SS_CTL2) == 0xFFFFFFFFu) {
        ep->setProperty("VMD-LinkPM-L1SS-Skipped", "L1SS control read all-ones");
        return 0;
    }

    // All L1.x off: child first, then parent (pcie_config_aspm_l1ss).
    vmdExtWrite32(v, ep, es + VMD_L1SS_CTL1, vmdExtRead32(v, ep, es + VMD_L1SS_CTL1) & ~0xFu);
    vmdExtWrite32(v, rp, rs + VMD_L1SS_CTL1, vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1) & ~0xFu);

    if (en & 0x4) {                                  // aspm_calc_l12_info()
        const uint32_t rcm = (rcap >> 8) & 0xFF, ecm = (ecap >> 8) & 0xFF;
        const uint32_t cm = (rcm > ecm) ? rcm : ecm;
        const uint32_t rpo = vmdL12PwrOnUs(rcap), epo = vmdL12PwrOnUs(ecap);
        const uint32_t pcap = (rpo > epo) ? rcap : ecap;
        const uint32_t tpo = (rpo > epo) ? rpo : epo;
        const uint32_t ctl2 = (((pcap >> 19) & 0x1F) << 3) | ((pcap >> 16) & 3);
        uint32_t sc = 0, val = 0;
        vmdEncodeL12Th(2 + 4 + cm + tpo, &sc, &val);
        const uint32_t th = (sc << 29) | (val << 16);
        // T_POWER_ON in both ports
        vmdExtWrite32(v, rp, rs + VMD_L1SS_CTL2, (vmdExtRead32(v, rp, rs + VMD_L1SS_CTL2) & ~0xFBu) | ctl2);
        vmdExtWrite32(v, ep, es + VMD_L1SS_CTL2, (vmdExtRead32(v, ep, es + VMD_L1SS_CTL2) & ~0xFBu) | ctl2);
        // Common_Mode_Restore_Time in the Downstream Port (root port) only
        vmdExtWrite32(v, rp, rs + VMD_L1SS_CTL1, (vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1) & ~0x0000FF00u) | (cm << 8));
        // LTR_L1.2_THRESHOLD in both ports
        vmdExtWrite32(v, rp, rs + VMD_L1SS_CTL1, (vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1) & ~0xE3FF0000u) | th);
        vmdExtWrite32(v, ep, es + VMD_L1SS_CTL1, (vmdExtRead32(v, ep, es + VMD_L1SS_CTL1) & ~0xE3FF0000u) | th);
        ep->setProperty("VMD-LinkPM-L12-ThresholdUs", (unsigned long long)(2 + 4 + cm + tpo), 32);
    }

    // Enable: parent first, then child.
    vmdExtWrite32(v, rp, rs + VMD_L1SS_CTL1, vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1) | en);
    vmdExtWrite32(v, ep, es + VMD_L1SS_CTL1, vmdExtRead32(v, ep, es + VMD_L1SS_CTL1) | en);
    ep->setProperty("VMD-LinkPM-L1SS-Enabled", (unsigned long long)en, 32);
    ep->setProperty("VMD-LinkPM-RP-L1SSCTL1", (unsigned long long)vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1), 32);
    ep->setProperty("VMD-LinkPM-RP-L1SSCTL2", (unsigned long long)vmdExtRead32(v, rp, rs + VMD_L1SS_CTL2), 32);
    ep->setProperty("VMD-LinkPM-EP-L1SSCTL1", (unsigned long long)vmdExtRead32(v, ep, es + VMD_L1SS_CTL1), 32);
    ep->setProperty("VMD-LinkPM-EP-L1SSCTL2", (unsigned long long)vmdExtRead32(v, ep, es + VMD_L1SS_CTL2), 32);
    return 4;
}

/* bit0 without bit2 = plain ASPM L1.  Called with ASPM L1 OFF on both
 * partners: clear any L1.x enable firmware left set (child first, then
 * parent, as pcie_config_aspm_l1ss), so L1.1/L1.2 never run with unvalidated
 * timing or without LTR_EN.  Same meaning IOPCIFamily gives
 * pci-aspm-default = 2 (setDeviceL1PMBits zeroes L1PM Control 1/2). */
static void vmdClearL1SS(VMDDriver *v, IOPCIDevice *rp, IOPCIDevice *ep)
{
    const uint32_t rs = vmdExtFind(v, rp, VMD_EXT_CAP_L1SS);
    const uint32_t es = vmdExtFind(v, ep, VMD_EXT_CAP_L1SS);
    if (!rs || !es) return;                           // L1.x needs both ends
    const uint32_t e1 = vmdExtRead32(v, ep, es + VMD_L1SS_CTL1);
    const uint32_t r1 = vmdExtRead32(v, rp, rs + VMD_L1SS_CTL1);
    if (e1 == 0xFFFFFFFFu || r1 == 0xFFFFFFFFu) return;
    if (!((e1 | r1) & 0xFu)) return;                  // nothing enabled: no write
    vmdExtWrite32(v, ep, es + VMD_L1SS_CTL1, e1 & ~0xFu);
    vmdExtWrite32(v, rp, rs + VMD_L1SS_CTL1, r1 & ~0xFu);
    ep->setProperty("VMD-LinkPM-L1SS-Cleared",
                    (unsigned long long)(((r1 & 0xFu) << 4) | (e1 & 0xFu)), 32);
}

static void vmdLinkPMOptIn(VMDDriver *v, IOPCIDevice *rp, IOPCIDevice *ep)
{
    uint32_t raw = 0;
    const bool found = PE_parse_boot_argn("vmdaspm", &raw, sizeof(raw));
    // A non-numeric value ("off", "on") comes back as up to 3 raw chars with
    // found == true; only 0..7 is accepted.
    const uint32_t mask = (found && raw <= 7) ? raw : 0;
    ep->setProperty("VMD-LinkPM-Requested", (unsigned long long)mask, 32);
    if (!mask) {
        if (found && raw > 7)
            ep->setProperty("VMD-LinkPM-BadArg", (unsigned long long)raw, 32);
        return;
    }

    UInt8 rc = 0, ec = 0;
    if (!v || !rp || !rp->findPCICapability(kIOPCIPCIExpressCapability, &rc) || !rc ||
        !ep->findPCICapability(kIOPCIPCIExpressCapability, &ec) || !ec) {
        ep->setProperty("VMD-LinkPM-Skipped", "no PCIe capability");
        return;
    }
    // NVMe function (class 0x010802) directly below a Root Port only.
    if ((ep->configRead32(kIOPCIConfigRevisionID) >> 8) != 0x010802 ||
        ((rp->configRead16(rc + 0x02) >> 4) & 0xF) != 0x4) {
        ep->setProperty("VMD-LinkPM-Skipped", "not NVMe below a Root Port");
        return;
    }
    gVmdLinkPMMask = mask;
    vmdLinkPMSnapshot(v, rp, ep, "Pre");

    const uint32_t rcap = rp->configRead32(rc + 0x0C);   // LNKCAP
    const uint32_t ecap = ep->configRead32(ec + 0x0C);
    const uint32_t edevcap = ep->configRead32(ec + 0x04); // DEVCAP
    const UInt16 rctl = rp->configRead16(rc + 0x10);      // LNKCTL
    const UInt16 ectl = ep->configRead16(ec + 0x10);
    ep->setProperty("VMD-LinkPM-RP-LNKCAP", (unsigned long long)rcap, 32);
    ep->setProperty("VMD-LinkPM-EP-LNKCAP", (unsigned long long)ecap, 32);
    ep->setProperty("VMD-LinkPM-EP-DEVCAP", (unsigned long long)edevcap, 32);
    ep->setProperty("VMD-LinkPM-RP-LNKCTL-Old", (unsigned long long)rctl, 16);
    ep->setProperty("VMD-LinkPM-EP-LNKCTL-Old", (unsigned long long)ectl, 16);
    if (rcap == 0xFFFFFFFFu || ecap == 0xFFFFFFFFu || edevcap == 0xFFFFFFFFu ||
        rctl == 0xFFFF || ectl == 0xFFFF) {
        ep->setProperty("VMD-LinkPM-Skipped", "config read all-ones");
        return;
    }

    // Linux pcie_aspm_check_latency(): the larger L1 exit latency (LNKCAP
    // [17:15], 2^n us) must not exceed the endpoint's DEVCAP L1 acceptable
    // latency ([11:9], 2^n us, 7 = no limit).  Direct RP link: no switch term.
    const uint32_t accL1 = (edevcap >> 9) & 7;
    const uint32_t rEx = (rcap >> 15) & 7, eEx = (ecap >> 15) & 7;
    const uint32_t exL1 = (rEx > eEx) ? rEx : eEx;
    const bool l1LatOK = (accL1 == 7) || (exL1 <= accL1);
    const bool l1Cap = (rcap & (1u << 11)) && (ecap & (1u << 11));   // ASPM Support L1

    uint32_t applied = 0;
    if (mask & 1) {
        if (!l1Cap) {
            ep->setProperty("VMD-LinkPM-Skipped", "L1 not supported by both ports");
        } else if (!l1LatOK) {
            ep->setProperty("VMD-LinkPM-Skipped", "L1 exit latency > EP acceptable");
        } else if (!vmdRmw16(ep, ec + 0x10, 0x0003, 0) ||     // ASPM L1 off: endpoint first,
                   !vmdRmw16(rp, rc + 0x10, 0x0003, 0)) {     // then root port
            ep->setProperty("VMD-LinkPM-Skipped", "LNKCTL read all-ones");
        } else {
            if (mask & 4)
                applied |= vmdEnableL1SS(v, rp, ep, rc, ec);
            else
                vmdClearL1SS(v, rp, ep);                      // bit0 = plain L1
            // ASPM L1 on: root port first, then endpoint.
            if (vmdRmw16(rp, rc + 0x10, 0x0003, 0x0002) &&
                vmdRmw16(ep, ec + 0x10, 0x0003, 0x0002)) {
                applied |= 1;
                // keep IOPCIFamily's ASPM view in step with the hardware (L1)
                uint32_t l1 = 2;
                OSData *d = OSData::withBytes(&l1, sizeof(l1));
                if (d) {
                    ep->setProperty(kIOPCIExpressASPMDefaultKey, d);
                    d->release();
                }
                ep->setProperty("VMD-Dbg-ASPM-Prop", (unsigned long long)2, 32);
            }
        }
    }
    // LNKCAP bit 18 = Clock Power Management; LNKCTL bit 8 = ECPM.
    if ((mask & 2) && (ecap & (1u << 18)) && vmdRmw16(ep, ec + 0x10, 0, 0x0100))
        applied |= 2;
    ep->setProperty("VMD-LinkPM-Applied", (unsigned long long)applied, 32);
    vmdLinkPMSnapshot(v, rp, ep, "Set");
}

void VMDDriver::endpointLinkPrep(IOPCIDevice *rp, IOPCIDevice *nub2)
{
    if (!rp || !nub2) return;

    uint32_t aspmOff = 0;
    OSData *aspmProp = OSData::withBytes(&aspmOff, sizeof(aspmOff));
    if (aspmProp) {
        nub2->setProperty(kIOPCIExpressASPMDefaultKey, aspmProp);
        aspmProp->release();
    }
    nub2->setProperty("VMD-Dbg-ASPM-Key", "pci-aspm-default");
    nub2->setProperty("VMD-Dbg-ASPM-Prop", (unsigned long long)0, 32);

    // Root-port (downstream port) side of the same link: ASPM Control lives
    // in BOTH partners; IOPCI2PCIBridge::setDeviceASPMState() always
    // programs both and NVMeFix likewise walks SSD + bridge.
    UInt8 rpcie = 0;
    if (rp->findPCICapability(kIOPCIPCIExpressCapability, &rpcie) && rpcie) {
        UInt16 rplctl = rp->configRead16(rpcie + 0x10);
        nub2->setProperty("VMD-Dbg-RP-LNKCTL", rplctl, 16);
        if (rplctl & 0x0003) {
            rp->configWrite16(rpcie + 0x10, rplctl & ~0x0003);
            nub2->setProperty("VMD-Dbg-RP-ASPM-Cleared", true);
            nub2->setProperty("VMD-Dbg-RP-LNKCTL-New",
                              rp->configRead16(rpcie + 0x10), 16);
        }
    }

    // Endpoint side of the link + DLLLA wait.
    UInt8 pcie = 0;
    if (nub2->findPCICapability(kIOPCIPCIExpressCapability, &pcie) && pcie) {
        UInt16 lctl = nub2->configRead16(pcie + 0x10);
        nub2->setProperty("VMD-Dbg-EP-LNKCTL", lctl, 16);
        if (lctl & 3) {
            nub2->configWrite16(pcie + 0x10, lctl & ~0x0003);
            nub2->setProperty("VMD-Dbg-EP-ASPM-Cleared", true);
            nub2->setProperty("VMD-Dbg-EP-LNKCTL-New",
                              nub2->configRead16(pcie + 0x10), 16);
            IOSleep(1);
        }

        for (int i = 0; i < 100; i++) {
            IODelay(100);
            if (nub2->configRead16(pcie + 0x12) & 0x2000)   // LSTS.DLLLA
                break;
        }

        IODelay(10000);
    }

    vmdBridgeCheck(rp, nub2);

    appleNVMeInitDebug(nub2);

    vmdLinkPMOptIn(this, rp, nub2);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Bridge-window helpers (vmdBridgeCheck).
 *
 * PCI-to-PCI bridge spec: the memory window is 1 MB granular — the base
 * register's bits 15:4 carry address bits 31:20, the limit register is
 * inclusive.  resetDomain() writes PCI_MEMORY_BASE = 0x0000fff0, i.e. base
 * field 0xFFF0 ("disabled": base > limit), so a window must be reprogrammed
 * before any endpoint MMIO works.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
static bool vmdWindowCovers(uint32_t reg32, uint64_t lo, uint64_t hi)
{
    uint64_t wLo = ((uint64_t)(reg32 & 0xFFF0)) << 16;
    uint64_t wHi = ((((uint64_t)(reg32 >> 16)) & 0xFFF0) << 16) | 0xFFFFF;
    return (wLo <= wHi) && (lo >= wLo) && (hi <= wHi);
}

static void vmdProgramWindow(IOPCIDevice *rp, uint8_t offset,
                             uint64_t lo, uint64_t hi)
{
    uint64_t aLo = lo & ~0xFFFFFULL;                  // 1 MB align down
    uint64_t aHi = (hi | 0xFFFFFULL) + 1;             // 1 MB align up, exclusive
    if (aHi > 0x100000000ULL) aHi = 0x100000000ULL;   // window regs are 32-bit
    if (aHi <= aLo) return;
    uint32_t baseReg = (uint32_t)((aLo >> 16) & 0xFFF0);
    uint32_t limReg  = (uint32_t)(((aHi - 0x100000ULL) >> 16) & 0xFFF0);
    rp->configWrite32(offset, baseReg | (limReg << 16));
    IOSleep(2);
}

/* CAP = BAR0+0x00 as two 32-bit little-endian reads (see appleNVMeInitDebug). */
static inline uint64_t vmdReadCAP(volatile UInt8 *mmio)
{
    return (uint64_t)OSReadLittleInt32(mmio, NVME_REG_CAP)
         | ((uint64_t)OSReadLittleInt32(mmio, NVME_REG_CAP + 4) << 32);
}

void VMDDriver::vmdBridgeCheck(IOPCIDevice *rp, IOPCIDevice *nub2)
{
    if (!rp || !nub2) return;

    // ---- endpoint identity (ECAM sanity) ----------------------------------
    UInt16 vid = nub2->configRead16(kIOPCIConfigVendorID);
    UInt16 did = nub2->configRead16(kIOPCIConfigDeviceID);
    nub2->setProperty("VMD-Dbg-DEV-VID", vid, 16);
    nub2->setProperty("VMD-Dbg-DEV-DID", did, 16);
    if (vid == 0xFFFF) {
        // ECAM itself is not answering for this device — nothing to fix.
        nub2->setProperty("VMD-Dbg-RP-Cfg-Dead", true);
        return;
    }


    uint64_t memMin = ~0ULL, memMax = 0, pfMin = ~0ULL, pfMax = 0;
    uint64_t bar0Cfg = 0;
    bool haveMem = false, havePF = false;

    // BAR0 straight from config space.  "IODeviceMemory" only exists
    // once the nub has been matched (getResources), and with deferred
    // registration it is still absent here.
    {
        uint32_t lo0 = nub2->configRead32(kIOPCIConfigBaseAddress0);
        uint32_t hi0 = (((lo0 >> 1) & 0x3) == 0x2)
                     ? nub2->configRead32(kIOPCIConfigBaseAddress0 + 4) : 0;
        bar0Cfg = ((uint64_t)hi0 << 32) | lo0;
    }

    OSArray *deviceMemory = nub2->getDeviceMemory();
    for (uint32_t idx = 0; deviceMemory; idx++) {
        IOMemoryDescriptor *mem =
            (IOMemoryDescriptor *)deviceMemory->getObject(idx);
        if (!mem) break;
        IOPCIAddressSpace tag;
        tag.bits = mem->getTag();
        if (tag.s.registerNum < kIOPCIConfigBaseAddress0) continue;
        uint32_t barIdx = (tag.s.registerNum - kIOPCIConfigBaseAddress0) / 4;
        if (barIdx >= 6) continue;

        uint32_t lo = nub2->configRead32(kIOPCIConfigBaseAddress0 + 4 * barIdx);
        if (barIdx == 0) {
            uint32_t hi = nub2->configRead32(kIOPCIConfigBaseAddress0 + 4);
            bar0Cfg = ((uint64_t)hi << 32) | lo;
        }
        if (lo & 0x1) continue;                    // I/O BAR — not our case
        uint64_t base = mem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
        uint64_t len  = mem->getLength();
        if (!base || !len) continue;
        uint64_t end  = base + len - 1;
        if (lo & 0x8) {                            // prefetchable
            pfMin = (base < pfMin) ? base : pfMin;
            pfMax = (end  > pfMax) ? end  : pfMax;
            havePF = true;
        } else {
            memMin = (base < memMin) ? base : memMin;
            memMax = (end  > memMax) ? end  : memMax;
            haveMem = true;
        }
    }
    nub2->setProperty("VMD-Dbg-BAR0-CFG", (unsigned long long)bar0Cfg, 64);

    // No device-memory entries: fall back to the raw BAR0 config value so
    // the window fix still has something to route to.
    if (!haveMem && !havePF && bar0Cfg && !(bar0Cfg & 0x1)) {
        uint64_t base = bar0Cfg & ~0xFULL;
        // Cover the whole BAR (configurator entry size), not 1 byte
        uint64_t len = 0;
        IOPCIConfigEntry *be = VMDChildDevice::configEntryOf(nub2);
        if (be && be->ranges[kIOPCIRangeBAR0]) len = be->ranges[kIOPCIRangeBAR0]->size;
        uint64_t end = base + (len ? len - 1 : 0);
        if (bar0Cfg & 0x8) { pfMin = base; pfMax = end; havePF = true; }
        else               { memMin = base; memMax = end; haveMem = true; }
        nub2->setProperty("VMD-Dbg-BAR0-FromCfg", true);
    }

    // ---- endpoint command: MEM decode must be on for MMIO to answer -------
    UInt16 devCmd = nub2->configRead16(kIOPCIConfigCommand);
    nub2->setProperty("VMD-Dbg-DEV-CMD", devCmd, 16);
    if ((devCmd & PCI_COMMAND_MEMORY) == 0) {
        nub2->configWrite16(kIOPCIConfigCommand,
                            devCmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        IOSleep(2);
        nub2->setProperty("VMD-Dbg-DEV-MEM-Fixed", true);
    }

    // ---- VMD device command (top of the decode chain) ---------------------
    UInt16 vmdCmd = fPCIDev->configRead16(kIOPCIConfigCommand);
    nub2->setProperty("VMD-Dbg-VMD-CMD", vmdCmd, 16);
    if ((vmdCmd & PCI_COMMAND_MEMORY) == 0) {
        fPCIDev->configWrite16(kIOPCIConfigCommand,
                               vmdCmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        IOSleep(2);
        nub2->setProperty("VMD-Dbg-VMD-MEM-Fixed", true);
    }

    // ---- root-port command ------------------------------------------------
    UInt16 rpCmd = rp->configRead16(kIOPCIConfigCommand);
    nub2->setProperty("VMD-Dbg-RP-CMD", rpCmd, 16);
    if ((rpCmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
                != (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) {
        rp->configWrite16(kIOPCIConfigCommand,
                          rpCmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        IOSleep(2);
        nub2->setProperty("VMD-Dbg-RP-MEM-Fixed", true);
    }

    // ---- root-port memory window ------------------------------------------
    uint32_t memReg = rp->configRead32(PCI_MEMORY_BASE);
    nub2->setProperty("VMD-Dbg-RP-MEM-REG", memReg, 32);
    bool memOK = !haveMem || vmdWindowCovers(memReg, memMin, memMax);
    nub2->setProperty("VMD-Dbg-RP-MEM-Win-OK", memOK);
    if (haveMem && !memOK) {
        vmdProgramWindow(rp, PCI_MEMORY_BASE, memMin, memMax);
        nub2->setProperty("VMD-Dbg-RP-MEM-Win-Fixed", true);
        memReg = rp->configRead32(PCI_MEMORY_BASE);
    }
    if (haveMem) {
        nub2->setProperty("VMD-Dbg-RP-MEM-BASE",
            (unsigned long long)(((uint64_t)(memReg & 0xFFF0)) << 16), 64);
        nub2->setProperty("VMD-Dbg-RP-MEM-LIMIT",
            (unsigned long long)(((((uint64_t)(memReg >> 16)) & 0xFFF0) << 16)
                                 | 0xFFFFF), 64);
    }

    // ---- root-port prefetchable window ------------------------------------
    uint32_t pfReg = rp->configRead32(PCI_PREF_MEMORY_BASE);
    nub2->setProperty("VMD-Dbg-RP-PF-REG", pfReg, 32);
    bool pfOK = !havePF || vmdWindowCovers(pfReg, pfMin, pfMax);
    nub2->setProperty("VMD-Dbg-RP-PF-Win-OK", pfOK);
    if (havePF && !pfOK) {
        vmdProgramWindow(rp, PCI_PREF_MEMORY_BASE, pfMin, pfMax);
        // 64-bit pref window upper halves — VMD windows live below 4 GB
        rp->configWrite32(PCI_PREF_BASE_UPPER32, 0);
        rp->configWrite32(PCI_PREF_LIMIT_UPPER32, 0);
        nub2->setProperty("VMD-Dbg-RP-PF-Win-Fixed", true);
        pfReg = rp->configRead32(PCI_PREF_MEMORY_BASE);
    }
    if (havePF) {
        nub2->setProperty("VMD-Dbg-RP-PF-BASE",
            (unsigned long long)(((uint64_t)(pfReg & 0xFFF0)) << 16), 64);
        nub2->setProperty("VMD-Dbg-RP-PF-LIMIT",
            (unsigned long long)(((((uint64_t)(pfReg >> 16)) & 0xFFF0) << 16)
                                 | 0xFFFFF), 64);
    }

    // ---- secondary status: master-abort history ---------------------------
    UInt16 secsts = rp->configRead16(PCI_SEC_STATUS);
    nub2->setProperty("VMD-Dbg-RP-SECSTS", secsts, 16);
    if (secsts & (PCI_SEC_STATUS_RMA | PCI_SEC_STATUS_RTA)) {
        nub2->setProperty("VMD-Dbg-RP-Abort-History", true);
        rp->configWrite16(PCI_SEC_STATUS, secsts);   // write-1-to-clear
    }

    // ---- link status on both sides of the link ----------------------------
    UInt8 rpPcie = 0, epPcie = 0;
    if (rp->findPCICapability(kIOPCIPCIExpressCapability, &rpPcie) && rpPcie) {
        UInt16 lsts = rp->configRead16(rpPcie + 0x12);
        nub2->setProperty("VMD-Dbg-RP-LSTS", lsts, 16);
        nub2->setProperty("VMD-Dbg-RP-DLLLA", (lsts & PCI_EXP_LNKSTA_DLLLA) != 0);
    }
    if (nub2->findPCICapability(kIOPCIPCIExpressCapability, &epPcie) && epPcie) {
        UInt16 lsts = nub2->configRead16(epPcie + 0x12);
        nub2->setProperty("VMD-Dbg-EP-LSTS", lsts, 16);
        nub2->setProperty("VMD-Dbg-EP-DLLLA", (lsts & PCI_EXP_LNKSTA_DLLLA) != 0);
    }
}



/* Map BAR0 straight from its live physical address.  With deferred
 * registration the nub has not been matched yet, so IOPCIDevice::
 * getResources() has not built "IODeviceMemory" and
 * mapDeviceMemoryWithRegister() returns NULL. */
static IOMemoryMap * vmdMapBar0Phys(IOPCIDevice *nub)
{
    uint32_t lo = nub->configRead32(kIOPCIConfigBaseAddress0);
    if (lo == 0xFFFFFFFFU || (lo & 0x1)) return NULL;       // dead / I/O BAR
    uint32_t hi = (((lo >> 1) & 0x3) == 0x2)
                ? nub->configRead32(kIOPCIConfigBaseAddress1) : 0;
    uint64_t base = (((uint64_t)hi << 32) | lo) & ~0xFULL;
    if (!base) return NULL;

    uint64_t len = 0;
    IOPCIConfigEntry *entry = VMDChildDevice::configEntryOf(nub);
    if (entry && entry->ranges[kIOPCIRangeBAR0])
        len = entry->ranges[kIOPCIRangeBAR0]->size;
    if (!len || len > 0x100000) len = 0x4000;               // NVMe regs + doorbells

    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
        (IOPhysicalAddress)base, (IOByteCount)len, kIODirectionInOut);
    if (!md) return NULL;
    IOMemoryMap *map = md->map(kIOMapInhibitCache);
    md->release();
    if (map) nub->setProperty("VMD-Dbg-BAR0-PhysMap", true);
    return map;
}

void VMDDriver::appleNVMeInitDebug(IOPCIDevice *nub2)
{
    if (!nub2) return;

    // ---- config-space context (InitializePCI precondition) ---------------
    UInt16 cmdReg = nub2->configRead16(kIOPCIConfigCommand);
    nub2->setProperty("VMD-Dbg-CMD", (unsigned long long)cmdReg, 16);
    nub2->setProperty("VMD-Dbg-MEM-Decode", (cmdReg & 0x0002) != 0);

    VMDChildDevice *child = OSDynamicCast(VMDChildDevice, nub2);
    bool stashed = false;

    // ---- InitializePCI(): map BAR0 the way Apple does ---------------------
    // Reuse the mapping stabilizeNVMeForCAP() stashed on the nub when it
    // ran; only map here if there is none yet.
    IOMemoryMap *bar0 = NULL;
    if (child && child->rmmio) {
        bar0 = child->rmmio;
        stashed = true;
    } else {
        bar0 = nub2->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
                                                 kIOMapInhibitCache
                                                 | kIOMapAnywhere);
        if (!bar0) bar0 = vmdMapBar0Phys(nub2);   // not matched yet
    }
    if (!bar0) {
        // InitializePCI() would fail: no map -> "( 0 != fBaseAddress )"
        nub2->setProperty("VMD-Dbg-BAR0-Valid", false);
        nub2->setProperty("VMD-Dbg-APL-InitOK", false);
        return;
    }

    nub2->setProperty("VMD-Dbg-BAR0-Phys",
                      (unsigned long long)bar0->getPhysicalAddress(), 64);
    nub2->setProperty("VMD-Dbg-BAR0-Size",
                      (unsigned long long)bar0->getLength(), 64);

    volatile UInt8 *mmio = (volatile UInt8 *)bar0->getVirtualAddress();
    if (!mmio) {
        if (!stashed) bar0->release();
        return;
    }


    // Class first; only an NVMe BAR0 is kept for the CSTS/CC
    // trace readers (timer, handleOpen/handleClose)
    uint32_t cls = nub2->configRead32(kIOPCIConfigRevisionID) >> 16;

    if (child && !child->rmmio && cls == VMD_CLASS_NVME) {
        child->rmmio    = bar0;
        child->rmmioPtr = (volatile UInt32 *)mmio;
        stashed = true;
    }

    if (cls == VMD_CLASS_NVME)
    {
        
        // ---- SetNVMeProperties(): CAP = ReadRegister64(0) ---------------------
        // Two 32-bit little-endian reads are equivalent to Apple's 64-bit read
        // on little-endian x86 and avoid any 64-bit MMIO quirk; an unrouted MMIO
        // path returns all-ones either way.
        uint64_t cap = vmdReadCAP(mmio);
        
        // VS (0x08), CC (0x14), CSTS (0x1C) — read-only samples.
        uint32_t vs   = OSReadLittleInt32(mmio, NVME_REG_VS);
        uint32_t cc   = OSReadLittleInt32(mmio, NVME_REG_CC);
        uint32_t csts = OSReadLittleInt32(mmio, NVME_REG_CSTS);
        
        uint32_t mqes   = (uint32_t)(cap & kNVMe_CAP_MQES_Mask);
        uint32_t cqr    = (uint32_t)((cap & kNVMe_CAP_CQR_Mask)  >> 16);
        uint32_t ams    = (uint32_t)((cap & kNVMe_CAP_AMS_Mask)  >> 17);
        uint32_t to     = (uint32_t)((cap & kNVMe_CAP_TO_Mask)   >> 24);
        uint32_t dstrd  = (uint32_t)((cap & kNVMe_CAP_DSTRD_Mask) >> 32);
        uint32_t nssr   = (uint32_t)((cap & kNVMe_CAP_NSSR_Mask)  >> 36);
        uint32_t cssNVM = (uint32_t)((cap & kNVMe_CAP_CommandSetSupported_NVM_Mask) >> 37);
        uint32_t mpsmin = (uint32_t)((cap & kNVMe_CAP_MPS_Min_Mask) >> 48);
        uint32_t mpsmax = (uint32_t)((cap & kNVMe_CAP_MPS_Max_Mask) >> 52);
        
        nub2->setProperty("VMD-Dbg-CAP",     (unsigned long long)cap,  64);
        nub2->setProperty("VMD-Dbg-VS",      (unsigned long long)vs,   32);
        nub2->setProperty("VMD-Dbg-CC",      (unsigned long long)cc,   32);
        nub2->setProperty("VMD-Dbg-CSTS",    (unsigned long long)csts, 32);
        nub2->setProperty("VMD-Dbg-MQES",    (unsigned long long)mqes,   32);
        nub2->setProperty("VMD-Dbg-CQR",     (unsigned long long)cqr,    32);
        nub2->setProperty("VMD-Dbg-AMS",     (unsigned long long)ams,    32);
        nub2->setProperty("VMD-Dbg-TO",      (unsigned long long)to,     32);
        nub2->setProperty("VMD-Dbg-STRIDE",  (unsigned long long)dstrd,  32);
        nub2->setProperty("VMD-Dbg-NSSR",    (unsigned long long)nssr,   32);
        nub2->setProperty("VMD-Dbg-CSS-NVM", (unsigned long long)cssNVM, 32);
        nub2->setProperty("VMD-Dbg-MPSMIN",  (unsigned long long)mpsmin, 32);
        nub2->setProperty("VMD-Dbg-MPSMAX",  (unsigned long long)mpsmax, 32);
        nub2->setProperty("VMD-Dbg-CSTS-RDY", (csts & kNVMe_CSTS_RDY_Mask) != 0);
        
        // ---- Apple's four init guards, one registry flag each ----------------
        bool capOK = (cap != kNVMeInvalidRegisterValue64);                    // :0x4d8
        bool cssOK = (cap & kNVMe_CAP_CommandSetSupported_NVM_Mask) != 0;     // :0x4dd
        bool mpsOK = (cap & kNVMe_CAP_MPS_Min_Mask) == 0;                     // :0x4e0
        bool vsOK  = (vs   != kNVMeInvalidRegisterValue32);                   // :0x501
        
        nub2->setProperty("VMD-Dbg-APL-CAP-OK",     capOK);
        nub2->setProperty("VMD-Dbg-APL-CSS-NVM-OK", cssOK);
        nub2->setProperty("VMD-Dbg-APL-MPSMIN-OK",  mpsOK);
        nub2->setProperty("VMD-Dbg-APL-VS-OK",      vsOK);
        
        // Apple-derived init parameters (SetNVMeProperties() +
        // GetDefaultInitTimeout(): (CAP >> 24 & 0xFF) * 500 ms).
        nub2->setProperty("VMD-Dbg-APL-InitTimeoutMS",
                          (unsigned long long)to * 500, 32);
        
        if (vsOK) {
            // Apple publishes "NVMe Revision Supported" via snprintf("%u.%u%u");
            // the conventional dotted form is easier to read in a registry dump.
            char rev[16];
            snprintf(rev, sizeof(rev), "%u.%u.%u",
                     (unsigned)((vs >> 16) & 0xff),
                     (unsigned)((vs >> 8)  & 0xff),
                     (unsigned)( vs        & 0xff));
            nub2->setProperty("VMD-Dbg-APL-Revision", rev);
        }
        
        nub2->setProperty("VMD-Dbg-APL-InitOK", capOK && cssOK && mpsOK && vsOK);
    }
    
    if (!stashed) bar0->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_acpi_find_companion() — port.
 *
 * Linux:
 *   busnr = pci_dev->bus->number - bridge->bus->number;  (<= 31)
 *   addr  = (busnr << 24) | (devfn << 16) | 0x8000FFFF;
 *   return acpi_find_child_device(ACPI_COMPANION(bridge->dev.parent), addr);
 *
 * On macOS the ACPI children of the VMD device live as DT-plane children
 * with an "acpi-device" property and a numeric "_ADR".  Some firmware
 * tables use different low-bit encodings, so the two address forms observed
 * on real VMD platforms are matched as well (carried over from the
 * original port, which validated them on 8086:9A0B hardware).
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
IOACPIPlatformDevice * VMDDriver::findACPICompanion(uint32_t busnrEcam,
                                                    uint8_t dev, uint8_t fn)
{
    if (!fPCIDev || busnrEcam > 31) return NULL;

    // exact Linux encoding
    uint32_t linuxAddr = ((busnrEcam & 0x1F) << 24)
                       | ((uint32_t)PCI_DEVFN(dev, fn) << 16)
                       | 0x8000FFFFU;

    // encodings observed in real VMD ACPI tables
    uint32_t ecamOff = PCIE_ECAM_OFFSET(busnrEcam, PCI_DEVFN(dev, fn), 0);
    uint32_t altAddr;
    if (ecamOff >= 0xe0000 && ecamOff < 0x100000)
        altAddr = (ecamOff << 4) + 0x8000FFFFU;
    else if (ecamOff >= 0x100000)
        altAddr = (ecamOff >> 4) + 0x80ffffU;
    else
        altAddr = (ecamOff << 4) + 0xFFF0U;
    altAddr &= 0xFFFFFFFF;

    OSIterator *iter = fPCIDev->getChildIterator(gIODTPlane);
    if (!iter) return NULL;
    iter->reset();

    IOACPIPlatformDevice *match = NULL;
    IORegistryEntry *entry;
    while ((entry = (IORegistryEntry *)iter->getNextObject())) {
        IOACPIPlatformDevice *acpi = VMDACPIDevice(
                                        entry->getProperty("acpi-device"));
        if (!acpi) continue;
        OSNumber *adr = OSDynamicCast(OSNumber, acpi->getProperty("_ADR"));
        if (!adr) continue;
        uint32_t value = adr->unsigned32BitValue();
        if (value == linuxAddr || value == altAddr) {
            match = acpi;
            break;
        }
    }
    iter->release();

    if (match)
        IOLog("IntelVMD: ACPI companion found for %02x:%02x.%x\n",
              fBusnStart + busnrEcam, dev, fn);
    return match;
}

/* Publish the ACPI association on a device's nub.
 *
 *   entry       — config entry of the device being associated
 *   inheritFrom — if the device has no ACPI node of its own (endpoints),
 *                 inherit the association from this nub (its root port) so
 *                 IONVMeController::CopyACPIParent() finds a non-NULL
 *                 acpiRootDevice via the parent-plane property walk.
 */
void VMDDriver::associateACPI(IOPCIConfigEntry *entry, IOPCIDevice *inheritFrom)
{
    if (!entry || !entry->dtNub) return;

    uint32_t busnrEcam = entry->space.s.busNum - fBusnStart;
    IOACPIPlatformDevice *acpiDev = findACPICompanion(busnrEcam,
                                                      entry->space.s.deviceNum,
                                                      entry->space.s.functionNum);
    if (acpiDev) {
        // Primary: publish a fromPath()-resolvable gIOServicePlane path
        // generated from the matched IOACPIPlatformDevice; fall back to the
        // node's own "acpi-path" property.
        char path[512];
        bzero(path, sizeof(path));
        int s=sizeof(path);
        if (acpiDev->getPath(path, &s, gIOServicePlane) && path[0]) {
            /* Memory leak fix: withCString() returns a retained string and
             * setProperty() takes its own reference — release ours after
             * publishing. */
            OSString *pathStr = OSString::withCString(path);
            if (pathStr) {
                entry->dtNub->setProperty(kACPIDevicePathKey, pathStr);
                pathStr->release();
            }
        } else {
            OSObject *prop = acpiDev->getProperty(kACPIDevicePathKey);
            if (prop) entry->dtNub->setProperty(kACPIDevicePathKey, prop);
        }
        entry->dtNub->setProperty("acpi-device", acpiDev);
        entry->acpiDevice = acpiDev;
        return;
    }

    if (inheritFrom) {
        OSObject *ad = inheritFrom->getProperty("acpi-device");
        OSObject *ap = inheritFrom->getProperty(kACPIDevicePathKey);
        if (ap) entry->dtNub->setProperty(kACPIDevicePathKey, ap);
        if (ad) {
            entry->dtNub->setProperty("acpi-device", ad);
            entry->acpiDevice = OSDynamicCast(IORegistryEntry, ad);
        }
        IOLog("IntelVMD: endpoint %02x:%02x.%x inherits ACPI association "
              "from its root port\n",
              entry->space.s.busNum, entry->space.s.deviceNum,
              entry->space.s.functionNum);
    } else {
        IOLog("IntelVMD: no ACPI companion for %02x:%02x.%x\n",
              entry->space.s.busNum, entry->space.s.deviceNum,
              entry->space.s.functionNum);
    }
}


void VMDDriver::wireChildMSI(IOPCIDevice *nub)
{
    if (!nub) return;

    nub->setProperty("VMD-MSI-Pass", true);

    // In demux mode this endpoint's interrupt KPI is served by
    // VMDChildDevice (see VMDDriver::vmdDemuxInterrupt).
    if (fDemuxMode) {
        VMDChildDevice *kid = OSDynamicCast(VMDChildDevice, nub);
        nub->setProperty("VMD-INT-Demux", kid ? addDemuxChild(kid) : false);
        // tripwire: a stock MSI registration (enableDeviceMSI sets this key)
        // already happened, i.e. a client beat the demux claim
        if (nub->getProperty("IOPCIMSIMode"))
            nub->setProperty("VMD-INT-Demux-Late", true);
    }

    // Demux mode never uses a stock CPU vector for the child, so
    // do not allocate one (it would only be a stale MSI the family might
    // restore later).
    if (!fDemuxMode && !nub->getProperty("IOPCIMSIMode")) {
        IOReturn r = nub->configureInterrupts(kIOInterruptTypePCIMessaged,
                                              1, 1);
        nub->setProperty("VMD-MSI-Cfg-Stat", r, 32);
    }

    composeChildMSI(nub);
}


void VMDDriver::composeChildMSI(IOPCIDevice *nub)
{

    OSNumber *vecProp =
        OSDynamicCast(OSNumber, nub->getProperty("VMD-MSI-Vec"));
    if (!vecProp) {
        uint32_t nv;
        if ((fMsixCount > (uint32_t)fFirstVec + 1)
            && (fNextFreeVec < fMsixCount)) {
            nv = fNextFreeVec++;
        } else {
            nv = fFirstVec;
        }
        // Only aim at a VMD index that has a forwarder; any one
        // works because vmdDemuxInterrupt() fans out to every demux child.
        if (fDemuxMode && fRegMask && (nv >= kVMDMaxIrqStat
                                       || !(fRegMask & (1u << nv)))) {
            for (uint32_t i = fFirstVec; i < kVMDMaxIrqStat; i++) {
                if (fRegMask & (1u << i)) { nv = i; break; }
            }
            nub->setProperty("VMD-MSI-Vec-Fallback", true);
        }
        nub->setProperty("VMD-MSI-Vec", nv, 32);
        vecProp = OSDynamicCast(OSNumber, nub->getProperty("VMD-MSI-Vec"));
        VMDChildDevice *cc = OSDynamicCast(VMDChildDevice, nub);
        /* Safety fix: both lookups can fail — do not dereference blindly. */
        if (cc && vecProp)
            cc->fVmdVector = vecProp->unsigned32BitValue();
        
    }
    uint32_t vec    = vecProp ? vecProp->unsigned32BitValue() : fFirstVec;
    uint32_t addrLo = X86_MSI_BASE_ADDRESS_LOW  | (vec << X86_MSI_DESTID_SHIFT);
    uint32_t addrHi = X86_MSI_BASE_ADDRESS_HIGH;
    if (vec >= fMsixCount) {                /* degenerate msix_count */
        nub->setProperty("VMD-MSI-NoVec", true);
        return;
    }

    bool wrote = false;
    uint8_t cap = 0;
    
    // MSI 1st place
    if (!wrote
        && nub->findPCICapability(kIOPCICapabilityIDMSI, &cap) && cap) {
        uint16_t ctrl = nub->configRead16(cap + PCI_MSI_FLAGS);
        nub->setProperty("VMD-MSI-Mode", "MSI");

        // MSI and MSI-X must never be enabled together; a stock
        // configureInterrupts(MSI-X) from the client driver may have
        // switched MSI-X on.  Turn it off BEFORE MSI is (re-)enabled.
        uint8_t xcap = 0;
        if (nub->findPCICapability(kIOPCICapabilityIDMSIX, &xcap) && xcap) {
            uint16_t xctrl = nub->configRead16(xcap + PCI_MSIX_FLAGS);
            if (xctrl & PCI_MSIX_FLAGS_ENABLE) {
                nub->configWrite16(xcap + PCI_MSIX_FLAGS,
                                   xctrl & ~PCI_MSIX_FLAGS_ENABLE);
                nub->setProperty("VMD-MSI-MSIX-Cleared", true);
            }
        }

        // Program the message BEFORE enabling MSI (the old code enabled
        // first, briefly sending with the previous message).  Linux
        // vmd_compose_msi_msg() zeroes the message and sets only address
        // bits 19:12 (= VMD vector index), so data is 0 in demux mode.
        UInt16 msgData = fDemuxMode ? 0 : (UInt16)vec;
        nub->configWrite32(cap + PCI_MSI_MSG_LOWER_ADDRESS, addrLo);
        if (ctrl & PCI_MSI_FLAGS_64BIT) {
            nub->configWrite32(cap + PCI_MSI_MSG_UPPER_ADDRESS, addrHi);
            nub->configWrite16(cap + PCI_MSI_MSG_DATA_64, msgData);
        } else {
            nub->configWrite16(cap + PCI_MSI_MSG_DATA_32, msgData);
        }

        if (((ctrl & PCI_MSI_FLAGS_ENABLE) == 0)) {
            ctrl |= PCI_MSI_FLAGS_ENABLE;
            nub->configWrite16(cap + PCI_MSI_FLAGS, ctrl);
            ctrl = nub->configRead16(cap + PCI_MSI_FLAGS);
            if (ctrl & PCI_MSI_FLAGS_ENABLE)
                nub->setProperty("VMD-MSI-MSI-En-Fixed", true);
        }
        if (ctrl & PCI_MSI_FLAGS_ENABLE) {
            nub->setProperty("VMD-MSI-AddrLo",
                             (unsigned long long)addrLo, 64);
            // read-back of what the endpoint will actually send
            nub->setProperty("VMD-MSI-RB-Ctrl", ctrl, 16);
            nub->setProperty("VMD-MSI-RB-AddrLo",
                (unsigned long long)nub->configRead32(cap + PCI_MSI_MSG_LOWER_ADDRESS), 64);
            nub->setProperty("VMD-MSI-RB-Data",
                (unsigned long long)nub->configRead16(cap + ((ctrl & PCI_MSI_FLAGS_64BIT)
                                                 ? PCI_MSI_MSG_DATA_64
                                                 : PCI_MSI_MSG_DATA_32)), 16);
            wrote = true;
        }
    }
    

    // MSI-X 2nd place
    if (!wrote)
    if (nub->findPCICapability(kIOPCICapabilityIDMSIX, &cap) && cap) {
        uint16_t ctrl = nub->configRead16(cap + PCI_MSIX_FLAGS);
        nub->setProperty("VMD-MSI-Mode", "MSI-X");
        nub->setProperty("VMD-MSI-Ctrl", ctrl, 16);
        if (((ctrl & PCI_MSIX_FLAGS_ENABLE) == 0)) {
            uint8_t msiCap = 0;
            if (nub->findPCICapability(kIOPCICapabilityIDMSI, &msiCap)
                && msiCap) {
                uint16_t msiCtrl = nub->configRead16(msiCap + PCI_MSI_FLAGS);
                if (msiCtrl & PCI_MSI_FLAGS_ENABLE) {
                    nub->configWrite16(msiCap + PCI_MSI_FLAGS,
                                       msiCtrl & ~PCI_MSI_FLAGS_ENABLE);
                    nub->setProperty("VMD-MSI-MSI-Cleared", true);
                }
            }
            ctrl |= PCI_MSIX_FLAGS_ENABLE;
            nub->configWrite16(cap + PCI_MSIX_FLAGS, ctrl);
            ctrl = nub->configRead16(cap + PCI_MSIX_FLAGS);
            if (ctrl & PCI_MSIX_FLAGS_ENABLE)
                nub->setProperty("VMD-MSI-MSIX-En-Fixed", true);
        }
        if (ctrl & PCI_MSIX_FLAGS_ENABLE) {
            uint32_t tobir = nub->configRead32(cap + PCI_MSIX_TABLE_OFFSET);
            uint8_t  bir   = tobir & 7;
            uint64_t toff  = tobir & ~7ULL;
            IOMemoryMap *bar = nub->mapDeviceMemoryWithRegister(
                        kIOPCIConfigBaseAddress0 + 4 * bir,
                        kIOMapInhibitCache | kIOMapAnywhere);
            if (bar) {
                volatile UInt8 *table =
                    (volatile UInt8 *)bar->getVirtualAddress() + toff;
                uint16_t nvec = (ctrl & 0x7FF) + 1;
                for (uint16_t i = 0; i < nvec; i++) {
                    volatile UInt32 *entry = (volatile UInt32 *)
                        (table + (uint32_t)i * PCI_MSIX_ENTRY_SIZE);
                    OSWriteLittleInt32(entry, PCI_MSIX_ENTRY_UPPER_ADDR, addrHi);
                    OSWriteLittleInt32(entry, PCI_MSIX_ENTRY_LOWER_ADDR, addrLo);
                    OSWriteLittleInt32(entry, PCI_MSIX_ENTRY_DATA,
                                       fDemuxMode ? 0 : vec);   /* Linux data 0 */
                    // Vector Control bit 0 = Mask.  Entries come up masked
                    // after reset and a masked entry swallows the interrupt
                    // even with MSI-X enabled; IOPCIFamily only unmasks
                    // vectors it allocated through the stock path.
                    OSWriteLittleInt32(entry, PCI_MSIX_ENTRY_VECTOR_CTRL, 0);
                }
                nub->setProperty("VMD-MSI-AddrLo",
                                 (unsigned long long)addrLo, 32);
                nub->setProperty("VMD-MSI-Verify",
                    OSReadLittleInt32((volatile UInt32 *)table,
                                      PCI_MSIX_ENTRY_LOWER_ADDR), 32);
                nub->setProperty("VMD-MSI-Verify-Ctrl",
                    OSReadLittleInt32((volatile UInt32 *)table,
                                      PCI_MSIX_ENTRY_VECTOR_CTRL), 32);
                bar->release();
                wrote = true;
            } else {
                nub->setProperty("VMD-MSI-Table-Map-Failed", true);
            }
        }
    }

    

    if (!wrote) {
        nub->setProperty("VMD-MSI-NoMessageCap", true);
        return;
    }


    IOPCIConfigShadow *shadow = configShadow(nub);
    if (shadow)
        for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
            shadow->configSave.savedConfig[i] = nub->configRead32(i * 4);
}


void VMDDriver::enableLTRQuirk(IOPCIDevice *dev)
{
    if (!dev || !(fFeatures & VMD_FEAT_BIOS_PM_QUIRK)) return;

    // pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_LTR)
    IOPCIAddressSpace space = dev->space;
    IOByteCount pos = 0;
    if (!extendedFindPCICapability(space, -PCI_EXT_CAP_ID_LTR, &pos) || !pos)
        return;

    // full 12-bit offset through the extended space bits
    IOPCIAddressSpace es = space;
    es.bits &= ~0x0F000000;
    es.es.registerNumExtended = ((uint32_t)pos >> 8) & 0xF;

    // skip if BIOS already programmed the max snoop LTR
    uint32_t ltrReg = configRead32(es, (UInt8)(pos & 0xFF));
    if (ltrReg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK)) return;

    uint16_t ltr = VMD_BIOS_PM_QUIRK_LTR;
    ltrReg = ((uint32_t)ltr << 16) | ltr;
    configWrite32(es, (UInt8)(pos & 0xFF), ltrReg);
    IOLog("IntelVMD: Default LTR value set by driver (%02x:%02x.%x)\n",
          dev->space.s.busNum, dev->space.s.deviceNum,
          dev->space.s.functionNum);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_domain_reset() — clear stale I/O ranges on bridges behind the VMD.
 *
 * Linux calls this between pci_scan_child_bus() and resource assignment.
 * IOPCIFamily assigns resources during the scan itself, so this is NOT
 * called automatically by start(); keep the Linux behaviour available for
 * debugging (call from a test harness if a platform needs it).
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::resetDomain(void)
{
    if (!fCfgBar) return;

    // vmd_cfgbar_ecam_space(): buses covered by the ECAM window
    uint32_t maxBuses = (uint32_t)(fCfgBarSize >> 20);
    uint32_t ecamStart = (fFeatures & VMD_FEAT_USE_BIOS_INFO) ? fBusnStart : 0;
    if (ecamStart >= maxBuses) return;
    maxBuses -= ecamStart;

    for (uint32_t bus = 0; bus < maxBuses; bus++) {
        uint32_t ecamBus = (fFeatures & VMD_FEAT_USE_BIOS_INFO)
                         ? (fBusnStart + bus) : bus;
        for (uint8_t dev = 0; dev < 32; dev++) {
            // Serialize with every other config cycle into the VMD
            // ECAM (Apple's root-port bridges may be probing concurrently).
            IOInterruptState rdIs = IOSimpleLockLockDisableInterrupt(fCfgLock);
            uint32_t off = PCIE_ECAM_OFFSET(ecamBus, PCI_DEVFN(dev, 0), 0);
            volatile uint8_t *base = fCfgBar + off;
            uint8_t hdrType = base[PCI_HEADER_TYPE];
            uint8_t functions = (hdrType & PCI_HEADER_TYPE_MFD) ? 8 : 1;

            for (uint8_t fn = 0; fn < functions; fn++) {
                off  = PCIE_ECAM_OFFSET(ecamBus, PCI_DEVFN(dev, fn), 0);
                base = fCfgBar + off;
                hdrType = base[PCI_HEADER_TYPE] & PCI_HEADER_TYPE_MASK;

                if (hdrType != PCI_HEADER_TYPE_BRIDGE
                    || OSReadLittleInt16(base, PCI_CLASS_DEVICE)
                       != PCI_CLASS_BRIDGE_PCI)
                    continue;

                // Temporarily disable the I/O range before updating
                // PCI_IO_BASE, then reset the bridge windows.
                OSWriteLittleInt32(base, PCI_IO_BASE_UPPER16, 0x0000ffff);
                OSWriteLittleInt16(base, PCI_IO_BASE, 0x00f0);
                OSWriteLittleInt32(base, PCI_IO_BASE_UPPER16, 0);

                OSWriteLittleInt32(base, PCI_MEMORY_BASE, 0x0000fff0);

                OSWriteLittleInt32(base, PCI_PREF_LIMIT_UPPER32, 0);
                OSWriteLittleInt32(base, PCI_PREF_MEMORY_BASE, 0x0000fff0);
                OSWriteLittleInt32(base, PCI_PREF_BASE_UPPER32, 0xffffffff);
                // force completion (VMD posts config writes; Linux reads back)
                (void)OSReadLittleInt32(base, PCI_PREF_BASE_UPPER32);
            }
            IOSimpleLockUnlockEnableInterrupt(fCfgLock, rdIs);
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Lifecycle — vmd_shutdown() / vmd_remove().
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::stop(IOService *provider)
{
    IOPCI2PCIBridge::stop(provider);
}

void VMDDriver::free(void)
{
    // Demux teardown (stats timer, then the VMD vector forwarders)
    if (fIrqStatTimer) {
        fIrqStatTimer->cancelTimeout();
        if (fVmdWorkLoop) fVmdWorkLoop->removeEventSource(fIrqStatTimer);
        fIrqStatTimer->release();
        fIrqStatTimer = NULL;
    }
    if (fPCIDev && fRegMask) {
        for (uint32_t i = 0; i < kVMDMaxIrqStat; i++) {
            if (!(fRegMask & (1u << i))) continue;   // only what we own
            fPCIDev->disableInterrupt((int)i);
            fPCIDev->unregisterInterrupt((int)i);
        }
        fRegMask = 0;
        fIrqVectorsRegistered = 0;
    }
    // no forwarder can run any more: drop the demux children
    {
        UInt32 n = fDemuxKidCount;
        fDemuxKidCount = 0;
        VMD_BARRIER();
        for (UInt32 i = 0; i < n && i < kVMDMaxDemuxKids; i++) {
            if (fDemuxKids[i]) {
                fDemuxKids[i]->release();
                fDemuxKids[i] = NULL;
            }
        }
    }
    if (fBridgeES) {
        if (fVmdWorkLoop) fVmdWorkLoop->removeEventSource(fBridgeES);
        fBridgeES->release();
        fBridgeES = NULL;
    }
    if (fCfgMap) {
        fCfgMap->release();
        fCfgMap = NULL;
    }
    if (fCfgLock) {
        IOSimpleLockFree(fCfgLock);
        fCfgLock = NULL;
    }
    if (fVmdWorkLoop) {
        fVmdWorkLoop->release();
        fVmdWorkLoop = NULL;
    }

    if (fPCIDev) {
        fPCIDev->close(this);
        fPCIDev->release();
        fPCIDev = NULL;
    }

    IOPCI2PCIBridge::free();
}

IOWorkLoop * VMDDriver::getWorkLoop(void) const
{
    return fVmdWorkLoop;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * MSI remap demux — Linux vmd_alloc_irqs() + vmd_irq().
 *
 * Remap-active VMD: a child's MSI write carries the VMD vector index in
 * address bits 19:12 (composeChildMSI()); the VMD re-issues it through its
 * OWN MSI-X table entry of that index.  IOPCIFamily programmed that table
 * (configureInterrupts(MSI-X) in setupMSIX()), so the CPU vector lands in
 * IOPCIMessagedInterruptController, which calls vmdDemuxInterrupt() below
 * in primary interrupt context.  It forwards to every demux child whose
 * interrupt is enabled; a spurious call is harmless (an NVMe filter only
 * inspects its completion-queue phase bits).
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::registerDemuxVectors(void)
{
    IOInterruptAction act = OSMemberFunctionCast(IOInterruptAction, this,
                                                 &VMDDriver::vmdDemuxInterrupt);
    uint32_t ok = 0, firstFail = 0xFFFFFFFF;
    IOReturn failRet = kIOReturnSuccess;
    fRegMask = 0;
    for (uint32_t i = 0; i < fMsixCount && i < kVMDMaxIrqStat; i++) {
        IOReturn r = fPCIDev->registerInterrupt((int)i, this, act,
                                                (void *)(uintptr_t)i);
        if (r == kIOReturnSuccess) {
            r = fPCIDev->enableInterrupt((int)i);
            if (r == kIOReturnSuccess) {
                fRegMask |= (1u << i);
                ok++;
                continue;
            }
            fPCIDev->unregisterInterrupt((int)i);
        }
        if (firstFail == 0xFFFFFFFF) {
            firstFail = i;
            failRet = r;
        }
    }
    fIrqVectorsRegistered = ok;
    fPCIDev->setProperty("VMD-IRQ-Vectors-Registered", ok, 32);
    fPCIDev->setProperty("VMD-IRQ-Reg-Mask", fRegMask, 32);
    if (firstFail != 0xFFFFFFFF) {
        fPCIDev->setProperty("VMD-IRQ-Register-FirstFail", firstFail, 32);
        fPCIDev->setProperty("VMD-IRQ-Register-FailRet", failRet, 32);
    }
    IOLog("IntelVMD: demux: %u of %u VMD vectors registered (mask 0x%x)\n",
          ok, fMsixCount, fRegMask);

    // Linux never remaps a child onto vector 0 when OFFSET_FIRST_VECTOR is
    // set, so at least one index >= fFirstVec must work.
    if ((fRegMask >> fFirstVec) == 0) {
        for (uint32_t i = 0; i < kVMDMaxIrqStat; i++) {
            if (fRegMask & (1u << i)) {
                fPCIDev->disableInterrupt((int)i);
                fPCIDev->unregisterInterrupt((int)i);
            }
        }
        fRegMask = 0;
        fIrqVectorsRegistered = 0;
        return false;
    }
    return true;
}

bool VMDDriver::ownsConfigEntry(IOPCIConfigEntry *entry) const
{
    for (IOPCIConfigEntry *e = entry ? entry->parent : NULL; e; e = e->parent)
        if (e == fBridgeEntry) return true;
    return false;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * read-only snapshot of the VMD's own MSI-X table (rows 0..3),
 * its control word and PBA dword 0, plus the live VMCONFIG.  If the NVMe
 * MSI is remapped but no VMD vector fires, this shows whether the rows are
 * programmed and unmasked.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::snapshotVmdMsix(void)
{
    fPCIDev->setProperty("VMD-VMCONFIG-Live",
        (unsigned long long)fPCIDev->configRead16(PCI_REG_VMCONFIG), 16);

    uint8_t cap = 0;
    if (!fPCIDev->findPCICapability(kIOPCICapabilityIDMSIX, &cap) || !cap) return;
    uint16_t ctrl  = fPCIDev->configRead16(cap + 2);
    uint32_t tobir = fPCIDev->configRead32(cap + 4);
    uint32_t pbobir = fPCIDev->configRead32(cap + 8);
    fPCIDev->setProperty("VMD-MSIX-Snap-Ctrl", ctrl, 16);
    fPCIDev->setProperty("VMD-MSIX-Snap-TableOffBIR", (unsigned long long)tobir, 64);

    IOMemoryMap *bar = fPCIDev->mapDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0 + 4 * (tobir & 7),
        kIOMapInhibitCache | kIOMapAnywhere);
    if (!bar) {
        fPCIDev->setProperty("VMD-MSIX-Snap-MapFailed", true);
        return;
    }
    volatile UInt8 *table = (volatile UInt8 *)bar->getVirtualAddress()
                          + (tobir & ~7U);
    OSArray *rows = OSArray::withCapacity(16);
    if (rows) {
        for (uint32_t i = 0; i < 4 && i < fMsixCount; i++) {
            for (uint32_t w = 0; w < 4; w++) {
                OSNumber *n = OSNumber::withNumber((unsigned long long)
                    OSReadLittleInt32(table, i * PCI_MSIX_ENTRY_SIZE + w * 4), 64);
                if (n) { rows->setObject(n); n->release(); }
            }
        }
        fPCIDev->setProperty("VMD-MSIX-Snap-Rows0to3", rows);   // lo,hi,data,ctrl x4
        rows->release();
    }
    if ((pbobir & 7) == (tobir & 7)) {
        fPCIDev->setProperty("VMD-MSIX-Snap-PBA0",
            (unsigned long long)OSReadLittleInt32((volatile UInt8 *)bar->getVirtualAddress(),
                                                  pbobir & ~7U), 64);
    }
    bar->release();
}

void VMDDriver::vmdDemuxInterrupt(void *refCon, IOService * /* nub */,
                                  int /* source */)
{
    uintptr_t idx = (uintptr_t)refCon;
    if (idx < kVMDMaxIrqStat)
        OSIncrementAtomic((volatile SInt32 *)&fIrqHits[idx]);

    UInt32 n = fDemuxKidCount;
    VMD_BARRIER();
    bool matched = false;
    for (UInt32 i = 0; i < n && i < kVMDMaxDemuxKids; i++) {
        VMDChildDevice *kid = fDemuxKids[i];
        // Linux vmd_irq(): only the devices aimed at THIS vector.  A spurious
        // call into IONVMe's filter can leave its INTMS mask set (the filter
        // masks, a workloop pass that finds nothing never unmasks), so do
        // not fan out.  A kid not yet aimed (no vector) still gets every
        // event; VMD-IRQ-Hits / VMD-IRQ-Orphan show where events land.
        if (kid && (kid->fVmdVector == 0xFFFFFFFF || kid->fVmdVector == idx)) {
            matched = true;
            kid->demuxDispatch();
        }
    }
    if (!matched && idx < kVMDMaxIrqStat)
        OSIncrementAtomic((volatile SInt32 *)&fIrqOrphan[idx]);
}

bool VMDDriver::addDemuxChild(VMDChildDevice *kid)
{
    if (!kid) return false;
    for (UInt32 i = 0; i < fDemuxKidCount; i++)
        if (fDemuxKids[i] == kid) return true;
    if (fDemuxKidCount >= kVMDMaxDemuxKids) return false;
    if (!kid->fIntLock) kid->fIntLock = IOSimpleLockAlloc();
    if (!kid->fIntLock) return false;

    kid->retain();                 // the forwarder holds a raw pointer
    kid->fDemux = true;
    // allocate the VMD vector exactly once, here (single-threaded: nub
    // creation under the configurator gate, or postScanConfigure);
    // composeChildMSI() then only reuses "VMD-MSI-Vec"
    if (kid->fVmdVector == 0xFFFFFFFF && !kid->getProperty("VMD-MSI-Vec")) {
        kid->fVmdVector = allocDemuxVector();
        kid->setProperty("VMD-MSI-Vec", kid->fVmdVector, 32);
    }
    fDemuxKids[fDemuxKidCount] = kid;
    VMD_BARRIER();
    fDemuxKidCount = fDemuxKidCount + 1;
    return true;
}

uint32_t VMDDriver::allocDemuxVector(void)
{
    uint32_t nv;
    if ((fMsixCount > (uint32_t)fFirstVec + 1) && (fNextFreeVec < fMsixCount))
        nv = fNextFreeVec++;               // Linux vmd_next_irq(): NVMe gets its own
    else
        nv = fFirstVec;
    // only an index that has a forwarder
    if (fRegMask && (nv >= kVMDMaxIrqStat || !(fRegMask & (1u << nv)))) {
        for (uint32_t i = fFirstVec; i < kVMDMaxIrqStat; i++)
            if (fRegMask & (1u << i)) { nv = i; break; }
    }
    return nv;
}

/* Remove the deferral key FIRST, then decide on service-plane attachment
 * (getProvider): either IOPCIBridge::publishNub() has already attached the
 * nub, and we register it (if publishNub registers it too, the second
 * registerService only re-runs matching), or it has not attached it yet and
 * will register it itself because the key is gone.  No interleaving leaves
 * the nub attached but never registered. */
bool VMDDriver::releaseDeferredKid(VMDChildDevice *kid)
{
    if (!kid || !kid->fRegDeferred) return false;
    kid->fRegDeferred = false;
    kid->removeProperty("pci-deferred-registration");
    VMD_BARRIER();
    if (kid->getProvider()) {
        IOSleep(20);        // let publishNub finish IOPCIDevice::attach()
        kid->setProperty("VMD-Reg-Released-MS", vmdUptimeMS(), 64);
        // Asynchronous: if the provider chain carries
        // kIOServiceSynchronousState a plain registerService() would run the
        // client's start() (IONVMeController) inline on this thread - on the
        // late path that is the VMD workloop, which the nub's own
        // getWorkLoop() returns.
        kid->registerService(kIOServiceAsynchronous);
        kid->setProperty("VMD-Reg-Return-MS", vmdUptimeMS(), 64);
        return true;
    }
    kid->setProperty("VMD-Reg-Released-Unattached", true);
    return false;
}

static uint32_t vmdCountInaccessible(IOPCIConfigEntry *bridge, int depth)
{
    if (!bridge || depth > 4) return 0;
    uint32_t n = 0;
    for (IOPCIConfigEntry *c = bridge->child; c; c = c->peer) {
        VMDChildDevice *k = OSDynamicCast(VMDChildDevice, c->dtNub);
        if (k && !k->vmdConfigAccessible()) n++;
        if (c->isBridge) n += vmdCountInaccessible(c, depth + 1);
    }
    return n;
}

uint32_t VMDDriver::waitDomainAccessible(uint32_t maxMS)
{
    uint32_t waited = 0;
    while (vmdCountInaccessible(fBridgeEntry, 0) && waited < maxMS) {
        IOSleep(10);
        waited += 10;
    }
    fPCIDev->setProperty("VMD-Domain-Inaccessible",
                         vmdCountInaccessible(fBridgeEntry, 0), 32);
    return waited;
}

void VMDDriver::irqStatTick(IOTimerEventSource *sender)
{
    fIrqStatTicks++;
    bool publishVmd = (fIrqStatTicks <= 600) || ((fIrqStatTicks % 10) == 0);

    OSArray *hits = publishVmd ? OSArray::withCapacity(fMsixCount ? fMsixCount : 1) : NULL;
    if (hits) {
        for (uint32_t i = 0; i < fMsixCount && i < kVMDMaxIrqStat; i++) {
            OSNumber *num = OSNumber::withNumber((unsigned long long)fIrqHits[i], 32);
            if (num) {
                hits->setObject(num);
                num->release();
            }
        }
        fPCIDev->setProperty("VMD-IRQ-Hits", hits);
        hits->release();
    }
    OSArray *orph = publishVmd ? OSArray::withCapacity(fMsixCount ? fMsixCount : 1) : NULL;
    if (orph) {
        for (uint32_t i = 0; i < fMsixCount && i < kVMDMaxIrqStat; i++) {
            OSNumber *num = OSNumber::withNumber((unsigned long long)fIrqOrphan[i], 32);
            if (num) {
                orph->setObject(num);
                num->release();
            }
        }
        fPCIDev->setProperty("VMD-IRQ-Orphan", orph);
        orph->release();
    }
    if (publishVmd)
        fPCIDev->setProperty("VMD-IRQ-Stat-Ticks", fIrqStatTicks, 32);

    // An endpoint whose nub was not initialised within the configure wait:
    // configure it now that Apple's bridge has initialised it, then release
    // it for matching (it stayed deferred meanwhile).
    for (UInt32 i = 0; i < fDemuxKidCount && i < kVMDMaxDemuxKids; i++) {
        VMDChildDevice *kid = fDemuxKids[i];
        if (!kid || !kid->fCfgPending || !kid->vmdConfigAccessible()) continue;
        kid->fCfgPending = false;
        configureChildDevice(kid->fCfgEntry, kid->fCfgParent, kid->fCfgDepth);
        vmdReassertAll();
        kid->setProperty("VMD-Cfg-Late-Done-MS", vmdUptimeMS(), 64);
        releaseDeferredKid(kid);
    }

    // No periodic device MMIO or config-space traffic into the VMD
    // domain while I/O may be running - counters only.  The one exception is
    // the stuck-INTMS rescue below, which touches the device only after two
    // full seconds without a single interrupt.
    for (UInt32 i = 0; i < fDemuxKidCount && i < kVMDMaxDemuxKids; i++) {
        VMDChildDevice *kid = fDemuxKids[i];
        if (!kid) continue;

        // Lost-interrupt safety net (memory only, no device access).  If no
        // interrupt arrived for 2 ticks while the client's handler is live,
        // run the handler once ourselves, exactly as the interrupt would:
        // IONVMe's filter only inspects its completion-queue phase bits in
        // RAM; if entries are pending it masks/signals and its workloop
        // processes them and unmasks (covers a dropped MSI and a stuck
        // INTMS mask alike).  With nothing pending the call is a no-op.
        // Same lock discipline as enableInterrupt()'s replay.
        UInt32 d = kid->fIntDispatched;
        if (d != kid->fLastDispatchSeen) {
            kid->fLastDispatchSeen = d;
            kid->fStallTicks = 0;
        } else if (kid->fIntEnabled && kid->fIntHandler && kid->fIntLock) {
            if (++kid->fStallTicks >= 2) {
                IOInterruptState is = IOSimpleLockLockDisableInterrupt(kid->fIntLock);
                IOInterruptAction h = kid->fIntHandler;
                if (kid->fIntEnabled && h) {
                    h(kid->fIntTarget, kid->fIntRefCon, kid, kid->fIntSource);
                    kid->fIntmsRescues++;                 // = synthetic dispatches
                }
                IOSimpleLockUnlockEnableInterrupt(kid->fIntLock, is);
                kid->fStallTicks = 0;                     // again in 2 s
            }
        } else {
            kid->fStallTicks = 0;
            kid->fLastDispatchSeen = d;
        }

        bool publish = (fIrqStatTicks <= 600) || ((fIrqStatTicks % 10) == 0);
        if (!publish) continue;
        kid->setProperty("VMD-INT-Dispatched", (unsigned long long)d, 64);
        kid->setProperty("VMD-INT-Synthetic",
                         (unsigned long long)kid->fIntmsRescues, 64);
        kid->setProperty("VMD-INT-Latched",
                         (unsigned long long)kid->fIntLatched, 64);
        // bit0 handler stored, bit1 enabled, bit2 pending (latched)
        kid->setProperty("VMD-INT-State",
            (unsigned long long)((kid->fIntHandler ? 1 : 0)
                                 | (kid->fIntEnabled ? 2 : 0)
                                 | (kid->fIntPending ? 4 : 0)), 64);
    }

    // always 1 s (the safety net needs it); properties thin out after 10 min
    if (sender)
        sender->setTimeoutMS(1000);
}

UInt8 VMDDriver::firstBusNum(void)
{
    return fBusnStart;
}

UInt8 VMDDriver::lastBusNum(void)
{
    return fBusnEnd;
}

IOPCIAddressSpace VMDDriver::getBridgeSpace(void)
{
    return fBridgeSpace;
}


/* Vector 0 is reserved when VMD_FEAT_OFFSET_FIRST_VECTOR is set (Linux:
 * "Device may use MSI-X vector 0 for software triggering and will not be
 * used for MSI remapping").  Keep the handler idle; real bridge events use
 * IOPCI2PCIBridge's own fBridgeInterruptSource. */
void VMDDriver::handleBridgeInterrupt(OSObject * /* owner */,
                                      IOInterruptEventSource * /* sender */,
                                      int /* count */)
{
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * VMDChildDevice helpers.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOWorkLoop * VMDChildDevice::getWorkLoop(void) const
{
    if (pvmd) return pvmd->getWorkLoop();
    return IOService::getWorkLoop();
}

void VMDChildDevice::free(void)
{
    // appleNVMeInitDebug() stashes its BAR0 debug mapping here; release it
    // so the probe does not leak the IOMemoryMap when the nub is torn down.
    if (rmmio) {
        rmmio->release();
        rmmio = NULL;
    }
    rmmioPtr = NULL;
    if (fIntLock) {
        IOSimpleLockFree(fIntLock);
        fIntLock = NULL;
    }
    IOPCIDevice::free();
}


IOPCIConfigEntry * VMDChildDevice::configEntryOf(IOPCIDevice *device)
{
    if (!device) return NULL;
    return ((VMDChildDevice *)device)->reserved->configEntry;
}

bool VMDChildDevice::handleOpen(IOService * forClient, IOOptionBits options, void * arg)
{
    this->setProperty("SERV", forClient->getName());
    // Controller state at the moment the client driver takes over
    this->setProperty("VMD-Open-MS", vmdUptimeMS(), 64);
    if (rmmioPtr) {
        this->setProperty("VMD-Open-CSTS",  (unsigned long long)rmmioPtr[0x1C / 4], 64);
        this->setProperty("VMD-Open-CC",    (unsigned long long)rmmioPtr[0x14 / 4], 64);
        this->setProperty("VMD-Open-INTMS", (unsigned long long)rmmioPtr[0x0C / 4], 64);
    }
    if (!strcmp(forClient->getName(),"IONVMeController"))
    {
        //getMember<void *>(forClient, 0x148)=rmmio;
        
        IOPCIDevice *rp, *nub2;
        nub2 = this;
        rp=OSDynamicCast(IOPCIDevice,nub2->getParentEntry( gIODTPlane));
        pvmd->vmdBridgeCheck(rp, nub2);
        // opt-in only: link PM state as IONVMe starts
        if (gVmdLinkPMMask) vmdLinkPMSnapshot(pvmd, rp, nub2, "Open");
        
    }
    return IOPCIDevice::handleOpen(forClient,  options,  arg);
}

/* Controller state when the client driver gives up / detaches. */
void VMDChildDevice::handleClose(IOService * forClient, IOOptionBits options)
{
    if (forClient) {
        this->setProperty("VMD-Close-Client", forClient->getName());
        this->setProperty("VMD-Close-MS", vmdUptimeMS(), 64);
        if (rmmioPtr) {
            this->setProperty("VMD-Close-CSTS",  (unsigned long long)rmmioPtr[0x1C / 4], 64);
            this->setProperty("VMD-Close-CC",    (unsigned long long)rmmioPtr[0x14 / 4], 64);
            this->setProperty("VMD-Close-INTMS", (unsigned long long)rmmioPtr[0x0C / 4], 64);
        }
        this->setProperty("VMD-INT-Dispatched", (unsigned long long)fIntDispatched, 64);
        // opt-in only: link PM state + AER counts at close
        if (gVmdLinkPMMask && pvmd && !strcmp(forClient->getName(), "IONVMeController"))
            vmdLinkPMSnapshot(pvmd, OSDynamicCast(IOPCIDevice, getParentEntry(gIODTPlane)), this, "Close");
    }
    IOPCIDevice::handleClose(forClient, options);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * demux-mode IOService interrupt KPI.
 *
 * Apple's IONVMeController::CreateDeviceInterrupt() calls
 * configureInterrupts(MSI-X) (stock, left alone), then walks
 * getInterruptType() for an MSI/MSI-X index and builds an
 * IOFilterInterruptEventSource on it, which calls registerInterrupt() /
 * enableInterrupt() / disableInterrupt() / unregisterInterrupt() here.
 * Index 0 is reported as an edge PCI-messaged interrupt; the handler is
 * stored and run by VMDDriver::vmdDemuxInterrupt() when the VMD vector
 * this endpoint is wired to fires.  Non-demux nubs keep the stock path.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDChildDevice::demuxDispatch(void)
{
    if (!fIntLock) return;
    IOSimpleLockLock(fIntLock);              // already at interrupt level
    IOInterruptAction h = fIntHandler;
    if (h) {
        if (fIntEnabled) {
            OSIncrementAtomic((volatile SInt32 *)&fIntDispatched);
            h(fIntTarget, fIntRefCon, this, fIntSource);
            // NO device MMIO here.  A non-posted read at interrupt
            // level on the CPU that takes every device interrupt can wedge
            // the machine if the read stalls (neither Apple's NVMe path nor
            // Linux vmd_irq() reads the device in the primary handler).
        } else {
            fIntPending = true;              // replayed by enableInterrupt()
            OSIncrementAtomic((volatile SInt32 *)&fIntLatched);
        }
    }
    IOSimpleLockUnlock(fIntLock);
}

IOReturn VMDChildDevice::getInterruptType(int source, int *interruptType)
{
    if (!fDemux) return IOPCIDevice::getInterruptType(source, interruptType);
    if (source != 0) return kIOReturnNoInterrupt;
    if (interruptType)
        *interruptType = kIOInterruptTypeEdge | kIOInterruptTypePCIMessaged;
    return kIOReturnSuccess;
}

IOReturn VMDChildDevice::registerInterrupt(int source, OSObject *target,
                                           IOInterruptAction handler,
                                           void *refCon)
{
    if (!fDemux) {
        // tripwire: under remap a stock CPU-vector registration can never
        // fire (the VMD re-targets the message); make it visible in ioreg
        if (pvmd && pvmd->isDemuxMode())
            this->setProperty("VMD-INT-Stock-Register-Under-Remap",
                              (unsigned long long)source, 32);
        return IOPCIDevice::registerInterrupt(source, target, handler, refCon);
    }
    if (source != 0 || !handler || !fIntLock) return kIOReturnNoInterrupt;

    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fIntLock);
    if (fIntHandler) {
        IOSimpleLockUnlockEnableInterrupt(fIntLock, is);
        return kIOReturnNoResources;
    }
    fIntEnabled = false;
    fIntPending = false;
    fIntTarget  = target;
    fIntRefCon  = refCon;
    fIntSource  = source;
    fIntHandler = handler;
    IOSimpleLockUnlockEnableInterrupt(fIntLock, is);

    this->setProperty("VMD-INT-Registered-MS", vmdUptimeMS(), 64);
    if (target)
        this->setProperty("VMD-INT-Reg-Target",
                          target->getMetaClass()->getClassName());
    return kIOReturnSuccess;
}

IOReturn VMDChildDevice::unregisterInterrupt(int source)
{
    if (!fDemux) return IOPCIDevice::unregisterInterrupt(source);
    if (source != 0 || !fIntLock) return kIOReturnNoInterrupt;

    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fIntLock);
    fIntEnabled = false;
    fIntPending = false;
    fIntHandler = NULL;
    fIntTarget  = NULL;
    fIntRefCon  = NULL;
    IOSimpleLockUnlockEnableInterrupt(fIntLock, is);

    this->setProperty("VMD-INT-Unregistered-MS", vmdUptimeMS(), 64);
    return kIOReturnSuccess;
}

IOReturn VMDChildDevice::enableInterrupt(int source)
{
    if (!fDemux) return IOPCIDevice::enableInterrupt(source);
    if (source != 0 || !fIntHandler) return kIOReturnNoInterrupt;

    // the client's stock configureInterrupts() may have re-aimed the MSI at
    // a CPU vector (or switched to MSI-X): re-aim it at our VMD vector.
    if (pvmd) pvmd->composeChildMSI(this);

    fIntEnableCalls++;
    VMD_BARRIER();
    fIntEnabled = true;

    // Like the stock controller's enableVector(): an MSI that fired while
    // the source was disabled is latched (fIntPending) and replayed once
    // here.  Only when one really fired, so the client's handler never runs
    // before it could have run on real hardware.
    if (fIntLock) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fIntLock);
        IOInterruptAction h = fIntHandler;
        if (fIntPending && fIntEnabled && h) {
            fIntPending = false;
            OSIncrementAtomic((volatile SInt32 *)&fIntDispatched);
            h(fIntTarget, fIntRefCon, this, fIntSource);
        }
        IOSimpleLockUnlockEnableInterrupt(fIntLock, is);
    }

    this->setProperty("VMD-INT-Enable-Calls", fIntEnableCalls, 32);
    this->setProperty("VMD-INT-Enabled-MS", vmdUptimeMS(), 64);
    return kIOReturnSuccess;
}

IOReturn VMDChildDevice::disableInterrupt(int source)
{
    if (!fDemux) return IOPCIDevice::disableInterrupt(source);
    if (source != 0) return kIOReturnNoInterrupt;
    fIntEnabled = false;             // may run at interrupt level: no registry
    // Like the stock controller: when called with interrupts enabled (thread
    // context), wait out a dispatch that is already running the handler, so
    // the client can tear down its queues after disable() returns.
    if (fIntLock && ml_get_interrupts_enabled()) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fIntLock);
        IOSimpleLockUnlockEnableInterrupt(fIntLock, is);
    }
    return kIOReturnSuccess;
}



/*
IOReturn VMDChildDevice::configureInterrupts(UInt32 interruptType,
                                             UInt32 numRequired,
                                             UInt32 numRequested,
                                             IOOptionBits options)
{
    IOReturn ret = IOPCIDevice::configureInterrupts(interruptType,numRequired,numRequested, options);
    setProperty("VMD-MSI-configureInterrupts-Type", interruptType, 32);
    setProperty("VMD-MSI-configureInterrupts-Stat", ret, 32);

    return ret;
}

IOReturn VMDChildDevice::registerInterrupt(int source, OSObject *target,
                                           IOInterruptAction handler,
                                           void *refCon)
{
    IOReturn ret = IOPCIDevice::registerInterrupt(source, target, handler,refCon);
    setProperty("VMD-MSI-registerInterrupt-Stat", ret, 32);

    return ret;
}
*/

void IOPCIConfigurator::doConfigure(uint32_t options)
{
    bool bootConfig = (kIOPCIConfiguratorBoot & options);

    if (bootConfig) 
        iterate("boot reset", &IOPCIConfigurator::scanProc,&IOPCIConfigurator::bootResetProc,this);
    
    iterate("scan total", &IOPCIConfigurator::scanProc,&IOPCIConfigurator::totalProc,NULL);
    iterate("allocate",   &IOPCIConfigurator::allocateProc,NULL,NULL);
    iterate("finalize",   &IOPCIConfigurator::bridgeFinalizeConfigProc, &IOPCIConfigurator::domainFinalizeConfigProc, NULL);
}

int32_t IOPCIConfigurator::bridgeFinalizeConfigProc(void * unused, IOPCIConfigEntry * bridge)
{
    uint32_t deviceControl, newControl;

    if (!(kPCIDeviceStateAllocated & bridge->deviceState)) return (true);
    bridge->deviceState &= ~kPCIDeviceStateChildChanged;

#if defined(__i386__) || defined(__x86_64__)
    if ((kPCIHPTypeMask & bridge->supportsHotPlug) >= kPCIHotPlug)
#endif
    {
        FOREACH_CHILD(bridge, child)
        {
            if (kPCIDeviceStateDeadOrHidden   & child->deviceState) continue;
            if (!child->expressCapBlock)                             continue;
            // Re-calculate Device Control settings if either the domain changed or we paused to service an MPS update
            if (!(child->rootPortEntry->deviceState & kPCIDeviceStateDomainChanged) && !(fStates & kIOPCIConfiguratorMPSOverridePause)) continue;
            deviceControl = configRead16(child, child->expressCapBlock + 0x08);
            if ((fStates & kIOPCIConfiguratorMPSOverride) && (child->deviceState & kPCIDeviceStatePaused))
            {
                fStates &= ~kIOPCIConfiguratorMPSOverride;
                // in the future, we want to intelligently set RP MPS to the lowest MPS in the topology
                // for now, we will set RP MPS = 128B until pause is more stable
                child->rootPortEntry->expressMaxPayload = MIN_MPS;
                // since the root port is not paused, this is the USP connected to the root port. We need to manually update root port's MPS.
                uint32_t rootPortDeviceControl = configRead16(child->rootPortEntry, child->rootPortEntry->expressCapBlock + 0x08);
                uint32_t rootPortNewControl = rootPortDeviceControl & ~(7 << 5);
                rootPortNewControl |= ((child->rootPortEntry->expressMaxPayload & 7) << 5);
                configWrite16(child->rootPortEntry, child->rootPortEntry->expressCapBlock + 0x08, rootPortNewControl);
            }
            newControl    = deviceControl & ~(7 << 5);
            newControl    |= ((child->rootPortEntry->expressMaxPayload & 7) << 5);
            if (!child->isBridge && (child->hostBridgeEntry->expressEndpointMaxReadRequestSize != -1))
            {
                newControl = newControl & ~(7 << 12);
                newControl |= (child->hostBridgeEntry->expressEndpointMaxReadRequestSize << 12);
            }
            if (newControl != deviceControl)
            {
                configWrite16(child, child->expressCapBlock + 0x08, newControl);
            }
        }
    }

    if ((kPCIDeviceStateChildAdded & bridge->deviceState) && !fWaitingPause)
    {
        bridge->deviceState &= ~kPCIDeviceStateChildAdded;
        markChanged(bridge);
    }

    return (bridgeConstructDeviceTree(unused, bridge));
}

void IOPCIConfigurator::configure(uint32_t options)
{
    bool bootConfig = (kIOPCIConfiguratorBoot & options);


    // An Endpoint may return CRS after a Reset which requires up to 1s wait time
    // for Successful response. However it is arguable that boot is a form of reset.
    // rdar://106326237 shows that the Endpoint responded with CRS on boot scan
    fResetStartTime = mach_absolute_time();
    // Time scale is millisecond
    // Per PCIE Base spec v5.0, PCIE device must link train, initialize, and return
    // successful completion to valid Config Request 1s after reset exit
    // So we're supposed to wait 900ms since we waited 100ms after link up
    fResetWaitTime = kIOPCIEnumerationWaitTime;

    PE_Video           consoleInfo;

    fFlags |= options;

#if defined(__i386__) || defined(__x86_64__)
    if (bootConfig)
    {
        IOService::getPlatform()->getConsoleInfo(&consoleInfo);

        if (!fPFMConsole)
        {
            fPFMConsole  = consoleInfo.v_baseAddr;
#ifndef __LP64__
            fPFMConsole |= (((uint64_t) consoleInfo.v_baseAddrHigh) << 32);
#endif

        }
        fConsoleRange = NULL;
        getPlatform()->setConsoleInfo(0, kPEDisableScreen);
    }
#endif

    doConfigure(options);

#if defined(__i386__) || defined(__x86_64__)
    if (bootConfig)
    {
        if (fConsoleRange)
        {
            if (fConsoleRange->start)
                fPFMConsole += fConsoleRange->start;
            else
                fPFMConsole = 0;
        }
        if (!fPFMConsole)
        {
        }
        else
        {
            if (fConsoleRange)
            {
                consoleInfo.v_baseAddr = (fPFMConsole | 1);
#ifdef __LP64__
#else
                consoleInfo.v_baseAddrHigh = (fPFMConsole >> 32);
                DLOG("console setting 0x%lx:%lx\n",
                    consoleInfo.v_baseAddrHigh, consoleInfo.v_baseAddr);
#endif
                getPlatform()->setConsoleInfo(&consoleInfo, kPEBaseAddressChange);
            }
            getPlatform()->setConsoleInfo(NULL, kPEEnableScreen);
            if (fConsoleRange)
                IOLog("console relocated to 0x%llx\n", fPFMConsole);
        }
    }
#endif

    fFlags &= ~options;

    fResetStartTime = 0;
    fResetWaitTime = 0;
}

IOReturn IOPCIConfigurator::configOp(IOService * device, uintptr_t op, void * arg, void * arg2)
{
    IOReturn           ret = kIOReturnUnsupported;
    IOPCIConfigEntry * entry = NULL;
    IOPCIDevice *      pciDevice;

    if ((pciDevice = OSDynamicCast(IOPCIDevice, device)))
        entry = pciDevice->reserved->configEntry;

    switch (op)
    {
        case kConfigOpAddHostBridge:
            ret = addHostBridge(OSDynamicCast(IOPCIHostBridge, device));
            return (ret);
            break;

        case kConfigOpGetState:
            if (!entry) *((uint32_t *) arg) = kPCIDeviceStateDead;
            else
            {
                *((uint32_t *) arg) = entry->deviceState;
            }
            ret = kIOReturnSuccess;
            return (ret);
            break;


        case kConfigOpFindEntry:
            entry = findEntry(device, *((IOPCIAddressSpace *) arg2));
            *((void **) arg) = NULL;
            if (!entry || !entry->dtNub) ret = kIOReturnNotFound;
            else
            {
                entry->dtNub->retain();
                *((void **) arg) = entry->dtNub;
                ret = kIOReturnSuccess;
            }
            return (ret);
            break;

        case kConfigOpFindEntryByAddress:
            entry = findEntryByAddress(device, *((IOPCIScalar *) arg2));
            *((void **) arg) = NULL;
            if (!entry || !entry->dtNub) ret = kIOReturnNotFound;
            else
            {
                entry->dtNub->retain();
                *((void **) arg) = entry->dtNub;
                ret = kIOReturnSuccess;
            }
            return (ret);
            break;

        case kConfigOpTerminated:
            if (entry)
            {
                bridgeRemoveChild(entry->parent, entry);
            }
            ret = kIOReturnSuccess;
            return (ret);
            break;
    }

    if (!entry) return (kIOReturnBadArgument);

    switch (op)
    {
        case kConfigOpNeedsScan:
            entry->deviceState &= ~kPCIDeviceStateScanned;
            ret = kIOReturnSuccess;
            break;

        case kConfigOpLinkInt:
            entry->deviceState |= kPCIDeviceStateLinkInt;
            ret = kIOReturnSuccess;
            break;

        case kConfigOpShadowed:
            entry->configShadow = (uint8_t *) arg;
            ret = kIOReturnSuccess;
            if (!arg) break;
            /* fall thru */
        case kConfigOpPaused:

            if (kPCIDeviceStateRequestPause & entry->deviceState)
            {
                entry->deviceState &= ~kPCIDeviceStateRequestPause;
                entry->deviceState &= ~kPCIDeviceStateAllocated;
                fWaitingPause--;
            }
            if (!(kPCIDeviceStatePaused & entry->deviceState))
            {
#if !ACPI_SUPPORT
                // Prevent the device/bridge from initiating/forwarding memory or I/O requests
                // during pause handling.
                if (op == kConfigOpPaused)
                {
                    entry->pausedCommand = configRead16(entry, kIOPCIConfigCommand);
                    configWrite16(entry, kIOPCIConfigCommand, entry->pausedCommand & ~kIOPCICommandBusLead);
                    entry->commandSaved = true;
                }
#endif
                entry->deviceState |= kPCIDeviceStatePaused;
                if (fStates & kIOPCIConfiguratorMPSOverride)
                {
                    fStates |= kIOPCIConfiguratorMPSOverridePause;
                }
            }
            ret = kIOReturnSuccess;
            break;

        case kConfigOpUnpaused:
            if (kPCIDeviceStatePaused & entry->deviceState)
            {
#if !ACPI_SUPPORT
                if (entry->commandSaved)
                {
                    configWrite16(entry, kIOPCIConfigCommand, entry->pausedCommand);
                    entry->commandSaved = false;
                }
#endif
                entry->deviceState &= ~kPCIDeviceStatePaused;
                if (fStates & kIOPCIConfiguratorMPSOverridePause)
                {
                    fStates &= ~kIOPCIConfiguratorMPSOverridePause;
                }
            }
            ret = kIOReturnSuccess;
            break;

        case kConfigOpTestPause:
            ret = kIOReturnSuccess;
            break;

        case kConfigOpScan:
            if (kPCIDeviceStateScanned & entry->deviceState)
            {
                *((void **)arg) = NULL;
                ret = kIOReturnSuccess;
                return (ret);
                break;
            }

            // always reset regs due to hotp interrupt clearing memory ranges
            entry->rangeBaseChanges |= ((1 << kIOPCIRangeBridgeMemory)
                                      |  (1 << kIOPCIRangeBridgePFMemory));

            // fall thru

        case kConfigOpRealloc:

            fChangedServices = OSSet::withCapacity(8);
            configure(0);

            if (entry)
            {
                entry->deviceState &= ~kPCIDeviceStateLinkInt;
            }

            if (arg)
            {
                *((void **)arg) = fChangedServices;
            }
            else
            {
                fChangedServices->release();
            }
            fChangedServices = 0;
            ret = kIOReturnSuccess;
            return (ret);
            break;


        case kConfigOpEject:
            entry->deviceState |= kPCIDeviceStateEjected;
            ret = kIOReturnSuccess;
            break;

        case kConfigOpKill:
            entry->deviceState |= kPCIDeviceStateToKill;
            ret = kIOReturnSuccess;
            break;

        case kConfigOpProtect:
            entry->deviceState = (entry->deviceState & ~(kPCIDeviceStateConfigRProtect|kPCIDeviceStateConfigWProtect))
                                | (*((uint32_t *) arg));


            ret = kIOReturnSuccess;
            break;
    }

    return (ret);
}

bool IOPCIConfigurator::bridgeConstructDeviceTree(void * unused, IOPCIConfigEntry * bridge)
{
    IORegistryEntry *   dtBridge = bridge->dtNub;
    uint32_t            int32;
    bool                ok = true;
    VMDChildDevice       * nub;
    OSDictionary      * initFrom;

//    DLOG("bridgeConstructDeviceTree(%p)\n", bridge);
    if (!dtBridge) return (ok);

    int32 = 3;
    dtBridge->setProperty("#address-cells", &int32, sizeof(int32));
    int32 = 2;
    dtBridge->setProperty("#size-cells", &int32, sizeof(int32));

    // Create missing device-tree entries for any child devices.

    FOREACH_CHILD( bridge, child )
    {
        VMDChildDevice     * pciDevice;
        IORegistryEntry * copyReg;
        OSDictionary    * propTable;
        OSObject        * obj;
        const OSSymbol  * sym;
        bool              addProps;
        bool              initDT;

        if (kPCIDeviceStatePropertiesDone & child->deviceState) continue;
        child->deviceState |= kPCIDeviceStatePropertiesDone;


        propTable = constructProperties(child);
        if (!propTable) continue;

#if ACPI_SUPPORT
        if (child->acpiDevice)
        {
            OSObject  *    obj;
            OSDictionary * mergeProps;
            obj = child->acpiDevice->copyProperty(kACPIDevicePropertiesKey);
            if (obj)
            {
                if ((mergeProps = OSDynamicCast(OSDictionary, obj)))
                    propTable->merge(mergeProps);
                obj->release();
            }
        }
#endif /* ACPI_SUPPORT */

        pciDevice = OSDynamicCast(VMDChildDevice, child->dtNub);
        addProps = true;
        if (!pciDevice)
        {
            nub = OSTypeAlloc(VMDChildDevice);
            if (!nub) continue;
            initFrom = 0;
            initDT = false;
            ok = false;
            if (child->dtEntry)
            {
#if ACPI_SUPPORT
                ok = nub->init(child->dtEntry, gIODTPlane);
                if (ok) child->deviceState |= kPCIDeviceStateAttached;
                child->dtNub = child->dtEntry = 0;
                initDT = true;
#else  /* !ACPI_SUPPORT */
                initFrom = child->dtEntry->dictionaryWithProperties();
#endif /* !ACPI_SUPPORT */
            }
            if (!initDT)
            {
                if (!initFrom)
                {
                    initFrom = propTable;
                    initFrom->retain();
                    addProps = false;
                }
                ok = (nub->init(initFrom) && nub->attachToParent(dtBridge, gIODTPlane));
                if (ok) child->deviceState |= kPCIDeviceStateAttached;
                initFrom->release();
            }
            nub->release();
            if (!ok) continue;
            nub->reserved->configEntry = child;
            nub->reserved->hostBridge = child->hostBridge;
            nub->reserved->domainId = fDomainId;
            child->dtNub = nub;
            // an endpoint in the VMD domain is demux-owned from the
            // moment its nub exists, so no client can ever register its
            // interrupt through the stock (CPU-vector) path.  It is also
            // Created with "pci-deferred-registration": Apple's
            // IOPCI2PCIBridge on the root port still attaches it (config
            // access needs the parent bridge) but does not register it for
            // matching; VMDDriver::start() does that after
            // postScanConfigure(), so the client driver never starts on a
            // half-configured domain.
            if (pvmd && pvmd->isDemuxMode() && !child->isBridge
                && pvmd->ownsConfigEntry(child)) {
                bool claimed = pvmd->addDemuxChild(nub);
                nub->setProperty("VMD-INT-Demux-AtCreate", claimed);
                if (claimed) {
                    nub->fRegDeferred = true;
                    nub->setProperty("pci-deferred-registration", kOSBooleanTrue);
                }
            }
#if ACPI_SUPPORT
            child->dtEntry = nub;
            copyReg = child->acpiDevice;
#else /* !ACPI_SUPPORT */
            copyReg = child->dtEntry;
#endif /* !ACPI_SUPPORT */

            if (child->dtEntry && (child->dtEntry != nub)) nub->setProperty(kIOPCIDeviceDeviceTreeEntryKey, child->dtEntry);
            if (!initDT)
            {
                if (copyReg && !copyReg->propertyExists("pci-generate-name"))
                {
                    if ((sym = copyReg->copyName()))
                    {
                        nub->setName(sym);
                        sym->release();
                    }
                }
                else if ((sym = OSDynamicCast(OSSymbol, propTable->getObject(gIONameKey))))
                    nub->setName(sym);
            }
        }

        if (addProps)
        {
            // update existing device
            OSCollectionIterator * propIter =
                OSCollectionIterator::withCollection(propTable);
            if (propIter)
            {
                const OSSymbol * propKey;

                while ((propKey = (const OSSymbol *)propIter->getNextObject()))
                {
                    if (   !child->dtNub->getProperty(propKey)
                        || (propKey->isEqualTo("name") && child->dtNub->propertyExists("pci-generate-name"))
                        || (propKey->isEqualTo("ranges"))
                        || (propKey->isEqualTo("reg"))
                        || (propKey->isEqualTo("assigned-addresses"))
                        || (propKey->isEqualTo("pcidebug")))
                    {
                        obj = propTable->getObject(propKey);
                        child->dtNub->setProperty(propKey, obj);
                    }
                }
                propIter->release();
            }
        }

        if (pciDevice
          && (kPCIDeviceStatePaused & child->deviceState)
            && pciDevice->getProperty(kIOPCIResourcedKey))
        {
            // relocate existing device
            for (uint32_t idx = 0; idx < kIOPCIRangeCount; idx++)
            {
                IOPCIRange * range;
                range = child->ranges[idx];
                if (!range)    continue;

            }
            pciDevice->relocate(false);
        }
        propTable->release();
    }
    dtBridge->setProperty(kIOPCIConfiguredKey, kOSBooleanTrue);

    return (ok);
}

IOReturn IOPCIBridge::configOp(configOpParams *params)
{
    IOReturn       ret = kIOReturnSuccess;
    IOPCIDevice   *probeDevice = nullptr;
    OSSet *        changed = nullptr;
    IOPCIDevice *  next = nullptr;
    uint32_t       state;
    IOPCIHostBridgeData *vars = reserved->hostBridgeData;

    if (params->op == kConfigOpPaused)
    {
        probeDevice = OSDynamicCast(IOPCIDevice, params->device);
        assert(probeDevice);
    }

    // Make sure we are running in gate.
    if (!vars->_configWorkLoop->inGate())
    {
        return (vars->_configWorkLoop->runAction(
                        OSMemberFunctionCast(IOCommandGate::Action, this, &IOPCIBridge::configOp),
                        this, params));
    }
    
    if (kConfigOpScan != params->op)
    {
        ret = vars->_configurator->configOp(params->device, params->op, params->result, params->arg);
        if (kIOReturnSuccess != ret) return (ret);

        next = (IOPCIDevice *) params->device;
        if (kConfigOpTerminated == params->op)
        {
            vars->_waitingPauseSet->removeObject(next);
            vars->_pausedSet->removeObject(next);
            vars->_probeSet->removeObject(next);
            vars->_publishSet->removeObject(next);
        }
        else if (kConfigOpTestPause == params->op)
        {
            if (vars->_waitingPauseSet->setObject(next))
            {
                next->changePowerStateToPriv(kIOPCIDevicePausedState);
                next->powerOverrideOnPriv();
            }
        }
        else if (kConfigOpUnpaused == params->op)
        {
            vars->_pausedSet->removeObject(next);
        }

        if (params->op != kConfigOpPaused)
        {
            params->op = 0;
        }
        else
        {
            params->op = 0;

            if (vars->_waitingPauseSet->containsObject(params->device))
            {
                vars->_pausedSet->setObject(params->device);
                vars->_waitingPauseSet->removeObject(params->device);
                if (!vars->_waitingPauseSet->getCount())
                {
                    params->op = kConfigOpRealloc;
                }
            }
        }
    }

    while (params->op)
    {
        ret = vars->_configurator->configOp(params->device, params->op, &changed);
        params->op = 0;
        if (kIOReturnSuccess != ret) break;
        if (!changed) break;

        while ((next = (IOPCIDevice *) changed->getAnyObject()))
        {
            ret = vars->_configurator->configOp(next, kConfigOpGetState, &state);
            if (kIOReturnSuccess == ret)
            {
                if (kPCIDeviceStateDead & state)
                {
                    next->terminate();
                }
                else if (kPCIDeviceStateRequestPause & state)
                {
                    if (vars->_waitingPauseSet->setObject(next))
                    {
                        // initiatePause() starts the pause IOPM transition if the nub and its children have
                        // finished initializing, else it defers the transition for (at least) another 100ms.
                        next->initiatePause();
                    }
                }
                else
                {
                    vars->_probeSet->setObject(next);
                }
            }
            changed->removeObject(next);
        }
        changed->release();
    }

    if (!vars->_waitingPauseSet->getCount())
    {
        vars->_pausedSet->iterateObjects(^bool(OSObject *object)
        {
            IOPCIDevice *next = OSDynamicCast(IOPCIDevice, object);
            if (2 != next->reserved->pauseFlags)
            {
                next->changePowerStateToPriv(kIOPCIDeviceOnState);
                next->powerOverrideOffPriv();
            }
            next->reserved->pauseFlags = 0;
            return false;
        });
        while ((next = (IOPCIDevice *) vars->_probeSet->getAnyObject()))
        {
            vars->_publishSet->setObject(next);
            if (kIOPCIDeviceOnState == next->reserved->pciPMState && next != probeDevice) deferredProbe(next);
            else                                     next->reserved->needsProbe = true;
            vars->_probeSet->removeObject(next);
        }
    }

    return (ret);
}
