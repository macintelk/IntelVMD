

#ifndef VMDDriver_h
#define VMDDriver_h

#include <IOKit/IOLib.h>
#include <IOKit/assert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOPlatformExpert.h>

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pci/IOPCIBridge.h>

#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <IOKit/acpi/IOACPITypes.h>

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOInterruptController.h>
#include <libkern/OSDebug.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOTimerEventSource.h>

#include <libkern/OSAtomic.h>

#include <IOKit/IOCommandGate.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>

#include "IOKit/pci/IOPCIPrivate.h"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Linux vmd.c definitions, ported verbatim where possible.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define VMD_CFGBAR      0
#define VMD_MEMBAR1     2
#define VMD_MEMBAR2     4

#define PCI_REG_VMCAP           0x40
#define BUS_RESTRICT_CAP(vmcap) (vmcap & 0x1)
#define PCI_REG_VMCONFIG        0x44
#define BUS_RESTRICT_CFG(vmcfg) ((vmcfg >> 8) & 0x3)
#define VMCONFIG_MSI_REMAP      0x2
#define PCI_REG_VMLOCK          0x70
#define MB2_SHADOW_EN(vmlock)   (vmlock & 0x2)

#define MB2_SHADOW_OFFSET       0x2000
#define MB2_SHADOW_SIZE         16

/* DMR BAR4 register offsets (device 0x28C1 BIOS-info path) */
#define SHADOW_MEMBAR1_28C1     0x2818  /* MEMBAR1 physical address */
#define SHADOW_MEMBAR2_28C1     0x2820  /* MEMBAR2 physical address */
#define BASE_ID_REG_28C1        0x2840
#define MEMBAR2_OFFSET_28C1     0x30d0

/* Linux enum vmd_features — bit values identical to the kernel driver. */
enum vmd_features {
    VMD_FEAT_HAS_MEMBAR_SHADOW       = (1 << 0),
    VMD_FEAT_HAS_BUS_RESTRICTIONS    = (1 << 1),
    VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP = (1 << 2),
    VMD_FEAT_OFFSET_FIRST_VECTOR     = (1 << 3),
    VMD_FEAT_CAN_BYPASS_MSI_REMAP    = (1 << 4),
    VMD_FEAT_BIOS_PM_QUIRK           = (1 << 5),
    VMD_FEAT_USE_BIOS_INFO           = (1 << 6),
};

#define VMD_BIOS_PM_QUIRK_LTR   0x1003  /* 3145728 ns */

/* LTR extended capability (Linux pci_regs.h). */
#define PCI_EXT_CAP_ID_LTR      0x18
#define PCI_LTR_MAX_SNOOP_LAT   0x4
#define PCI_LTR_VALUE_MASK      0x000003ff
#define PCI_LTR_SCALE_MASK      0x00001c00

/* ECAM (Linux PCIE_ECAM_OFFSET). */
#define PCI_DEVFN(slot, func)   ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCIE_ECAM_BUS(x)        (((x) & 0xff) << 20)
#define PCIE_ECAM_DEVFN(x)      (((x) & 0xff) << 12)
#define PCIE_ECAM_REG(x)        ((x) & 0xfff)
#define PCIE_ECAM_OFFSET(bus, devfn, where) \
    (PCIE_ECAM_BUS(bus) | PCIE_ECAM_DEVFN(devfn) | PCIE_ECAM_REG(where))

#define PCI_BASE_ADDRESS_MEM_MASK (~0x0fULL)

/* Vendor-specific capability "SHDW" signature (Linux vmd_get_phys_offsets). */
#define PCI_CAP_ID_VNDR         0x09
#define VMD_SHDW_SIGNATURE      0x53484457  /* "SHDW" */

/* Root-port class / header (vmd_domain_reset). */
#define PCI_HEADER_TYPE         0x0e
#define PCI_HEADER_TYPE_MFD     0x80
#define PCI_HEADER_TYPE_MASK    0x7f
#define PCI_HEADER_TYPE_BRIDGE  1
#define PCI_CLASS_DEVICE        0x0a
#define PCI_CLASS_BRIDGE_PCI    0x0604
#define PCI_IO_BASE             0x1c
#define PCI_IO_BASE_UPPER16     0x30
#define PCI_MEMORY_BASE         0x20
#define PCI_PREF_LIMIT_UPPER32  0x2c
#define PCI_PREF_MEMORY_BASE    0x24
#define PCI_PREF_BASE_UPPER32   0x28
#define PCI_BRIDGE_CONTROL      0x3e    /* bridge control (secondary bus reset) */
#define PCI_BRIDGE_CTL_BUS_RESET 0x40   /* PCI_BRIDGE_CTL_BUS_RESET */

/* IONVMeFamily integration keys (see VMD.cpp associateACPI). */
#ifndef kACPIDevicePathKey
#define kACPIDevicePathKey      "acpi-path"
#endif

class VMDChildDevice;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * VMDDriver — one instance per VMD PCI function (Linux struct vmd_dev).
 *
 * Derives from IOPCI2PCIBridge so the whole IOPCIFamily enumeration,
 * resource-allocation and MSI machinery treats the VMD root complex like
 * a normal PCI host bridge.  All config cycles for buses behind the VMD
 * are routed to the VMD ECAM window (BAR0), exactly like Linux
 * vmd_pci_read()/vmd_pci_write().
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
class VMDDriver : public IOPCI2PCIBridge
{
    OSDeclareDefaultStructors(VMDDriver)

private:
    /* --- Linux struct vmd_dev state --- */
    IOPCIDevice *           fPCIDev;        /* vmd->dev                    */
    IOMemoryMap *           fCfgMap;        /* pcim_iomap(VMD_CFGBAR)      */
    volatile uint8_t *      fCfgBar;        /* vmd->cfgbar                 */
    uint64_t                fCfgBarSize;    /* resource_size(cfgbar)       */
    IOSimpleLock*            fCfgLock;       /* vmd->cfg_lock               */

    uint8_t                 fBusnStart;     /* vmd->busn_start             */
    uint8_t                 fBusnEnd;       /* root bus window end         */
    uint8_t                 fFirstVec;      /* vmd->first_vec              */
    uint32_t                fMsixCount;     /* vmd->msix_count             */
    unsigned long           fFeatures;      /* vmd->features               */

    uint64_t                fMembar2Offset; /* vmd_enable_domain()         */
    uint64_t                fOffset1;       /* guest MEMBAR1 phys offset   */
    uint64_t                fOffset2;       /* guest MEMBAR2 phys offset   */

    /* window list used to seed the IOPCIConfigEntry ranges (macOS glue)   */
    struct VMDWindow {
        uint32_t type;                      /* kIOPCIResourceType*         */
        uint64_t start;
        uint64_t size;
    };
    VMDWindow               fWindows[3];
    uint32_t                fWindowCount;

    IOPCIAddressSpace       fBridgeSpace;   /* getBridgeSpace()            */
    IOPCIConfigEntry *      fBridgeEntry;   /* fPCIDev's config entry      */
    IOWorkLoop *            fVmdWorkLoop;
    IOInterruptEventSource *fBridgeES;      /* reserves MSI-X vector 0     */

    bool vmdProbe(IOService *provider);
    bool decodeAndPublishBars(void);
    bool getBusNumberStart(void);
    bool getPhysOffsets(bool nativeHint);
    bool getBusInfoFromBar4(void);
    void setMSIRemapping(bool enable);
    bool setupMSIX(void);
    bool seedBridgeEntry(void);
    void postScanConfigure(void);
    void associateACPI(IOPCIConfigEntry *entry, IOPCIDevice *inheritFrom);
    void wireChildMSI(IOPCIDevice *nub);
    void enableLTRQuirk(IOPCIDevice *dev);
    IOACPIPlatformDevice * findACPICompanion(uint32_t busnrEcam,
                                             uint8_t dev, uint8_t fn);
    void handleBridgeInterrupt(OSObject *owner,
                               IOInterruptEventSource *src, int count);

public:
    /* IOService */
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free(void) override;
    virtual IOWorkLoop * getWorkLoop(void) const override;

    /* IOPCIBridge */
    virtual UInt8 firstBusNum(void) override;
    virtual UInt8 lastBusNum(void) override;
    virtual IOPCIAddressSpace getBridgeSpace(void) override;
    virtual IOPCIDevice * createNub(OSDictionary *from) override;

    /* Linux vmd_ops — ECAM config access (vmd_pci_read/vmd_pci_write). */
    virtual UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) override;
    virtual void   configWrite32(IOPCIAddressSpace space,
                                 UInt8 offset, UInt32 data) override;
    virtual UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) override;
    virtual void   configWrite16(IOPCIAddressSpace space,
                                 UInt8 offset, UInt16 data) override;
    virtual UInt8  configRead8(IOPCIAddressSpace space, UInt8 offset) override;
    virtual void   configWrite8(IOPCIAddressSpace space,
                                UInt8 offset, UInt8 data) override;

    virtual UInt32 findPCICapability(IOPCIAddressSpace space,
                                     UInt8 capabilityID,
                                     UInt8 *offset = 0) override;
    virtual UInt32 extendedFindPCICapability(IOPCIAddressSpace space,
                                             UInt32 capabilityID,
                                             IOByteCount *offset = 0) override;

    /* Linux vmd_domain_reset() */
    void resetDomain(void);

    inline volatile uint8_t * cfgBar(void) const { return fCfgBar; }
    inline uint8_t busnStart(void) const { return fBusnStart; }
    void configure1( IOService * provider );
    void configurePCISpace(const char *propName, IOService *pciDevice,
                           IOPCIAddressSpace addrSpace,
                           UInt64 address,
                           UInt64 length);
    
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * VMDChildDevice — nub for every PCI device behind the VMD root complex.
 *
 * Created by VMDDriver::createNub() during the IOPCIFamily scan.  Config
 * cycles are answered from the VMD ECAM (a child device has no host-bus
 * presence).  The nub carries the ACPI association required by Apple's
 * IONVMeFamily (IONVMeController::CopyACPIParent asserts 0 != acpiRoot-
 * Device; see VMDDriver::associateACPI).
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
class VMDChildDevice : public IOPCIDevice
{
    OSDeclareDefaultStructors(VMDChildDevice)

public:
    VMDDriver * pvmd;

    
    virtual UInt32 configRead32( IOPCIAddressSpace space, UInt8 offset ) override;
    virtual void configWrite32( IOPCIAddressSpace space,
                                        UInt8 offset, UInt32 data ) override;
    virtual UInt16 configRead16( IOPCIAddressSpace space, UInt8 offset ) override;
    virtual void configWrite16( IOPCIAddressSpace space,
                                        UInt8 offset, UInt16 data ) override;
    virtual UInt8 configRead8( IOPCIAddressSpace space, UInt8 offset ) override;
    virtual void configWrite8( IOPCIAddressSpace space,
                                        UInt8 offset, UInt8 data ) override;
    
    virtual IOWorkLoop * getWorkLoop(void) const override;
    virtual void free(void) override;

    /* Access to IOPCIDevice-protected state (configEntry / msiVectors).
     * Defined as static members of VMDChildDevice because protected-member
     * access rules require the derived static type. */
    static IOPCIConfigEntry * configEntryOf(IOPCIDevice *device);
    static IOInterruptVector * msiVectorsOf(IOPCIDevice *device);
};

#endif /* VMDDriver_h */
