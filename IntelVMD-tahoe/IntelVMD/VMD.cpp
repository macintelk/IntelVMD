

#include "VMD.hpp"

OSDefineMetaClassAndStructors(VMDDriver, IOPCI2PCIBridge)
OSDefineMetaClassAndStructors(VMDChildDevice, IOPCIDevice)

/* --------------------------------------------------------------------------
 * VMDACPIDevice — OSDynamicCast(IOACPIPlatformDevice, ...) replacement.
 *
 * OSDynamicCast against IOACPIPlatformDevice emits a reference to
 * IOACPIPlatformDevice::metaClass, which AppleACPIPlatform does not export
 * to third-party kexts (kmutil: "could not find a kext which exports this
 * symbol").  Every object passed here comes from a source that IOPCIFamily /
 * AppleACPIPlatform itself populated (IOPCI2PCIBridge::acpiDevice, the
 * "acpi-device" property published on PCI nubs), so the IORegistryEntry
 * base class is checked at runtime and the pointer is then cast through —
 * the chain IOACPIPlatformDevice : IOService : IORegistryEntry : OSObject
 * is single-inheritance, so the pointer value is unchanged.
 * ------------------------------------------------------------------------ */
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
struct VMDBarInfo {
    uint64_t base;
    uint64_t size;
    bool     valid;
};

static bool vmdParseBars(IORegistryEntry *provider, VMDBarInfo *bars)
{
    OSData *assigned = OSDynamicCast(OSData, provider->getProperty("assigned-addresses"));
    if (!assigned) return false;

    const uint32_t *reg = (const uint32_t *) assigned->getBytesNoCopy();
    unsigned int    count = assigned->getLength() / (5 * sizeof(UInt32));
    bool            any = false;

    for (unsigned int i = 0; i < count; i++, reg += 5) {
        IOPCIAddressSpace hi;
        hi.bits = OSSwapLittleToHostInt32(reg[0]);
        uint64_t base = ((uint64_t) OSSwapLittleToHostInt32(reg[1]) << 32)
                      | OSSwapLittleToHostInt32(reg[2]);
        uint64_t len  = ((uint64_t) OSSwapLittleToHostInt32(reg[3]) << 32)
                      | OSSwapLittleToHostInt32(reg[4]);

        if (hi.s.space == kIOPCIConfigSpace) continue;

        uint32_t barIndex = (hi.s.registerNum - kIOPCIConfigBaseAddress0) / 4;
        if (barIndex > 5) continue;

        for (unsigned int b = 0; b < 3; b++) {
            static const uint32_t vmdBarRegs[3] = {
                VMD_CFGBAR * 4, VMD_MEMBAR1 * 4, VMD_MEMBAR2 * 4 };
            if (hi.s.registerNum == kIOPCIConfigBaseAddress0 + vmdBarRegs[b]) {
                bars[b].base  = base;
                bars[b].size  = len;
                bars[b].valid = (len != 0);
                any |= bars[b].valid;
            }
        }
    }
    return any;
}

// Publish reg/assigned-addresses/IODeviceMemory for the VMD device from the
// decoded windows (same properties Apple's stock enumeration would create).
static void vmdPublishBars(IOService *provider, const VMDBarInfo *bars)
{
    IOPCIAddressSpace space = ((IOPCIDevice *)provider)->space;

    OSData *regProp = OSData::withCapacity(64);
    OSData *asgProp = OSData::withCapacity(64);
    if (!regProp || !asgProp) {
        if (regProp) regProp->release();
        if (asgProp) asgProp->release();
        return;
    }

    // config-space tuple first (like the stock "reg" property)
    IOPCIAddressSpace cfg = space;
    cfg.s.space = kIOPCIConfigSpace;
    cfg.s.registerNum = 0;
    uint32_t tuple[5] = { cfg.bits, 0, 0, 0, 0 };
    regProp->appendBytes(tuple, sizeof(tuple));

    IODeviceMemory::InitElement rangeList[3];
    uint32_t                    nRanges = 0;

    static const uint32_t vmdBarRegs[3] = {
        VMD_CFGBAR * 4, VMD_MEMBAR1 * 4, VMD_MEMBAR2 * 4 };

    for (uint32_t b = 0; b < 3; b++) {
        if (!bars[b].valid) continue;

        IOPCIAddressSpace as = space;
        as.s.space       = (bars[b].base >> 32) ? kIOPCI64BitMemorySpace
                                                : kIOPCI32BitMemorySpace;
        as.s.registerNum = kIOPCIConfigBaseAddress0 + vmdBarRegs[b];
        as.s.reloc       = 1;

        uint32_t e[5] = {
            as.bits,
            (uint32_t)(bars[b].base >> 32),
            (uint32_t)(bars[b].base),
            (uint32_t)(bars[b].size >> 32),
            (uint32_t)(bars[b].size) };
        regProp->appendBytes(e, sizeof(e));
        asgProp->appendBytes(e, sizeof(e));

        rangeList[nRanges].start  = bars[b].base;
        rangeList[nRanges].length = bars[b].size;
        rangeList[nRanges].tag    = as.bits;
        nRanges++;
    }

    provider->removeProperty("reg");
    provider->removeProperty("assigned-addresses");
    provider->removeProperty("IODeviceMemory");
    provider->setProperty("reg", regProp);
    if (asgProp->getLength()) provider->setProperty("assigned-addresses", asgProp);
    regProp->release();
    asgProp->release();

    if (nRanges) {
        OSArray *memoryArray = IODeviceMemory::arrayFromList(rangeList, nRanges);
        if (memoryArray) {
            provider->setDeviceMemory(memoryArray);
            memoryArray->release();
        }
        provider->removeProperty("IOPCIResourced");
        ((IOPCIDevice *)provider)->getResources();
    }
}

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

    IOSimpleLockLock(fCfgLock);
    UInt32 value = OSReadLittleInt32(fCfgBar, addr);
    IOSimpleLockUnlock(fCfgLock);
    return value;
}

void VMDDriver::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configWrite32(space, offset, data);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 4);
    if (addr == 0xFFFFFFFF) return;

    IOSimpleLockLock(fCfgLock);
    OSWriteLittleInt32(fCfgBar, addr, data);
    OSReadLittleInt32(fCfgBar, addr);   // force completion (posted writes)
    IOSimpleLockUnlock(fCfgLock);
}

UInt16 VMDDriver::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configRead16(space, offset);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 2);
    if (addr == 0xFFFFFFFF) return 0xFFFF;

    IOSimpleLockLock(fCfgLock);
    UInt16 value = OSReadLittleInt16(fCfgBar, addr);
    IOSimpleLockUnlock(fCfgLock);
    return value;
}

void VMDDriver::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configWrite16(space, offset, data);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 2);
    if (addr == 0xFFFFFFFF) return;

    IOSimpleLockLock(fCfgLock);
    OSWriteLittleInt16(fCfgBar, addr, data);
    OSReadLittleInt16(fCfgBar, addr);
    IOSimpleLockUnlock(fCfgLock);
}

UInt8 VMDDriver::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configRead8(space, offset);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 1);
    if (addr == 0xFFFFFFFF) return 0xFF;

    IOSimpleLockLock(fCfgLock);
    UInt8 value = *(volatile uint8_t *)(fCfgBar + addr);
    IOSimpleLockUnlock(fCfgLock);
    return value;
}

void VMDDriver::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
    if (!fCfgBar || space.s.busNum < fBusnStart)
        return IOPCI2PCIBridge::configWrite8(space, offset, data);

    uint32_t addr = ecamOffset(fBusnStart, fCfgBarSize, space, offset, 1);
    if (addr == 0xFFFFFFFF) return;

    IOSimpleLockLock(fCfgLock);
    *(volatile uint8_t *)(fCfgBar + addr) = data;
    *(volatile uint8_t *)(fCfgBar + addr);
    IOSimpleLockUnlock(fCfgLock);
}

/* VMDChildDevice — same ECAM, always through the owning VMDDriver.
 * A VMDChildDevice never lives on the host bus; if it somehow appears to,
 * fail the access rather than touching hardware that is not ours. */

UInt32 VMDChildDevice::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
    if (!pvmd) return 0xFFFFFFFF;
    return pvmd->configRead32(space, offset);
}

void VMDChildDevice::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
    if (pvmd) pvmd->configWrite32(space, offset, data);
}

UInt16 VMDChildDevice::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
    if (!pvmd) return 0xFFFF;
    return pvmd->configRead16(space, offset);
}

void VMDChildDevice::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
    if (pvmd) pvmd->configWrite16(space, offset, data);
}

UInt8 VMDChildDevice::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
    if (!pvmd) return 0xFF;
    return pvmd->configRead8(space, offset);
}

void VMDChildDevice::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
    if (pvmd) pvmd->configWrite8(space, offset, data);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Capability walks.
 *
 * Convention (Apple IOPCIFamily): for extended capabilities the caller
 * passes the NEGATED capability ID (>= 0x100); the walk goes through the
 * 12-bit extended capability chain.  The full offset rides in
 * es.registerNumExtended and is applied by ecamFullOffset() inside the
 * config accessors above.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

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
        return false;
    }

    uint16_t ctrl = fPCIDev->configRead16(cap + 2);
    fMsixCount = (ctrl & 0x7FF) + 1;

    IOReturn ret = fPCIDev->configureInterrupts(kIOInterruptTypePCIMessagedX,
                                                fMsixCount, fMsixCount);
    if (ret != kIOReturnSuccess)
        IOLog("IntelVMD: configureInterrupts(MSIX x%u) -> 0x%x\n",
              fMsixCount, ret);

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
    fPCIDev->configWrite16(cap + 2, ctrl);

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

void VMDDriver::configure1( IOService * provider )
{
    //setProperty("domain-id", 0x10000,32);
    
    IOPCIDevice* pciDevice = OSDynamicCast(IOPCIDevice, provider);
    
    if (!pciDevice) return;
    
    uint16_t reg0;
    uint16_t busStart,busEnd;
    
    reg0=pciDevice->configRead16(   PCI_REG_VMCAP);
    if (BUS_RESTRICT_CAP(reg0)) {
        reg0=pciDevice->configRead16( PCI_REG_VMCONFIG);
        
        switch (BUS_RESTRICT_CFG(reg0)) {
            case 0:
                busStart = 0;
                break;
            case 1:
                busStart = 128;
                break;
            case 2:
                busStart = 224;
                break;
            default:
                busStart=0;
        }
    }
    
    busEnd=0xFF;
    
    
    bool enable=true;
    reg0=pciDevice->configRead16( PCI_REG_VMCONFIG);
    reg0 = enable ? (reg0 & ~VMCONFIG_MSI_REMAP) :
    (reg0 | VMCONFIG_MSI_REMAP);
    pciDevice->configWrite16( PCI_REG_VMCONFIG, reg0);
    
    
    IOPCIAddressSpace space;
    space.s.busNum = busStart;
    
    IOPCIAddressSpace aBridgeSpace=space;
    
    provider->removeProperty("reg");
    provider->removeProperty("assigned-addresses");
    provider->removeProperty("IODeviceMemory");
    
    IOPCIAddressSpace addrSpace=pciDevice->space;
    UInt64 address;
    UInt64 length;
    
    
    address=0;
    length=0;
    addrSpace.s.space=kIOPCIConfigSpace;
    addrSpace.s.registerNum=0x0;
    configurePCISpace("reg", provider,addrSpace,address,length);
    
    address=0;
    length=0x2000000;
    addrSpace.s.space=kIOPCI64BitMemorySpace;
    addrSpace.s.registerNum=0x10;
    configurePCISpace("reg", provider,addrSpace,address,length);
    
    /*address=0;
     length=0;
     addrSpace.s.space=kIOPCI32BitMemorySpace;
     addrSpace.s.registerNum=0x14;
     configurePCISpace("reg", provider,addrSpace,address,length);*/
    
    address=0;
    length=0x2000000;
    addrSpace.s.space=kIOPCI32BitMemorySpace;
    addrSpace.s.registerNum=0x18;
    configurePCISpace("reg", provider,addrSpace,address,length);
    
    /*address=0;
     length=0;
     addrSpace.s.space=kIOPCI32BitMemorySpace;
     addrSpace.s.registerNum=0x1c;
     configurePCISpace("reg", provider,addrSpace,address,length);*/
    
    address=0;
    length=0x100000;
    // length=0x100000-0x2000;
    addrSpace.s.space=kIOPCI64BitMemorySpace;
    addrSpace.s.registerNum=0x20;
    configurePCISpace("reg", provider,addrSpace,address,length);
    
    /*address=0;
     length=0;
     addrSpace.s.space=kIOPCI32BitMemorySpace;
     addrSpace.s.registerNum=0x24;
     configurePCISpace("reg", provider,addrSpace,address,length);*/
    
    
    IODeviceMemory::InitElement rangeList[3];
    
    address=0x6000000000;
    length=0x2000000;
    addrSpace.s.space=kIOPCI64BitMemorySpace;
    addrSpace.s.registerNum=0x10;
    configurePCISpace("assigned-addresses", provider,addrSpace,address,length);
    
    rangeList[0] =
    {
        .start = address ,
        .length = length,
        .tag = addrSpace.bits
    };
    
    address=0x50000000;
    length=0x2000000;
    addrSpace.s.space=kIOPCI32BitMemorySpace;
    addrSpace.s.registerNum=0x18;
    configurePCISpace("assigned-addresses", provider,addrSpace,address,length);
    
    rangeList[1] =
    {
        .start = address ,
        .length = length,
        .tag = addrSpace.bits
    };
    
    address=0x6003100000;
    length=0x100000;
    // address=0x6003100000+0x2000;
    // length=0x100000-0x2000;
    addrSpace.s.space=kIOPCI64BitMemorySpace;
    addrSpace.s.registerNum=0x20;
    configurePCISpace("assigned-addresses", provider,addrSpace,address,length);
    
    rangeList[2] =
    {
        .start = address ,
        .length = length,
        .tag = addrSpace.bits
    };
    
    OSArray *memoryArray = IODeviceMemory::arrayFromList(rangeList, 3);
    if (memoryArray) {
        //provider->setProperty("IODeviceMemory",memoryArray);
        provider->setDeviceMemory(memoryArray);
        memoryArray->release();
    }

    /* ------------------------------------------------------------------
     * Fix Apple's misprogrammed VMD BARs BEFORE the domain scan
     * ("fix apple wrong 32 bit bars at start so scan can work").
     *
     * Values are the BIOS-assigned windows from the Linux boot log of this
     * machine (8086:9a0b):
     *   pci 0000:00:0e.0: reg 0x10: [mem 0x6000000000-0x6001ffffff 64bit]
     *   pci 0000:00:0e.0: reg 0x18: [mem 0x50000000-0x51ffffff]
     *   pci 0000:00:0e.0: reg 0x20: [mem 0x6003100000-0x60031fffff 64bit]
     *
     * Apple writes the 64-bit BARs as if they were 32-bit (wrong/absent
     * low dwords) and the child memory window (BAR2/MEMBAR1) must decode
     * at 0x50000000 because the domain root bus resource IS that window
     * (1:1 translation).  Rewrite every dword: BAR0 lo/hi, BAR2 (the real
     * 32-bit MEMBAR1), BAR3 (unused), BAR4 lo/hi.
     * ------------------------------------------------------------------ */
    struct VMDBarFix { uint8_t reg; uint32_t value; const char *name; };
    const VMDBarFix fixes[] = {
        { kIOPCIConfigBaseAddress0,     0x00000000, "BAR0 lo (CFG BAR hi=0x60)" },
        { kIOPCIConfigBaseAddress0 + 4, 0x00000060, "BAR0 hi" },
        { kIOPCIConfigBaseAddress2,     0x50000000, "BAR2 lo (MEMBAR1 32MB)" },
        { kIOPCIConfigBaseAddress2 + 4, 0x00000000, "BAR3 (unused)" },
        { kIOPCIConfigBaseAddress4,     0x03100000, "BAR4 lo (MEMBAR2 hi=0x60)" },
        { kIOPCIConfigBaseAddress4 + 4, 0x00000060, "BAR4 hi" },
    };
    for (unsigned i = 0; i < sizeof(fixes) / sizeof(fixes[0]); i++) {
        uint32_t old = pciDevice->configRead32(fixes[i].reg);
        if (old != fixes[i].value) {
            pciDevice->configWrite32(fixes[i].reg, fixes[i].value);
            IOLog("IntelVMD: fix host BAR reg 0x%02x: 0x%08x -> 0x%08x (%s)\n",
                  fixes[i].reg, old, fixes[i].value, fixes[i].name);
        }
    }
}

bool VMDDriver::vmdProbe(IOService *provider)
{
    fPCIDev = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDev) {
        IOLog("IntelVMD: provider is not an IOPCIDevice\n");
        return false;
    }

    configure1(provider);
    
    fPCIDev->retain();
    fPCIDev->open(this);
    fPCIDev->setMemoryEnable(true);
    fPCIDev->setBusMasterEnable(true);

    // vmd_ids[] match (informational — Info.plist already gated matching)
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

    if (!setupMSIX()) return false;

    // force the demux model: child MSIs land in the VMD MSI-X table
    setMSIRemapping(true);

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

/* decode + publish + map the VMD device's own BARs (CFGBAR/MEMBAR1/MEMBAR2).
 * Sizes and bases come from the BIOS-assigned "assigned-addresses" property
 * of the provider nub — no hardcoded addresses. */
bool VMDDriver::decodeAndPublishBars(void)
{
    VMDBarInfo bars[3] = {};
    if (!vmdParseBars(fPCIDev, bars)) {
        IOLog("IntelVMD: provider has no assigned-addresses; "
              "cannot decode VMD windows\n");
        return false;
    }
    fCfgBarSize = bars[0].size;

    vmdPublishBars(fPCIDev, bars);

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
    if (!fCfgMap) {
        IOLog("IntelVMD: cannot map CFGBAR\n");
        return false;
    }
    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * VMDDriver::start — entry point (Linux vmd_probe() + vmd_enable_domain()).
 *
 * macOS glue order:
 *   1. vmdProbe()            — hardware bring-up (Linux probe)
 *   2. seedBridgeEntry()     — describe the root complex to IOPCIFamily
 *   3. IOPCI2PCIBridge::start() — stock Apple bridge start; creates the
 *                              messaged-interrupt controller (the VMD
 *                              irq_domain equivalent). This kext does NOT
 *                              override IOPCI2PCIBridge::start, so the
 *                              qualified call binds to the real IOPCIFamily
 *                              implementation.
 *   4. kConfigOpScan         — pci_scan_child_bus(): enumerates the VMD
 *                              buses through our ECAM config accessors and
 *                              builds VMDChildDevice nubs via createNub().
 *   5. postScanConfigure()   — ACPI association + LTR quirk + MSI wiring.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
bool VMDDriver::start(IOService *provider)
{
    if (!vmdProbe(provider)) {
        IOLog("IntelVMD: probe failed\n");
        return false;
    }
    if (!seedBridgeEntry()) {
        IOLog("IntelVMD: cannot seed the bridge config entry\n");
        return false;
    }

    bool ok = IOPCI2PCIBridge::start(provider);
    if (!ok) {
        IOLog("IntelVMD: IOPCI2PCIBridge::start failed\n");
        return false;
    }

    fPCIDev->setPowerState(kIOPCIDeviceOnState, this);

    // pci_scan_child_bus()
    configOpParams cp;
    cp.device = fPCIDev;
    cp.op     = kConfigOpScan;
    cp.result = NULL;
    configOp(&cp);

    postScanConfigure();

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
    if (selfAcpi) {
        char path[512];
        bzero(path, sizeof(path));
        int s=sizeof(path);
        if (selfAcpi->getPath(path, &s, gIOServicePlane) && path[0])
            bridge->dtEntry->setProperty(kACPIDevicePathKey,
                                         OSString::withCString(path));
        bridge->dtEntry->setProperty("acpi-device", selfAcpi);
    }

    // Intel VMD _DSM function 5 (VD3A) — keep D3 power management enabled.
    if (selfAcpi) {
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
    }

    // Save the VMD device's config registers into the IOPCIFamily shadow
    // (used by saveDeviceState/restoreDeviceState across sleep).
    IOPCIConfigShadow *shadow = configShadow(fPCIDev);
    for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
        shadow->configSave.savedConfig[i] = fPCIDev->configRead32(i * 4);

    return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * postScanConfigure — per-device work after pci_scan_child_bus():
 *
 *   1. vmd_domain_reset() — clear stale bridge windows (resetDomain()).
 *   2. pci_assign_unassigned_bus_resources() + pci_setup_bridge() —
 *      assignDomainResources(): program every BAR behind the VMD, re-open
 *      each bridge's memory window around its children's live BARs, set
 *      bus numbers and enable decode.  THIS STAGE IS WHAT MAKES CHILD MMIO
 *      REACHABLE: resetDomain() alone leaves every window closed, config
 *      space keeps working (it goes through the CFG BAR ECAM), and every
 *      memory read returns all-1s — the "( CAP !=
 *      kNVMeInvalidRegisterValue64 )" signature.
 *   3. vmd_acpi_find_companion() for every device behind the VMD, so
 *      Apple's IONVMeFamily finds acpiRootDevice (the "big issue").
 *   4. vmd_pm_enable_quirk() — driver-default LTR values.
 *   5. child MSI wiring onto the VMD's own MSI-X vectors (irq demux).
 *   6. CAP probe on NVMe endpoints — reads NVMe CAP (BAR0+0) through a
 *      fresh mapping and logs whether the decode chain is alive.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::postScanConfigure(void)
{
    if (!fBridgeEntry) return;

    // Linux vmd_enable_domain() order: pci_scan_child_bus() [done by
    // kConfigOpScan] -> vmd_domain_reset() -> pci_assign_unassigned_bus_
    // resources() -> per-device setup.
    resetDomain();
    assignDomainResources();

    for (IOPCIConfigEntry *child = fBridgeEntry->child; child; child = child->peer) {
        IOPCIDevice *nub = OSDynamicCast(IOPCIDevice, child->dtNub);
        if (!nub) continue;

        // ACPI companion (vmd_acpi_find_companion)
        associateACPI(child, NULL);

        // vmd_pm_enable_quirk()
        enableLTRQuirk(nub);

        wireChildMSI(nub);


        if (child->isBridge) {
            IOPCIDevice *nub2 =
                OSDynamicCast(IOPCIDevice, nub->getChildEntry(gIODTPlane));
            if (!nub2) continue;
            IOPCIConfigEntry *child2 = VMDChildDevice::configEntryOf(nub2);
            if (!child2) continue;

            // Endpoints behind a VMD root port normally have no ACPI node
            // of their own (BIOS only declares the root ports).  Apple's
            // IONVMeController attaches to the endpoint nub and its
            // InitializeController() asserts "( 0 != acpiRootDevice )" via
            // CopyACPIParent() when the ssd-off-in-S4 flag is set — make
            // the endpoint nub inherit the root port's association so the
            // copyProperty("acpi-path", IterateRecursively|IterateParents)
            // lookup always resolves.
            associateACPI(child2, nub);

            enableLTRQuirk(nub2);
            wireChildMSI(nub2);

            // pci_enable_device() — memory decode + bus mastering.
            uint16_t cmdBefore = nub2->configRead16(kIOPCIConfigCommand);
            nub2->setBusMasterEnable(true);
            nub2->setMemoryEnable(true);
            uint16_t cmdAfter = nub2->configRead16(kIOPCIConfigCommand);
            IOLog("IntelVMD: endpoint %02x:%02x.%x cmd 0x%04x -> 0x%04x\n",
                  nub2->space.s.busNum, nub2->space.s.deviceNum,
                  nub2->space.s.functionNum, cmdBefore, cmdAfter);

            IOLog("IntelVMD: endpoint %02x:%02x.%x configured\n",
                  child2->space.s.busNum, child2->space.s.deviceNum,
                  child2->space.s.functionNum);

            // Decisive check: read the NVMe CAP register (BAR0+0) through a
            // fresh mapping.  "DECODE OK" here means the whole
            // VMD -> root port -> endpoint memory path is alive and any
            // later IONVMeFamily assert is a mapping/ownership issue.
            capProbeDiagnostic(nub2);
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * pci_assign_unassigned_bus_resources() — port.
 *
 * Linux vmd_enable_domain():  pci_scan_child_bus() -> vmd_domain_reset()
 * -> pci_assign_unassigned_bus_resources().  The VMD translates memory
 * 1:1, so a child BAR is programmed with a plain host address inside the
 * MEMBAR1 window (Linux dmesg of this machine:
 *   pci 10000:e1:00.0: BAR 0: assigned [mem 0x50000000-0x50003fff 64bit]
 *   pci 10000:e0:1c.4: bridge window [mem 0x50000000-0x500fffff]).
 * Valid existing assignments (BIOS or IOPCIFamily, e.g. the 1 MB-aligned
 * 0x50100000 IOPCIFamily gives the NVMe) are kept like Linux keeps
 * BIOS-configured endpoints; stale/out-of-window values are reprogrammed
 * from a 1 MB-granular cursor at the window start.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::assignDomainResources(void)
{
    if (!fBridgeEntry) return;

    VMDAssignCtx ctx;
    bzero(&ctx, sizeof(ctx));
    for (uint32_t i = 0; i < fWindowCount; i++) {
        if (fWindows[i].type == kIOPCIResourceTypeMemory) {
            ctx.memWinStart = fWindows[i].start;
            ctx.memWinSize  = fWindows[i].size;
        } else if (fWindows[i].type == kIOPCIResourceTypePrefetchMemory) {
            ctx.pfWinStart  = fWindows[i].start;
            ctx.pfWinSize   = fWindows[i].size;
        }
    }
    if (!ctx.memWinSize) {
        IOLog("IntelVMD: assign: MEMBAR1 window unknown, cannot assign\n");
        return;
    }
    ctx.memCursor = ctx.memWinStart;
    ctx.pfCursor  = ctx.pfWinStart;

    IOLog("IntelVMD: assign: mem window 0x%llx-0x%llx, pf window 0x%llx-0x%llx\n",
          (unsigned long long)ctx.memWinStart,
          (unsigned long long)(ctx.memWinStart + ctx.memWinSize - 1),
          (unsigned long long)ctx.pfWinStart,
          (unsigned long long)(ctx.pfWinStart + ctx.pfWinSize - 1));

    // Depth-first over every device behind the VMD.  'this' is the root
    // bus nub (its DT children are the bus 0xE0 devices).
    assignBusDevices(this, &ctx);
}

void VMDDriver::assignBusDevices(IORegistryEntry *busNub, VMDAssignCtx *ctx)
{
    OSIterator *iter = busNub->getChildIterator(gIODTPlane);
    if (!iter) return;
    iter->reset();

    IORegistryEntry *entry;
    while ((entry = (IORegistryEntry *)iter->getNextObject())) {
        IOPCIDevice *nub = OSDynamicCast(IOPCIDevice, entry);
        if (!nub) continue;
        if (nub->configRead16(kIOPCIConfigVendorID) == 0xFFFF) continue;

        assignDeviceBars(nub, ctx);

        uint8_t hdr = nub->configRead8(PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
        if (hdr == PCI_HEADER_TYPE_BRIDGE) {
            // Depth first (pci_assign_unassigned_bus_resources walks each
            // bus bottom-up): a bridge's window is computed from its
            // children's addresses, so assign below it first.
            assignBusDevices(nub, ctx);
            configureBridgeWindows(nub, ctx);
        }
    }
    iter->release();
}

void VMDDriver::assignDeviceBars(IOPCIDevice *nub, VMDAssignCtx *ctx)
{
    uint8_t  hdr      = nub->configRead8(PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
    uint32_t barCount = (hdr == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;

    for (uint32_t b = 0; b < barCount; b++) {
        uint32_t reg = kIOPCIConfigBaseAddress0 + b * 4;
        uint32_t low = nub->configRead32(reg);
        if (low == 0xFFFFFFFF) continue;
        if (low & 0x1) continue;                        // I/O BAR: no I/O window in a VMD domain
        bool is64 = ((low & 0x6) == 0x4);
        bool isPF = (low & 0x8) != 0;
        if (is64 && b + 1 >= barCount) continue;        // would overlap bridge regs
        uint32_t idx = b;                               // cache index of this BAR

        uint64_t addr = low & 0xFFFFFFF0ULL;
        if (is64) addr |= ((uint64_t)nub->configRead32(reg + 4) << 32);
        if (is64) b++;      // next dword is this BAR's upper half, not another BAR

        // Size: trust the family-probed range when present, else probe the
        // hardware (Linux pci_size()).
        uint64_t size = 0;
        IODeviceMemory *fam = nub->getDeviceMemoryWithRegister(reg);
        if (fam) size = fam->getLength();
        if (!size) size = probeBarSize(nub, reg, is64);
        if (!size) continue;

        // Remember the size for the child's live-decode accessor — BAR sizes
        // are only probed here, while no driver is mapped to the device yet.
        VMDChildDevice *child = OSDynamicCast(VMDChildDevice, nub);
        if (child) child->fBarSize[idx] = size;

        uint64_t winStart = isPF ? ctx->pfWinStart : ctx->memWinStart;
        uint64_t winSize  = isPF ? ctx->pfWinSize  : ctx->memWinSize;
        bool inWin = winSize && addr
                  && addr >= winStart
                  && (addr - winStart) + size <= winSize;

        if (inWin) {
            // Valid assignment (BIOS or IOPCIFamily) — keep it.  Linux:
            // "Endpoints must be initially configured by platform BIOS"
            // (vmd_enable_device) — in-window BARs are never reprogrammed.
            ctx->keep(addr, size);
            IOLog("IntelVMD: %02x:%02x.%x BAR%u keep 0x%llx/0x%llx%s\n",
                  nub->space.s.busNum, nub->space.s.deviceNum,
                  nub->space.s.functionNum, idx,
                  (unsigned long long)addr, (unsigned long long)size,
                  is64 ? " 64bit" : "");
            continue;
        }

        // Unassigned or decodes outside the VMD windows (stale high dword
        // from a 32-bit BAR write, IOPCIFamily allocation elsewhere, or a
        // bus reset that cleared the BAR) — program it inside the window.
        uint64_t cand = allocFromWindow(ctx, size, isPF);
        if (!cand) {
            IOLog("IntelVMD: %02x:%02x.%x BAR%u: no space for size 0x%llx "
                  "(stale 0x%llx)\n",
                  nub->space.s.busNum, nub->space.s.deviceNum,
                  nub->space.s.functionNum, idx,
                  (unsigned long long)size, (unsigned long long)addr);
            continue;
        }

        uint32_t oldLo = low;
        uint32_t oldHi = is64 ? nub->configRead32(reg + 4) : 0;
        nub->configWrite32(reg, (uint32_t)(cand & 0xFFFFFFF0ULL));
        if (is64) nub->configWrite32(reg + 4, (uint32_t)(cand >> 32));
        ctx->keep(cand, size);
        IOLog("IntelVMD: %02x:%02x.%x BAR%u 0x%08x%08x -> 0x%08x%08x /0x%llx\n",
              nub->space.s.busNum, nub->space.s.deviceNum,
              nub->space.s.functionNum, idx,
              oldHi, oldLo,
              (uint32_t)(cand >> 32), (uint32_t)(cand & 0xFFFFFFF0ULL),
              (unsigned long long)size);
    }

    // Rebuild the nub's published IODeviceMemory so every later map (base
    // class getter, index getters, AppleNVMe) agrees with live hardware.
    refreshNubDeviceMemory(nub);
}

uint64_t VMDDriver::probeBarSize(IOPCIDevice *nub, uint32_t reg, bool is64)
{
    uint32_t oldLo = nub->configRead32(reg);
    uint32_t maskLo, maskHi = 0;

    if (!is64) {
        nub->configWrite32(reg, 0xFFFFFFF0);
        maskLo = nub->configRead32(reg);
        nub->configWrite32(reg, oldLo);
        if (maskLo == 0 || maskLo == 0xFFFFFFFF) return 0;
        uint32_t mask = maskLo & 0xFFFFFFF0U;
        return ((uint64_t)(~mask)) + 1;     /* complement in 32 bits, then widen */
    }

    uint32_t oldHi = nub->configRead32(reg + 4);
    nub->configWrite32(reg,     0xFFFFFFF0);
    nub->configWrite32(reg + 4, 0xFFFFFFFF);
    maskLo = nub->configRead32(reg);
    maskHi = nub->configRead32(reg + 4);
    nub->configWrite32(reg + 4, oldHi);
    nub->configWrite32(reg,     oldLo);

    uint64_t mask = ((uint64_t)maskHi << 32) | (maskLo & 0xFFFFFFF0ULL);
    if (!mask) return 0;
    return (~mask) + 1;
}

uint64_t VMDDriver::allocFromWindow(VMDAssignCtx *ctx, uint64_t size, bool isPF)
{
    uint64_t &cursor   = isPF ? ctx->pfCursor   : ctx->memCursor;
    uint64_t winStart  = isPF ? ctx->pfWinStart : ctx->memWinStart;
    uint64_t winSize   = isPF ? ctx->pfWinSize  : ctx->memWinSize;

    if (!winSize || !size) return 0;

    while (cursor + size <= winStart + winSize) {
        // Apple- and Linux-like 1 MB granularity; natural alignment for
        // anything larger.
        uint64_t align = size > 0x100000 ? size : 0x100000;
        uint64_t cand  = (cursor + align - 1) & ~(align - 1);
        if (cand + size > winStart + winSize) return 0;
        if (!ctx->overlaps(cand, cand + size)) {
            cursor = cand + size;
            return cand;
        }
        // Bump past the clashing kept allocation.
        uint64_t bump = 0;
        for (uint32_t i = 0; i < ctx->keptCount; i++)
            if (cand < ctx->kept[i].end && ctx->kept[i].start < cand + size)
                if (ctx->kept[i].end > bump) bump = ctx->kept[i].end;
        if (!bump) return 0;
        cursor = bump;
    }
    return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * pci_setup_bridge() — port.  Programs the bridge windows from the live
 * BAR addresses of everything behind the bridge, plus bus numbers and the
 * bridge command.  This is the stage vmd_domain_reset() depends on: the
 * reset closes all windows, this re-opens them.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::configureBridgeWindows(IOPCIDevice *bridge, VMDAssignCtx *ctx)
{
    uint64_t memMin = ~0ULL, memMax = 0, pfMin = ~0ULL, pfMax = 0;
    collectDescendantRanges(bridge, &memMin, &memMax, &pfMin, &pfMax);

    // Memory window — 1 MB granularity (PCI_MEMORY_BASE 0x20 / LIMIT 0x22,
    // registers hold address bits 31:20).
    if (memMax > memMin) {
        uint32_t base20 = (uint32_t)(memMin >> 20);
        uint32_t lim20  = (uint32_t)((memMax - 1) >> 20);
        bridge->configWrite16(PCI_MEMORY_BASE,     (uint16_t)base20);
        bridge->configWrite16(PCI_MEMORY_BASE + 2, (uint16_t)lim20);
        IOLog("IntelVMD: bridge %02x:%02x.%x mem window 0x%08x-0x%08x\n",
              bridge->space.s.busNum, bridge->space.s.deviceNum,
              bridge->space.s.functionNum,
              base20 << 20, (lim20 << 20) | 0xFFFFF);
    } else {
        bridge->configWrite32(PCI_MEMORY_BASE, 0x0000fff0);   // closed
    }

    // I/O window stays closed — a VMD domain has no I/O space
    // (Linux: "BAR 13: failed to assign [io size 0x1000]").
    bridge->configWrite8(PCI_IO_BASE,     0xf0);
    bridge->configWrite8(PCI_IO_BASE + 1, 0x00);
    bridge->configWrite32(PCI_IO_BASE_UPPER16, 0);

    // Prefetchable window — opened only when a child actually decodes into
    // the MEMBAR2 region (64-bit encoding, bits 31:20 in 0x24/0x26, upper
    // dwords at 0x28/0x2c).
    if (pfMax > pfMin) {
        uint64_t pBase = pfMin & ~0xFFFFFULL;
        uint64_t pEnd  = (pfMax + 0xFFFFF) & ~0xFFFFFULL;
        bridge->configWrite16(PCI_PREF_MEMORY_BASE,
                 (uint16_t)(((uint32_t)(pBase >> 20) & 0xFFF) | 0x1));
        bridge->configWrite16(PCI_PREF_MEMORY_BASE + 2,
                 (uint16_t)(((uint32_t)((pEnd - 1) >> 20) & 0xFFF) | 0x1));
        bridge->configWrite32(PCI_PREF_BASE_UPPER32,  (uint32_t)(pBase >> 32));
        bridge->configWrite32(PCI_PREF_LIMIT_UPPER32,
                              (uint32_t)((pEnd - 1) >> 32));
        IOLog("IntelVMD: bridge %02x:%02x.%x pf window 0x%llx-0x%llx\n",
              bridge->space.s.busNum, bridge->space.s.deviceNum,
              bridge->space.s.functionNum,
              (unsigned long long)pBase, (unsigned long long)pEnd);
    } else {
        bridge->configWrite32(PCI_PREF_LIMIT_UPPER32, 0);
        bridge->configWrite32(PCI_PREF_MEMORY_BASE,   0x0000fff0);
        bridge->configWrite32(PCI_PREF_BASE_UPPER32,  0xffffffff);
    }

    // Bus numbers.  VMD root ports hard-wire the primary bus to 0 ("Primary
    // bus is hard wired to 0" in Linux dmesg); secondary must cover the
    // child bus and subordinate the rest of the VMD domain.
    IOPCIDevice *fnub =
        OSDynamicCast(IOPCIDevice, bridge->getChildEntry(gIODTPlane));
    if (fnub) {
        uint8_t childBus = fnub->space.s.busNum;
        uint8_t sec      = bridge->configRead8(0x19);
        bridge->configWrite8(0x18, 0x00);          // primary (hardwired 0)
        bridge->configWrite8(0x19, childBus);      // secondary
        bridge->configWrite8(0x1a, fBusnEnd);      // subordinate
        if (sec != childBus)
            IOLog("IntelVMD: bridge %02x:%02x.%x secondary bus 0x%02x -> 0x%02x\n",
                  bridge->space.s.busNum, bridge->space.s.deviceNum,
                  bridge->space.s.functionNum, sec, childBus);
    }

    // Bridge command: memory decode + bus master so TLPs are forwarded.
    uint16_t cmd  = bridge->configRead16(kIOPCIConfigCommand);
    uint16_t want = cmd | kIOPCICommandMemorySpace | kIOPCICommandBusMaster;
    if (want != cmd) {
        bridge->configWrite16(kIOPCIConfigCommand, want);
        IOLog("IntelVMD: bridge %02x:%02x.%x cmd 0x%04x -> 0x%04x\n",
              bridge->space.s.busNum, bridge->space.s.deviceNum,
              bridge->space.s.functionNum, cmd, want);
    }

    refreshNubDeviceMemory(bridge);
}

void VMDDriver::collectDescendantRanges(IORegistryEntry *busNub,
                                        uint64_t *memMin, uint64_t *memMax,
                                        uint64_t *pfMin,  uint64_t *pfMax)
{
    OSIterator *iter = busNub->getChildIterator(gIODTPlane);
    if (!iter) return;
    iter->reset();

    IORegistryEntry *entry;
    while ((entry = (IORegistryEntry *)iter->getNextObject())) {
        IOPCIDevice *nub = OSDynamicCast(IOPCIDevice, entry);
        if (!nub) continue;
        if (nub->configRead16(kIOPCIConfigVendorID) == 0xFFFF) continue;

        uint8_t  hdr      = nub->configRead8(PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
        uint32_t barCount = (hdr == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;

        for (uint32_t b = 0; b < barCount; b++) {
            uint32_t reg = kIOPCIConfigBaseAddress0 + b * 4;
            uint32_t low = nub->configRead32(reg);
            if (low == 0xFFFFFFFF || (low & 0x1)) continue;
            bool is64 = ((low & 0x6) == 0x4);
            uint64_t addr = low & 0xFFFFFFF0ULL;
            if (is64) addr |= ((uint64_t)nub->configRead32(reg + 4) << 32);
            if (!addr) continue;

            uint64_t size = 0;
            IODeviceMemory *fam = nub->getDeviceMemoryWithRegister(reg);
            if (fam) size = fam->getLength();
            if (!size) size = probeBarSize(nub, reg, is64);
            if (!size) continue;
            uint64_t end = addr + size;

            if (inVMDWindow(addr, end, false)) {
                if (addr < *memMin) *memMin = addr;
                if (end > *memMax)  *memMax = end;
            } else if (inVMDWindow(addr, end, true)) {
                if (addr < *pfMin) *pfMin = addr;
                if (end > *pfMax)  *pfMax = end;
            }
        }

        if (hdr == PCI_HEADER_TYPE_BRIDGE)
            collectDescendantRanges(nub, memMin, memMax, pfMin, pfMax);
    }
    iter->release();
}

bool VMDDriver::inVMDWindow(uint64_t begin, uint64_t end, bool prefetch)
{
    for (uint32_t i = 0; i < fWindowCount; i++) {
        bool isPF = (fWindows[i].type == kIOPCIResourceTypePrefetchMemory);
        if (isPF != prefetch) continue;
        if (begin >= fWindows[i].start &&
            end - 1 <= fWindows[i].start + fWindows[i].size - 1)
            return true;
    }
    return false;
}

/* Rebuild a nub's published IODeviceMemory array from the LIVE config BARs
 * (addresses may have just been reprogrammed; sizes come from the
 * family-probed ranges).  Keeps the stock index-based accessors and Apple's
 * save/restore consistent with hardware. */
void VMDDriver::refreshNubDeviceMemory(IOPCIDevice *nub)
{
    uint8_t  hdr      = nub->configRead8(PCI_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
    uint32_t barCount = (hdr == PCI_HEADER_TYPE_BRIDGE) ? 2 : 6;

    IODeviceMemory::InitElement ranges[6];
    uint32_t n = 0;

    for (uint32_t b = 0; b < barCount; b++) {
        uint32_t reg = kIOPCIConfigBaseAddress0 + b * 4;
        uint32_t low = nub->configRead32(reg);
        if (low == 0xFFFFFFFF || (low & 0x1)) continue;
        bool is64 = ((low & 0x6) == 0x4);
        uint64_t addr = low & 0xFFFFFFF0ULL;
        if (is64) addr |= ((uint64_t)nub->configRead32(reg + 4) << 32);
        if (!addr) continue;

        IODeviceMemory *fam = nub->getDeviceMemoryWithRegister(reg);
        uint64_t len = fam ? fam->getLength() : 0;
        if (!len) continue;

        IOPCIAddressSpace as = nub->space;
        as.s.space       = (is64 || (addr >> 32)) ? kIOPCI64BitMemorySpace
                                                  : kIOPCI32BitMemorySpace;
        as.s.registerNum = reg;
        as.s.reloc       = 1;

        ranges[n].start  = addr;
        ranges[n].length = len;
        ranges[n].tag    = as.bits;
        n++;
    }
    if (!n) return;

    OSArray *array = IODeviceMemory::arrayFromList(ranges, n);
    if (!array) return;
    nub->setDeviceMemory(array);
    array->release();
    IOLog("IntelVMD: %02x:%02x.%x device memory refreshed (%u ranges)\n",
          nub->space.s.busNum, nub->space.s.deviceNum,
          nub->space.s.functionNum, n);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * CAP probe — read NVMe CAP (BAR0+0, the exact register behind the
 * "( CAP != kNVMeInvalidRegisterValue64 )" assert) through a fresh mapping
 * of the live BAR0 decode address.  Distinguishes a dead decode chain
 * (all-1s = master abort, all-0s = unmapped) from a healthy controller.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::capProbeDiagnostic(IOPCIDevice *nub)
{
    if (nub->configRead16(PCI_CLASS_DEVICE) != 0x0108) return;   // NVMe only

    uint32_t barLo = nub->configRead32(kIOPCIConfigBaseAddress0);
    uint32_t barHi = nub->configRead32(kIOPCIConfigBaseAddress0 + 4);
    uint16_t cmd   = nub->configRead16(kIOPCIConfigCommand);

    IODeviceMemory *mem = nub->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!mem) {
        IOLog("IntelVMD: CAP probe %02x:%02x.%x: NO IODeviceMemory for BAR0 "
              "(bar 0x%08x%08x cmd 0x%04x)\n",
              nub->space.s.busNum, nub->space.s.deviceNum,
              nub->space.s.functionNum, barHi, barLo, cmd);
        return;
    }

    uint64_t phys = mem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
    IOMemoryMap *map = mem->map(kIOMapAnywhere);
    if (!map) {
        IOLog("IntelVMD: CAP probe %02x:%02x.%x: map failed @0x%llx\n",
              nub->space.s.busNum, nub->space.s.deviceNum,
              nub->space.s.functionNum, (unsigned long long)phys);
        mem->release();                 // +1 from getDeviceMemoryWithRegister
        return;
    }

    volatile uint32_t *regs = (volatile uint32_t *)map->getVirtualAddress();
    uint32_t capLo = OSReadLittleInt32(regs, 0);
    uint32_t capHi = OSReadLittleInt32(regs, 4);
    uint16_t vs    = (uint16_t)OSReadLittleInt32(regs, 8);   // VS register
    map->release();
    mem->release();                     // +1 from getDeviceMemoryWithRegister

    bool dead = (capLo == 0xFFFFFFFF && capHi == 0xFFFFFFFF)
             || (capLo == 0x00000000 && capHi == 0x00000000);
    IOLog("IntelVMD: CAP probe %02x:%02x.%x BAR0 0x%08x%08x -> phys 0x%llx: "
          "CAP 0x%08x%08x VS 0x%04x cmd 0x%04x — %s\n",
          nub->space.s.busNum, nub->space.s.deviceNum, nub->space.s.functionNum,
          barHi, barLo, (unsigned long long)phys,
          capHi, capLo, vs, cmd,
          dead ? "DECODE DEAD" : "DECODE OK");
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
            entry->dtNub->setProperty(kACPIDevicePathKey,
                                      OSString::withCString(path));
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

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Child MSI wiring — the irq_domain equivalent.
 *
 * IOPCI2PCIBridge::start() created a messaged-interrupt controller whose
 * vectors are the VMD device's MSI-X table entries.  For each child:
 *   - configureInterrupts(kIOInterruptTypePCIMessaged, 1, 1) allocates one
 *     vector for the child (vmd_msi_alloc()),
 *   - the allocated vector number is read back from the child's interrupt
 *     specifier (vmd_compose_msi_msg(): the VMD routes the child's MSI to
 *     MSI-X table entry <vector>),
 *   - the child's handler is (re-)registered on the VMD device at that
 *     vector, so a VMD MSI-X interrupt dispatches straight into the child
 *     driver (vmd_irq() -> generic_handle_irq()).
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void VMDDriver::wireChildMSI(IOPCIDevice *nub)
{
    if (!nub) return;

    uint8_t cap = 0;
    if (!nub->findPCICapability(kIOPCICapabilityIDMSI, &cap) || !cap) return;

    if (!nub->getProperty("IOPCIMSIMode"))
        nub->configureInterrupts(kIOInterruptTypePCIMessaged, 1, 1);

    OSArray *specs = OSDynamicCast(OSArray,
        nub->getPropertyTable()->getObject(gIOInterruptSpecifiersKey));
    OSData *spec = specs ? OSDynamicCast(OSData, specs->getObject(0)) : NULL;
    if (!spec || spec->getLength() < sizeof(uint32_t)) return;

    uint32_t vmdVector = *(const uint32_t *)spec->getBytesNoCopy();

    IOInterruptVector *vector = VMDChildDevice::msiVectorsOf(nub);
    if (vector && vector->handler) {
        fPCIDev->registerInterrupt(vmdVector, (OSObject *)vector->nub,
                                   (IOInterruptAction)vector->handler,
                                   vector->refCon);
        IOLog("IntelVMD: child %02x:%02x.%x wired to VMD MSI-X vector %u\n",
              nub->space.s.busNum, nub->space.s.deviceNum,
              nub->space.s.functionNum, vmdVector);
    }

    // refresh the child's config shadow (power management)
    IOPCIConfigShadow *shadow = configShadow(nub);
    for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
        shadow->configSave.savedConfig[i] = nub->configRead32(i * 4);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * vmd_pm_enable_quirk() — BIOS-PM quirk.
 *
 * "Enable ASPM and LTR settings on devices that aren't configured by BIOS."
 * If the LTR max-snoop/max-no-snoop values are zero, program the platform
 * default (0x1003 = 3145728 ns) so the device can reach the deepest
 * power-management savings.  Extended capabilities live at offsets >=
 * 0x100, so the 12-bit offset is carried explicitly through
 * es.registerNumExtended (see ecamFullOffset()).
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
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
            }
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
    // stock IOPCIFamily cleanup (binds to the IOPCIFamily export — this
    // kext does not define IOPCI2PCIBridge::free)
    IOPCI2PCIBridge::free();
}

IOWorkLoop * VMDDriver::getWorkLoop(void) const
{
    return fVmdWorkLoop;
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

/* Nub factory — the configurator scan creates VMDChildDevice instances for
 * every device it finds behind the VMD. */
IOPCIDevice * VMDDriver::createNub(OSDictionary *from)
{
    VMDChildDevice *nub = new VMDChildDevice;
    if (nub && !nub->init(from)) {
        nub->release();
        return NULL;
    }
    if (nub) nub->pvmd = this;
    return nub;
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

bool VMDChildDevice::init(OSDictionary *properties)
{
    for (uint32_t i = 0; i < 6; i++) { fFixedMemory[i] = NULL; fBarSize[i] = 0; }
    return IOPCIDevice::init(properties);
}

/* Live-decode BAR accessor.
 *
 * The stock IOPCIDevice::getDeviceMemoryWithRegister returns the
 * IODeviceMemory IOPCIFamily constructed from the BAR values it saw during
 * its scan.  Behind a VMD the effective decode address is whatever is in
 * config space RIGHT NOW (BIOS value, IOPCIFamily allocation, our assign
 * stage, or a re-write after a bus reset) — and IONVMeController maps BAR0
 * through this accessor to read CAP/CSTS.  A stale range here silently
 * redirects every NVMe register access into dead host memory and produces
 * "( CAP != kNVMeInvalidRegisterValue64 )".
 *
 * This override re-reads the BAR from live config space (through the VMD
 * ECAM) and, when the live address differs from the family range, hands
 * out a corrected IODeviceMemory of the same size.  When both agree the
 * stock range is returned untouched. */
IODeviceMemory * VMDChildDevice::getDeviceMemoryWithRegister( UInt8 reg )
{
    if (reg >= kIOPCIConfigBaseAddress0 && reg <= kIOPCIConfigBaseAddress5) {
        uint32_t low = configRead32(this->space,reg);
        if (low != 0xFFFFFFFF && !(low & 0x1)) {
            uint32_t idx = (reg - kIOPCIConfigBaseAddress0) / 4;
            bool     is64 = ((low & 0x6) == 0x4);
            uint64_t addr = low & 0xFFFFFFF0ULL;
            if (is64 && reg <= kIOPCIConfigBaseAddress4)
                addr |= ((uint64_t)configRead32(this->space,reg + 4) << 32);

            IODeviceMemory *family =
                IOPCIDevice::getDeviceMemoryWithRegister(reg);      // +1
            if (family) {
                uint64_t famAddr =
                    family->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
                if (famAddr == addr || !addr)
                    return family;              // consistent — stock range
            }

            /* Length: the size we recorded while assigning BARs is the
             * ground truth; the family range length is the fallback. */
            uint64_t len = (idx < 6) ? fBarSize[idx] : 0;
            if (!len && family) len = family->getLength();

            IODeviceMemory *result = NULL;
            if (addr && len) {
                IODeviceMemory *fixed = fFixedMemory[idx];
                if (fixed && fixed->getPhysicalSegment(
                                 0, NULL, kIOMemoryMapperNone) == addr) {
                    result = fixed;             // cached, still live
                } else {
                    if (fixed) {
                        fixed->release();       // drop stale cached range
                        fFixedMemory[idx] = NULL;
                    }
                    fixed = IODeviceMemory::withRange(
                                (IOPhysicalAddress)addr,
                                (IOPhysicalLength)len);
                    if (fixed) {
                        fFixedMemory[idx] = fixed;  /* nub keeps one ref */
                        IOLog("IntelVMD: %02x:%02x.%x reg 0x%02x live BAR "
                              "0x%llx len 0x%llx (family 0x%llx/0x%llx) — "
                              "using live decode address\n",
                              space.s.busNum, space.s.deviceNum,
                              space.s.functionNum, reg,
                              (unsigned long long)addr,
                              (unsigned long long)len,
                              family ? (unsigned long long)
                                  family->getPhysicalSegment(
                                      0, NULL, kIOMemoryMapperNone)
                                     : 0ULL,
                              family ? (unsigned long long)
                                  family->getLength() : 0ULL);
                    }
                    result = fixed;
                }
                if (result) result->retain();   /* caller owns this ref */
            }
            if (family) family->release();      /* our lookup ref */
            if (result) return result;
        }
    }
    return IOPCIDevice::getDeviceMemoryWithRegister(reg);
}

IOWorkLoop * VMDChildDevice::getWorkLoop(void) const
{
    if (pvmd) return pvmd->getWorkLoop();
    return IOService::getWorkLoop();
}

void VMDChildDevice::free(void)
{
    for (uint32_t i = 0; i < 6; i++) {
        if (fFixedMemory[i]) {
            fFixedMemory[i]->release();
            fFixedMemory[i] = NULL;
        }
    }
    IOService::free();
}

/* Protected-member access helpers.  IOPCIDevice::reserved is protected, so
 * the access must happen from a context whose static type is VMDChildDevice
 * — hence the cast inside these static members.  Family-created IOPCIDevice
 * instances always have `reserved` allocated by initReserved(). */
IOPCIConfigEntry * VMDChildDevice::configEntryOf(IOPCIDevice *device)
{
    if (!device) return NULL;
    return ((VMDChildDevice *)device)->reserved->configEntry;
}

IOInterruptVector * VMDChildDevice::msiVectorsOf(IOPCIDevice *device)
{
    if (!device) return NULL;
    return ((VMDChildDevice *)device)->reserved->msiVectors;
}
