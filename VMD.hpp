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





#define X86_MSI_BASE_ADDRESS_HIGH    (0)
#define X86_MSI_BASE_ADDRESS_LOW    (0xfee00000 >> 20)




typedef struct x86_msi_data {
    union {
        struct {
            uint32_t    vector            :  8,
                delivery_mode        :  3,
                dest_mode_logical    :  1,
                reserved        :  2,
                active_low        :  1,
                is_level        :  1;
        };
        uint32_t    dmar_subhandle;
    };
} __attribute__ ((packed)) arch_msi_msg_data_t;
#define arch_msi_msg_data    x86_msi_data

typedef struct x86_msi_addr_lo {
    union {
        struct {
            uint32_t    reserved_0        :  2,
                dest_mode_logical    :  1,
                redirect_hint        :  1,
                reserved_1        :  1,
                virt_destid_8_14    :  7,
                destid_0_7        :  8,
                base_address        : 12;
        };
        struct {
            uint32_t    dmar_reserved_0        :  2,
                dmar_index_15        :  1,
                dmar_subhandle_valid    :  1,
                dmar_format        :  1,
                dmar_index_0_14        : 15,
                dmar_base_address    : 12;
        };
    };
} __attribute__ ((packed)) arch_msi_msg_addr_lo_t;

typedef struct x86_msi_addr_hi {
    uint32_t    reserved        :  8,
        destid_8_31        : 24;
} __attribute__ ((packed)) arch_msi_msg_addr_hi_t;
#define arch_msi_msg_addr_hi    x86_msi_addr_hi

struct msi_msg {
    union {
        uint32_t            address_lo;
        arch_msi_msg_addr_lo_t    arch_addr_lo;
    };
    union {
        uint32_t            address_hi;
        arch_msi_msg_addr_hi_t    arch_addr_hi;
    };
    union {
        uint32_t            data;
        arch_msi_msg_data_t    arch_data;
    };
};



#define AbsoluteTime_to_scalar(x)       (*(uint64_t *)(x))

#define kIOPCIMSIEnable                0x0001
#define kIOPCIMSI64Bit                 0x0080
#define kIOPCIMSIDataTriggerEdge       0x0000
#define kIOPCIMSIDataTriggerLevel      0x8000
#define kIOPCIMSIMultipleMessageMask   0x70
#define kIOPCIMSIMultipleMessageShift  4

#define PCI_MSIX_ENTRY_SIZE 16
#define APIC_BASE_ADDRESS 0xFEE00000



#define PCI_MSIX_TABLE        4
#define PCI_MSIX_PBA            8       // Offset to PBA register in MSI-X cap
#define PCI_STATUS_INTERRUPT    0x0400  // BIT 9: Interrupt Status
#define PCI_COMMAND_STATUS_REG  0x04

#define PCI_CAP_ID_MSIX         0x11
#define PCI_MSIX_FLAGS          2
#define PCI_MSIX_FLAGS_QSIZE    0x7FF  // Table size mask
#define PCI_MSIX_FLAGS_ENABLE   0x8000 // MSI-X enable mask
#define PCI_MSIX_ENTRY_SIZE     16     // Size of each MSI-X entry
#define PCI_MSIX_ENTRY_VECTOR   8      // Offset to vector/data in entry
#define PCI_MSI_FLAGS        0x02    /* Message Control */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */
#define  PCI_MSI_FLAGS_QMASK    0x000e    /* Maximum queue size available */
#define  PCI_MSI_FLAGS_QSIZE    0x0070    /* Message queue size configured */
#define  PCI_MSI_FLAGS_64BIT    0x0080    /* 64-bit addresses allowed */
#define  PCI_MSI_FLAGS_MASKBIT    0x0100    /* Per-vector masking capable */
#define PCI_MSI_RFU        3    /* Rest of capability flags */
#define PCI_MSI_ADDRESS_LO    0x04    /* Lower 32 bits */
#define PCI_MSI_ADDRESS_HI    0x08    /* Upper 32 bits (if PCI_MSI_FLAGS_64BIT set) */
#define PCI_MSI_DATA_32        0x08    /* 16 bits of data for 32-bit devices */
#define PCI_MSI_MASK_32        0x0c    /* Mask bits register for 32-bit devices */
#define PCI_MSI_PENDING_32    0x10    /* Pending intrs for 32-bit devices */
#define PCI_MSI_DATA_64        0x0c    /* 16 bits of data for 64-bit devices */
#define PCI_MSI_MASK_64        0x10    /* Mask bits register for 64-bit devices */
#define PCI_MSI_PENDING_64    0x14    /* Pending intrs for 64-bit devices */



#define PCI_MSIX_PBA            8       // Offset to PBA register in MSI-X cap
#define PCI_STATUS_INTERRUPT    0x0400  // BIT 9: Interrupt Status
#define PCI_COMMAND_STATUS_REG  0x04

#define PCI_CAP_ID_MSIX         0x11
#define PCI_MSIX_FLAGS          2
#define PCI_MSIX_FLAGS_QSIZE    0x7FF  // Table size mask
#define PCI_MSIX_FLAGS_ENABLE   0x8000 // MSI-X enable mask
#define PCI_MSIX_ENTRY_SIZE     16     // Size of each MSI-X entry
#define PCI_MSIX_ENTRY_VECTOR   8      // Offset to vector/data in entry

#define PCI_MSIX_ENTRY_SIZE        16
#define PCI_MSIX_ENTRY_LOWER_ADDR    0x0  /* Message Address */
#define PCI_MSIX_ENTRY_UPPER_ADDR    0x4  /* Message Upper Address */
#define PCI_MSIX_ENTRY_DATA        0x8  /* Message Data */
#define PCI_MSIX_ENTRY_VECTOR_CTRL    0xc  /* Vector Control */
#define  PCI_MSIX_ENTRY_CTRL_MASKBIT    0x00000001  /* Mask Bit */
#define  PCI_MSIX_ENTRY_CTRL_ST        0xffff0000  /* Steering Tag */

#define VMD_CFGBAR       0

#define VMD_MSIX_TABLE   0x74
#define PCI_CAP_ID_MSIX  0x11
#define VMD_MAX_VECTORS  2048


#define VMD_BIOS_PM_QUIRK_LTR    0x1003    /* 3145728 ns */






#define KB        (1024ULL)
#define MB        (1024ULL*KB)
#define GB        (1024ULL*MB)
#define MAX_BAR_SIZE   (1ULL*GB)


#define kIOPCIConfigIOBase                0x1C    // 8-bit
#define kIOPCIConfigIOLimit               0x1D    // 8-bit
#define kIOPCIConfigMemoryBase            0x20    // 16-bit
#define kIOPCIConfigMemoryLimit           0x22    // 16-bit
#define kIOPCIConfigPrefetchableMemoryBase 0x24   // 32-bit
#define kIOPCIConfigPrefetchableMemoryLimit 0x28  // 32-bit
#define kIOPCIConfigIOBaseUpper           0x2C    // 16-bit
#define kIOPCIConfigIOLimitUpper          0x2E    // 16-bit
#define kIOPCIConfigPrefetchableBaseUpper 0x30    // 32-bit
#define kIOPCIConfigPrefetchableLimitUpper 0x34   // 32-bit
#define kIOPCIConfigBridgeControl         0x3E    // 16-bit



#define BRN(type)      ((type) + 7)





#define kPCIBridgeIOAlignment        (4096)
#define kPCIBridgeMemoryAlignment    (1024*1024)
#define kPCIBridgeBusNumberAlignment (1)

static const IOPCIScalar minBARAddressDefault[kIOPCIResourceTypeCount] = {
    kPCIBridgeMemoryAlignment, kPCIBridgeMemoryAlignment,
    0x400, 0
};



typedef unsigned char   BOOL;
    typedef unsigned char    BYTE;
    typedef unsigned int    WORD;
    typedef unsigned long    DWORD;

    typedef unsigned char    UCHAR;
    typedef unsigned int    UINT;
    typedef unsigned short  USHORT;
    typedef unsigned long    ULONG;

    typedef char            CHAR;
    typedef int                INT;
    typedef long            LONG;

typedef struct _PCI_SLOT_NUMBER {
    union {
        struct {
            ULONG   DeviceNumber:5;
            ULONG   FunctionNumber:3;
            ULONG   Reserved:24;
        } bits;
        ULONG   AsULONG;
    } u;
} PCI_SLOT_NUMBER, *PPCI_SLOT_NUMBER;

#define PCI_TYPE0_ADDRESSES             6
#define PCI_TYPE1_ADDRESSES             2
#define PCI_TYPE2_ADDRESSES             5

typedef struct _PCI_COMMON_HEADER {
    USHORT  VendorID;                   // (ro)
    USHORT  DeviceID;                   // (ro)
    USHORT  Command;                    // Device control
    USHORT  Status;
    UCHAR   RevisionID;                 // (ro)
    UCHAR   ProgIf;                     // (ro)
    UCHAR   SubClass;                   // (ro)
    UCHAR   BaseClass;                  // (ro)
    UCHAR   CacheLineSize;              // (ro+)
    UCHAR   LatencyTimer;               // (ro+)
    UCHAR   HeaderType;                 // (ro)
    UCHAR   BIST;                       // Built in self test

    union {
        struct _PCI_HEADER_TYPE_0 {
            ULONG   BaseAddresses[PCI_TYPE0_ADDRESSES];
            ULONG   CIS;
            USHORT  SubVendorID;
            USHORT  SubSystemID;
            ULONG   ROMBaseAddress;
            UCHAR   CapabilitiesPtr;
            UCHAR   Reserved1[3];
            ULONG   Reserved2;
            UCHAR   InterruptLine;      //
            UCHAR   InterruptPin;       // (ro)
            UCHAR   MinimumGrant;       // (ro)
            UCHAR   MaximumLatency;     // (ro)
        } type0;



        //
        // PCI to PCI Bridge
        //

        struct _PCI_HEADER_TYPE_1 {
            ULONG   BaseAddresses[PCI_TYPE1_ADDRESSES];
            UCHAR   PrimaryBus;
            UCHAR   SecondaryBus;
            UCHAR   SubordinateBus;
            UCHAR   SecondaryLatency;
            UCHAR   IOBase;
            UCHAR   IOLimit;
            USHORT  SecondaryStatus;
            USHORT  MemoryBase;
            USHORT  MemoryLimit;
            USHORT  PrefetchBase;
            USHORT  PrefetchLimit;
            ULONG   PrefetchBaseUpper32;
            ULONG   PrefetchLimitUpper32;
            USHORT  IOBaseUpper16;
            USHORT  IOLimitUpper16;
            UCHAR   CapabilitiesPtr;
            UCHAR   Reserved1[3];
            ULONG   ROMBaseAddress;
            UCHAR   InterruptLine;
            UCHAR   InterruptPin;
            USHORT  BridgeControl;
        } type1;

        //
        // PCI to CARDBUS Bridge
        //

        struct _PCI_HEADER_TYPE_2 {
            ULONG   SocketRegistersBaseAddress;
            UCHAR   CapabilitiesPtr;
            UCHAR   Reserved;
            USHORT  SecondaryStatus;
            UCHAR   PrimaryBus;
            UCHAR   SecondaryBus;
            UCHAR   SubordinateBus;
            UCHAR   SecondaryLatency;
            struct  {
                ULONG   Base;
                ULONG   Limit;
            }       Range[PCI_TYPE2_ADDRESSES-1];
            UCHAR   InterruptLine;
            UCHAR   InterruptPin;
            USHORT  BridgeControl;
        } type2;



    } u;

} PCI_COMMON_HEADER, *PPCI_COMMON_HEADER;



#ifdef __cplusplus

typedef struct _PCI_COMMON_CONFIG : PCI_COMMON_HEADER {
    UCHAR   DeviceSpecific[192];
} PCI_COMMON_CONFIG, *PPCI_COMMON_CONFIG;

#else

typedef struct _PCI_COMMON_CONFIG {
    PCI_COMMON_HEADER DUMMYSTRUCTNAME;
    UCHAR   DeviceSpecific[192];
} PCI_COMMON_CONFIG, *PPCI_COMMON_CONFIG;

#endif

#define PCI_COMMON_HDR_LENGTH (FIELD_OFFSET (PCI_COMMON_CONFIG, DeviceSpecific))
#define PCI_EXTENDED_CONFIG_LENGTH          0x1000



#define PCI_MAX_DEVICES                     32
#define PCI_MAX_FUNCTION                    8
#define PCI_MAX_BRIDGE_NUMBER               0xFF

#define PCI_INVALID_VENDORID                0xFFFF

//
// Bit encodings for  PCI_COMMON_CONFIG.HeaderType
//

#define PCI_MULTIFUNCTION                   0x80
#define PCI_DEVICE_TYPE                     0x00
#define PCI_BRIDGE_TYPE                     0x01
#define PCI_CARDBUS_BRIDGE_TYPE             0x02



#define PCI_CONFIGURATION_TYPE(PciData) \
    (((PPCI_COMMON_CONFIG)(PciData))->HeaderType & ~PCI_MULTIFUNCTION)

#define PCI_MULTIFUNCTION_DEVICE(PciData) \
    ((((PPCI_COMMON_CONFIG)(PciData))->HeaderType & PCI_MULTIFUNCTION) != 0)

//
// Bit encodings for PCI_COMMON_CONFIG.Command
//



#define PCI_ENABLE_IO_SPACE                 0x0001
#define PCI_ENABLE_MEMORY_SPACE             0x0002
#define PCI_ENABLE_BUS_MASTER               0x0004
#define PCI_ENABLE_SPECIAL_CYCLES           0x0008
#define PCI_ENABLE_WRITE_AND_INVALIDATE     0x0010
#define PCI_ENABLE_VGA_COMPATIBLE_PALETTE   0x0020
#define PCI_ENABLE_PARITY                   0x0040  // (ro+)
#define PCI_ENABLE_WAIT_CYCLE               0x0080  // (ro+)
#define PCI_ENABLE_SERR                     0x0100  // (ro+)
#define PCI_ENABLE_FAST_BACK_TO_BACK        0x0200  // (ro)
#define PCI_DISABLE_LEVEL_INTERRUPT         0x0400

//
// Bit encodings for PCI_COMMON_CONFIG.Status
//

#define PCI_STATUS_INTERRUPT_PENDING        0x0008
#define PCI_STATUS_CAPABILITIES_LIST        0x0010  // (ro)
#define PCI_STATUS_66MHZ_CAPABLE            0x0020  // (ro)
#define PCI_STATUS_UDF_SUPPORTED            0x0040  // (ro)
#define PCI_STATUS_FAST_BACK_TO_BACK        0x0080  // (ro)
#define PCI_STATUS_DATA_PARITY_DETECTED     0x0100
#define PCI_STATUS_DEVSEL                   0x0600  // 2 bits wide
#define PCI_STATUS_SIGNALED_TARGET_ABORT    0x0800
#define PCI_STATUS_RECEIVED_TARGET_ABORT    0x1000
#define PCI_STATUS_RECEIVED_MASTER_ABORT    0x2000
#define PCI_STATUS_SIGNALED_SYSTEM_ERROR    0x4000
#define PCI_STATUS_DETECTED_PARITY_ERROR    0x8000



//
// The NT PCI Driver uses a WhichSpace parameter on its CONFIG_READ/WRITE
// routines.   The following values are defined-
//

#define PCI_WHICHSPACE_CONFIG               0x0
#define PCI_WHICHSPACE_ROM                  0x52696350

//
// PCI Capability IDs
//



#define PCI_CAPABILITY_ID_POWER_MANAGEMENT  0x01
#define PCI_CAPABILITY_ID_AGP               0x02
#define PCI_CAPABILITY_ID_VPD               0x03
#define PCI_CAPABILITY_ID_SLOT_ID           0x04
#define PCI_CAPABILITY_ID_MSI               0x05
#define PCI_CAPABILITY_ID_CPCI_HOTSWAP      0x06
#define PCI_CAPABILITY_ID_PCIX              0x07
#define PCI_CAPABILITY_ID_HYPERTRANSPORT    0x08
#define PCI_CAPABILITY_ID_VENDOR_SPECIFIC   0x09
#define PCI_CAPABILITY_ID_DEBUG_PORT        0x0A
#define PCI_CAPABILITY_ID_CPCI_RES_CTRL     0x0B
#define PCI_CAPABILITY_ID_SHPC              0x0C
#define PCI_CAPABILITY_ID_P2P_SSID          0x0D
#define PCI_CAPABILITY_ID_AGP_TARGET        0x0E
#define PCI_CAPABILITY_ID_SECURE            0x0F
#define PCI_CAPABILITY_ID_PCI_EXPRESS       0x10
#define PCI_CAPABILITY_ID_MSIX              0x11
#define PCI_CAPABILITY_ID_SATA_CONFIG       0x12
#define PCI_CAPABILITY_ID_ADVANCED_FEATURES 0x13

//
// All PCI Capability structures have the following header.
//
// CapabilityID is used to identify the type of the structure (is
// one of the PCI_CAPABILITY_ID values above.
//
// Next is the offset in PCI Configuration space (0x40 - 0xfc) of the
// next capability structure in the list, or 0x00 if there are no more
// entries.
//
typedef struct _PCI_CAPABILITIES_HEADER {
    UCHAR   CapabilityID;
    UCHAR   Next;
} PCI_CAPABILITIES_HEADER, *PPCI_CAPABILITIES_HEADER;



//
// Power Management Capability
//

typedef struct _PCI_PMC {
    UCHAR       Version:3;
    UCHAR       PMEClock:1;
    UCHAR       Rsvd1:1;
    UCHAR       DeviceSpecificInitialization:1;
    UCHAR       Rsvd2:2;
    struct _PM_SUPPORT {
        UCHAR   Rsvd2:1;
        UCHAR   D1:1;
        UCHAR   D2:1;
        UCHAR   PMED0:1;
        UCHAR   PMED1:1;
        UCHAR   PMED2:1;
        UCHAR   PMED3Hot:1;
        UCHAR   PMED3Cold:1;
    } Support;
} PCI_PMC, *PPCI_PMC;

typedef struct _PCI_PMCSR {
    USHORT      PowerState:2;
    USHORT      Rsvd1:1;
    USHORT      NoSoftReset:1;
    USHORT      Rsvd2:4;
    USHORT      PMEEnable:1;
    USHORT      DataSelect:4;
    USHORT      DataScale:2;
    USHORT      PMEStatus:1;
} PCI_PMCSR, *PPCI_PMCSR;


typedef struct _PCI_PMCSR_BSE {
    UCHAR       Rsvd1:6;
    UCHAR       D3HotSupportsStopClock:1;       // B2_B3#
    UCHAR       BusPowerClockControlEnabled:1;  // BPCC_EN
} PCI_PMCSR_BSE, *PPCI_PMCSR_BSE;


typedef struct _PCI_PM_CAPABILITY {

    PCI_CAPABILITIES_HEADER Header;

    //
    // Power Management Capabilities (Offset = 2)
    //

    union {
        PCI_PMC         Capabilities;
        USHORT          AsUSHORT;
    } PMC;

    //
    // Power Management Control/Status (Offset = 4)
    //

    union {
        PCI_PMCSR       ControlStatus;
        USHORT          AsUSHORT;
    } PMCSR;

    //
    // PMCSR PCI-PCI Bridge Support Extensions
    //

    union {
        PCI_PMCSR_BSE   BridgeSupport;
        UCHAR           AsUCHAR;
    } PMCSR_BSE;

    //
    // Optional read only 8 bit Data register.  Contents controlled by
    // DataSelect and DataScale in ControlStatus.
    //

    UCHAR   Data;

} PCI_PM_CAPABILITY, *PPCI_PM_CAPABILITY;


//
// PCI-X Capability
//



typedef struct {

    PCI_CAPABILITIES_HEADER Header;

    union {
        struct {
            USHORT  DataParityErrorRecoveryEnable:1;
            USHORT  EnableRelaxedOrdering:1;
            USHORT  MaxMemoryReadByteCount:2;
            USHORT  MaxOutstandingSplitTransactions:3;
            USHORT  Reserved:9;
        } bits;
        USHORT  AsUSHORT;
    } Command;

    union {
        struct {
            ULONG   FunctionNumber:3;
            ULONG   DeviceNumber:5;
            ULONG   BusNumber:8;
            ULONG   Device64Bit:1;
            ULONG   Capable133MHz:1;
            ULONG   SplitCompletionDiscarded:1;
            ULONG   UnexpectedSplitCompletion:1;
            ULONG   DeviceComplexity:1;
            ULONG   DesignedMaxMemoryReadByteCount:2;
            ULONG   DesignedMaxOutstandingSplitTransactions:3;
            ULONG   DesignedMaxCumulativeReadSize:3;
            ULONG   ReceivedSplitCompletionErrorMessage:1;
            ULONG   CapablePCIX266:1;
            ULONG   CapablePCIX533:1;
        } bits;
        ULONG   AsULONG;
    } Status;
} PCI_X_CAPABILITY, *PPCI_X_CAPABILITY;


//
// PCI Express Extended Capabilities.
//

#define PCI_EXPRESS_ADVANCED_ERROR_REPORTING_CAP_ID                     0x0001
#define PCI_EXPRESS_VIRTUAL_CHANNEL_CAP_ID                              0x0002
#define PCI_EXPRESS_DEVICE_SERIAL_NUMBER_CAP_ID                         0x0003
#define PCI_EXPRESS_POWER_BUDGETING_CAP_ID                              0x0004
#define PCI_EXPRESS_RC_LINK_DECLARATION_CAP_ID                          0x0005
#define PCI_EXPRESS_RC_INTERNAL_LINK_CONTROL_CAP_ID                     0x0006
#define PCI_EXPRESS_RC_EVENT_COLLECTOR_ENDPOINT_ASSOCIATION_CAP_ID      0x0007
#define PCI_EXPRESS_MFVC_CAP_ID                                         0x0008
#define PCI_EXPRESS_VC_AND_MFVC_CAP_ID                                  0x0009
#define PCI_EXPRESS_RCRB_HEADER_CAP_ID                                  0x000A
#define PCI_EXPRESS_VENDOR_SPECIFIC_CAP_ID                              0x000B
#define PCI_EXPRESS_CONFIGURATION_ACCESS_CORRELATION_CAP_ID             0x000C
#define PCI_EXPRESS_ACCESS_CONTROL_SERVICES_CAP_ID                      0x000D
#define PCI_EXPRESS_ARI_CAP_ID                                          0x000E
#define PCI_EXPRESS_ATS_CAP_ID                                          0x000F
#define PCI_EXPRESS_SINGLE_ROOT_IO_VIRTUALIZATION_CAP_ID                0x0010
#define PCI_EXPRESS_MULTI_ROOT_IO_VIRTUALIZATION_CAP_ID                 0x0011
#define PCI_EXPRESS_MULTICAST_CAP_ID                                    0x0012
#define PCI_EXPRESS_PAGE_REQUEST_CAP_ID                                 0x0013
#define PCI_EXPRESS_RESERVED_FOR_AMD_CAP_ID                             0x0014
#define PCI_EXPRESS_RESIZABLE_BAR_CAP_ID                                0x0015
#define PCI_EXPRESS_DPA_CAP_ID                                          0x0016
#define PCI_EXPRESS_TPH_REQUESTER_CAP_ID                                0x0017
#define PCI_EXPRESS_LTR_CAP_ID                                          0x0018
#define PCI_EXPRESS_SECONDARY_PCI_EXPRESS_CAP_ID                        0x0019
#define PCI_EXPRESS_PMUX_CAP_ID                                         0x001A
#define PCI_EXPRESS_PASID_CAP_ID                                        0x001B
#define PCI_EXPRESS_LN_REQUESTER_CAP_ID                                 0x001C
#define PCI_EXPRESS_DPC_CAP_ID                                          0x001D
#define PCI_EXPRESS_L1_PM_SS_CAP_ID                                     0x001E
#define PCI_EXPRESS_PTM_CAP_ID                                          0x001F
#define PCI_EXPRESS_MPCIE_CAP_ID                                        0x0020
#define PCI_EXPRESS_FRS_QUEUEING_CAP_ID                                 0x0021
#define PCI_EXPRESS_READINESS_TIME_REPORTING_CAP_ID                     0x0022


//
// All Enhanced capabilities have the following header.
//

typedef struct _PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER {

    USHORT CapabilityID;
    USHORT Version:4;
    USHORT Next:12;

} PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER, *PPCI_EXPRESS_ENHANCED_CAPABILITY_HEADER;

//
// Vendor Specific Capability
//

typedef struct _PCI_EXPRESS_VENDOR_SPECIFIC_CAPABILITY {

    PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER Header;

    USHORT VsecId;
    USHORT VsecRev:4;
    USHORT VsecLength:12;

} PCI_EXPRESS_VENDOR_SPECIFIC_CAPABILITY, *PPCI_EXPRESS_VENDOR_SPECIFIC_CAPABILITY;

//
// Serial Number Capability.
//

typedef struct _PCI_EXPRESS_SERIAL_NUMBER_CAPABILITY {

    PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER Header;

    ULONG LowSerialNumber;
    ULONG HighSerialNumber;

} PCI_EXPRESS_SERIAL_NUMBER_CAPABILITY, *PPCI_EXPRESS_SERIAL_NUMBER_CAPABILITY;

//
// ARI Capability structures
//

typedef struct _PCI_EXPRESS_ARI_CAPABILITY_REGISTER {

    USHORT MfvcFunctionGroupsCapability:1;
    USHORT AcsFunctionGroupsCapability:1;
    USHORT Reserved:6;
    USHORT NextFunctionNumber:8;

} PCI_EXPRESS_ARI_CAPABILITY_REGISTER, *PPCI_EXPRESS_ARI_CAPABILITY_REGISTER;

typedef struct _PCI_EXPRESS_ARI_CONTROL_REGISTER {

    USHORT MfvcFunctionGroupsEnable:1;
    USHORT AcsFunctionGroupsEnable:1;
    USHORT Reserved1:2;
    USHORT FunctionGroup:3;
    USHORT Reserved2:9;

} PCI_EXPRESS_ARI_CONTROL_REGISTER, *PPCI_EXPRESS_ARI_CONTROL_REGISTER;

typedef struct _PCI_EXPRESS_ARI_CAPABILITY {

    PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER Header;

    PCI_EXPRESS_ARI_CAPABILITY_REGISTER Capability;
    PCI_EXPRESS_ARI_CONTROL_REGISTER Control;

} PCI_EXPRESS_ARI_CAPABILITY, *PPCI_EXPRESS_ARI_CAPABILITY;
    

//IOACPIAddress
union acpi_address {
    UInt64 addr64;
    struct {
        unsigned int offset     :16;
        unsigned int function   :3;
        unsigned int device     :5;
        unsigned int bus        :8;
        unsigned int segment    :16;
        unsigned int reserved   :16;
    } pci;
};


//IOACPIAddressSpaceDescriptor
struct acpi_address_space {
    uint32_t  resourceType;
    uint32_t  generalFlags;
    uint32_t  typeSpecificFlags;
    uint32_t  reserved1;
    uint64_t  granularity;
    uint64_t  minAddressRange;
    uint64_t  maxAddressRange;
    uint64_t  translationOffset;
    uint64_t  addressLength;
    uint64_t  reserved2;
    uint64_t  reserved3;
    uint64_t  reserved4;
};




  
#define arrayCount(x)    (sizeof(x) / sizeof(x[0]))
const uint32_t BUSES_PER_BLOCK = 32;
const uint32_t ECAM_BLOCK_SIZE = 32 * 0x100000;

#define  PCI_BASE_ADDRESS_MEM_MASK    (~0x0fUL)
#define PCI_CAPABILITY_LIST    0x34
#define  PCI_CAP_ID_VNDR    0x09
#define PCI_CONF1_BUS_SHIFT    16 /* Bus number */
#define PCI_CONF1_DEV_SHIFT    11 /* Device number */
#define PCI_CONF1_FUNC_SHIFT    8  /* Function number */

#define PCI_CONF1_BUS_MASK    0xff
#define PCI_CONF1_DEV_MASK    0x1f
#define PCI_CONF1_FUNC_MASK    0x7
#define PCI_CONF1_REG_MASK    0xfc /* Limit aligned offset to a maximum of 256B */

#define PCI_CONF1_ENABLE    BIT(31)
#define PCI_CONF1_BUS(x)    (((x) & PCI_CONF1_BUS_MASK) << PCI_CONF1_BUS_SHIFT)
#define PCI_CONF1_DEV(x)    (((x) & PCI_CONF1_DEV_MASK) << PCI_CONF1_DEV_SHIFT)
#define PCI_CONF1_FUNC(x)    (((x) & PCI_CONF1_FUNC_MASK) << PCI_CONF1_FUNC_SHIFT)
#define PCI_CONF1_REG(x)    ((x) & PCI_CONF1_REG_MASK)

#define PCI_CONF1_ADDRESS(bus, dev, func, reg) \
    (PCI_CONF1_ENABLE | \
     PCI_CONF1_BUS(bus) | \
     PCI_CONF1_DEV(dev) | \
     PCI_CONF1_FUNC(func) | \
     PCI_CONF1_REG(reg))

#define PCI_CONF1_EXT_REG_SHIFT    16
#define PCI_CONF1_EXT_REG_MASK    0xf00
#define PCI_CONF1_EXT_REG(x)    (((x) & PCI_CONF1_EXT_REG_MASK) << PCI_CONF1_EXT_REG_SHIFT)

#define PCI_CONF1_EXT_ADDRESS(bus, dev, func, reg) \
    (PCI_CONF1_ADDRESS(bus, dev, func, reg) | \
     PCI_CONF1_EXT_REG(reg))

#define PCI_BRIDGE_CONTROL    0x3e
#define  PCI_BRIDGE_CTL_PARITY    0x01    /* Enable parity detection on secondary interface */
#define  PCI_BRIDGE_CTL_SERR    0x02    /* The same for SERR forwarding */
#define  PCI_BRIDGE_CTL_ISA    0x04    /* Enable ISA mode */
#define  PCI_BRIDGE_CTL_VGA    0x08    /* Forward VGA addresses */
#define  PCI_BRIDGE_CTL_MASTER_ABORT    0x20  /* Report master aborts */
#define  PCI_BRIDGE_CTL_BUS_RESET    0x40    /* Secondary bus reset */
#define  PCI_BRIDGE_CTL_FAST_BACK    0x80    /* Fast Back2Back enabled on secondary interface */


#define PCI_SEC_LATENCY_TIMER    0x1b
#define  PCI_BRIDGE_CTL_BUS_RESET    0x40
#define upper_32_bits(n) ((uint32_t)((n) >> 32))
#define lower_32_bits(n) ((uint32_t)(n))  // Extracts lower 32 bits
    
#define PCI_COMMAND        0x04
#define  PCI_COMMAND_MEMORY    0x2
#define  PCI_COMMAND_MASTER    0x4
#define  PCI_COMMAND_IO        0x1
#define  PCI_COMMAND_PARITY    0x40
#define PCI_BRIDGE_CONTROL    0x3e
#define  PCI_BRIDGE_CTL_PARITY    0x01
#define  PCI_BRIDGE_CTL_SERR    0x02
#define PCI_CACHE_LINE_SIZE    0x0c    /* 8 bits */
#define PCI_LATENCY_TIMER    0x0d    /* 8 bits */
#define PCI_ROM_ADDRESS        0x30
#define L1_CACHE_SHIFT  6  // For 64-byte lines (2^6 = 64)
#define L1_CACHE_BYTES  (1 << L1_CACHE_SHIFT)

#define PCI_COMMAND_DECODE_ENABLE    (PCI_COMMAND_MEMORY | PCI_COMMAND_IO)
 
    

#define PCI_IO_BASE        0x1c    /* I/O range behind the bridge */
#define PCI_IO_LIMIT        0x1d
#define  PCI_IO_RANGE_TYPE_MASK    0x0fUL    /* I/O bridging type */
#define  PCI_IO_RANGE_TYPE_16    0x00
#define  PCI_IO_RANGE_TYPE_32    0x01
#define  PCI_IO_RANGE_MASK    (~0x0fUL) /* Standard 4K I/O windows */
#define  PCI_IO_1K_RANGE_MASK    (~0x03UL) /* Intel 1K I/O windows */
#define PCI_SEC_STATUS        0x1e    /* Secondary status register, only bit 14 used */
#define PCI_MEMORY_BASE        0x20    /* Memory range behind */
#define PCI_MEMORY_LIMIT    0x22
#define     PCI_PRIMARY_BUS   0x18 /* Primary bus number */
#define     PCI_SECONDARY_BUS   0x19 /* Secondary bus number */
#define     PCI_SUBORDINATE_BUS   0x1a /* Highest bus number behind the bridge */

#define VMD_SHADOW  0x2000

#define VMD_CFGBAR    0
#define VMD_MEMBAR1    2
#define VMD_MEMBAR2    4

#define PCI_REG_VMCAP        0x40
#define BUS_RESTRICT_CAP(vmcap)    (vmcap & 0x1)
#define PCI_REG_VMCONFIG    0x44
#define BUS_RESTRICT_CFG(vmcfg)    ((vmcfg >> 8) & 0x3)
#define VMCONFIG_MSI_REMAP    0x2
#define PCI_REG_VMLOCK        0x70
#define MB2_SHADOW_EN(vmlock)    (vmlock & 0x2)

#define MB2_SHADOW_OFFSET    0x2000
#define MB2_SHADOW_SIZE        16

#define PCI_HEADER_TYPE        0x0e
#define  PCI_HEADER_TYPE_MFD        0x80
#define  PCI_HEADER_TYPE_MASK        0x7f
#define  PCI_HEADER_TYPE_NORMAL        0
#define  PCI_HEADER_TYPE_BRIDGE        1
#define  PCI_HEADER_TYPE_CARDBUS    2
#define PCI_CLASS_DEVICE    0x0a
#define PCI_CLASS_BRIDGE_PCI        0x0604
#define PCI_IO_BASE_UPPER16    0x30
#define PCI_IO_BASE        0x1c
#define PCI_MEMORY_BASE        0x20
#define PCI_PREF_LIMIT_UPPER32    0x2c
#define PCI_PREF_MEMORY_BASE    0x24
#define PCI_PREF_BASE_UPPER32    0x28
#define PCI_DEVFN(slot, func)    ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_BUSMAX    255

#define PCIE_ECAM_BUS_SHIFT    20 /* Bus number */
#define PCIE_ECAM_DEVFN_SHIFT    12 /* Device and Function number */

#define PCIE_ECAM_BUS_MASK    0xff
#define PCIE_ECAM_DEVFN_MASK    0xff
#define PCIE_ECAM_REG_MASK    0xfff /* Limit offset to a maximum of 4K */

#define PCIE_ECAM_BUS(x)    (((x) & PCIE_ECAM_BUS_MASK) << PCIE_ECAM_BUS_SHIFT)
#define PCIE_ECAM_DEVFN(x)    (((x) & PCIE_ECAM_DEVFN_MASK) << PCIE_ECAM_DEVFN_SHIFT)
#define PCIE_ECAM_REG(x)    ((x) & PCIE_ECAM_REG_MASK)

#define PCIE_ECAM_OFFSET(bus, devfn, where) \
    (PCIE_ECAM_BUS(bus) | \
     PCIE_ECAM_DEVFN(devfn) | \
     PCIE_ECAM_REG(where))

template <class T, size_t N>
constexpr size_t arrsize(const T (&array)[N]) {
    return N;
}

#define UNREACHABLE() do { __builtin_unreachable(); } while (0)
#define PANIC_COND(cond, module, str, ...)                                                                     \
    do {                                                                                                       \
        if (cond) {                                                                                            \
            (panic)( "%s%10s: @ " str "\n", "VMD", "1.0", ## __VA_ARGS__);  \
            UNREACHABLE();                                                                                     \
        }                                                                                                      \
    } while (0)

#define BUS_RESTRICT    1
#define VMD_CAP        0x40
#define VMD_CONFIG    0x44
#define VMD_BUS_RESTRICT    0x1

// VMD Interrupt Registers (Intel VMD Technical Reference)
#define VMD_INTR_CTRL      0xE0  // Interrupt Control Register
#define VMD_INTR_STATUS    0xE4  // Interrupt Status Register
#define VMD_INTR_MASK      0xE8  // Interrupt Mask Register

// Interrupt Status Bits
enum {
    VMD_INTR_PENDING       = (1 << 0),  // Global interrupt pending
    VMD_INTR_MSI_RECEIVED  = (1 << 1),  // MSI message received
    VMD_INTR_ERR_DETECTED  = (1 << 2)   // Error detected
};

// VMD Capability Registers
#define kVMDVendorSpecificCapability  0x0C
#define PCI_CAP_LIST_ID     0x00
#define PCI_CAP_LIST_NEXT   0x01
#define PCI_VNDR_HEADER     0x04

#define PCI_VMD_CAP_MEMBAR1     0x14
#define PCI_VMD_CAP_MEMBAR2     0x18
#define PCI_VMD_CAP_MEMBAR_SHADOW 0x1C

// MEMBAR Memory Space Offset (from FreeBSD)
#define VMD_MEM_SPACE_OFFSET    0x1000
#define PCI_EXT_CAP_ID_LTR    0x18
#define PCI_LTR_MAX_SNOOP_LAT    0x4
#define PCI_LTR_MAX_NOSNOOP_LAT    0x6
#define  PCI_LTR_VALUE_MASK    0x000003ff
#define  PCI_LTR_SCALE_MASK    0x00001c00
#define  PCI_LTR_SCALE_SHIFT    10
    
#define VMD_MEMBAR           0x2000  // MEMBAR offset in config space
#define VMD_MEMBAR_SHADOW    0x2004  // Legacy shadow offset
#define VMD_MEMBAR_VSCAP     0x2208  // VSCAP shadow offset
#define VMD_MEM_SPACE_OFFSET 0x1000  // MEM space offset in BAR1

enum VMD_FEATURES {
    VMD_FEAT_HAS_MEMBAR_SHADOW        = (1 << 0),
    VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP  = (1 << 1),
    VMD_FEAT_HAS_BUS_RESTRICTIONS     = (1 << 2)
};

enum {
    VMD_MAX_BAR = 3,
    VMD_FLAG_MSI_REMAP    = (1 << 0),
    VMD_FLAG_HAS_MEMBAR   = (1 << 1),
    VMD_FLAG_BUS_RESTRICT = (1 << 2)
};

class VMDChildDevice;
class VMDInterruptController;

class VMDDriver : public IOPCI2PCIBridge {
    friend class IOPCIBridge;
    friend class IOPCIEventSource;
    friend class IOPCIDevice;
    
    OSDeclareDefaultStructors(VMDDriver)
private:
    bool addBridgePrefetchableMemoryRange( addr64_t start, addr64_t length );

public:
    
    VMDDriver *nd;
    
    uint8_t busStart;
    uint8_t busEnd;
    uint8_t busStart2;
   uint8_t busEnd2;
    IOMemoryMap *barMaps[6];
    
    volatile uint8_t *vmdConfigBase1 { nullptr };
    
    IOSimpleLock *lock;
    // static const VMDDeviceInfo supportedDevices[];
    IOPCIDevice *pciDevice;
    VMDChildDevice *childs[12];
    IOPCIAddressSpace aBridgeSpace;
    VMDChildDevice *pciDeviceB;
    VMDChildDevice *pciDeviceC;
    
    volatile UInt32*  _msixTable;
    IOMemoryMap*      _msixMap;
    
    volatile UInt32*  msiPBATable;
    IOMemoryMap*  msiPBAMap;
    
    IOWorkLoop *workLoop;
    IOCommandGate* command_gate;
    IORecursiveLock *childLock;
    UInt32              numVectors;
    IOInterruptEventSource** controllerInt;
   
    uint32_t                fNextID=0x16;
  // virtual IODeviceMemory * ioDeviceMemory( void ) override;

    UInt32 nextVector=2;
    VMDInterruptController *interruptController;
    IOPCIConfigEntry * bridge;
    IOPCIConfigEntry * bridge2;
    
   // virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;
    
    void configure1( IOService * provider ) ;
    
    void scanb( IOService * provider, int ii ) ;

    virtual UInt8 firstBusNum( void ) override ;
    virtual UInt8 lastBusNum( void ) override ;
    
    
    /*
    virtual UInt32 configRead32( IOPCIAddressSpace space, UInt8 offset ) override  ;
    virtual void configWrite32( IOPCIAddressSpace space,UInt8 offset, UInt32 data ) override  ;
    
    virtual UInt16 configRead16( IOPCIAddressSpace space, UInt8 offset ) override  ;
    virtual void configWrite16( IOPCIAddressSpace space,UInt8 offset, UInt16 data ) override  ;
    
    virtual UInt8 configRead8( IOPCIAddressSpace space, UInt8 offset ) override ;
    virtual void configWrite8( IOPCIAddressSpace space,
                              UInt8 offset, UInt8 data ) override ;
    */
    virtual IOPCIAddressSpace getBridgeSpace( void ) override;
    
    void resetDomain() ;
    OSArray * decomposeAddressingProperties(const char *propName, OSData *data);
    void configurePCISpace(const char *propName,IOService *pciDevice,
                           IOPCIAddressSpace addrSpace,
                           UInt64 address,
                           UInt64 length);
    
    IOWorkLoop* getWorkLoop() const override;
    
    virtual void handleInterrupt0( OSObject* owner, IOInterruptEventSource* src, int intCount );
    
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source);
    static void interruptHandler(OSObject *owner, IOInterruptEventSource *source, int count);

    virtual UInt32 findPCICapability(IOPCIAddressSpace space, UInt32 capabilityID, UInt8 * found );
    
    //void handleInterrupt( IOInterruptEventSource * source,int count );
    
    void vacpi(IOPCIAddressSpace space, bool on);
    
};


class VMDChildDevice : public IOPCIDevice {

    OSDeclareDefaultStructors(VMDChildDevice)
    
public:
    
    uint32_t irq;
    IOMemoryMap *barMaps[6];//VMD_MAX_BAR
  //  uint8_t busStart;
  //  uint8_t busEnd;
    IOPCIDevice *pciDevice;
    volatile uint8_t *vmdConfigBase { nullptr };
    volatile uint8_t *vmdConfigBase1 { nullptr };

    IOSimpleLock *lock;
    IOPhysicalAddress offset1 ;
    IOPhysicalAddress offset2 ;
    IOMemoryDescriptor* membar;
    IOMemoryDescriptor* membarDesc;
    uint32_t configOffset;
    bool isBridge=false;
    bool isRoot=false;
    uint32_t expressCapability;
    uint32_t powerCapability;
    
    UInt32 classCode;
    IOInterruptEventSource **controllerInt;
    IOPCIConfigEntry * bridge;
    
    IOWorkLoop *workLoop = nullptr;
    IOCommandGate* command_gate = nullptr;
    IORecursiveLock *childLock;
    
    uint32_t vmdVector = 2;
    UInt32              numVectors=1;
    
    
    volatile UInt32*  _msixTable;
    IOMemoryMap*      _msixMap;
    
    IOSubMemoryDescriptor *msiPBA_desc;
            IOMemoryMap          *msiPBA_map;
            IOMemoryMap          *msiPBA_barMap;
            IOVirtualAddress      msiPBA_virt;
            uint64_t              msiPBA_phys;
   
    VMDDriver *pvmd;
    VMDInterruptController *interruptController;
    IOACPIPlatformDevice *acpiDevice;
    IORegistryEntry *   dtEntry;
    
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;
    void configurePCISpace(const char *propName, IOService *pciDevice,
                           IOPCIAddressSpace addrSpace,
                           UInt64 address,
                           UInt64 length);
    

   // virtual IOReturn getResources( void ) override;
    //virtual IODeviceMemory * ioDeviceMemory( void ) override;
    void setmem(IOPCIBridge *   hostBridge);
    UInt32 decmem(IODeviceMemory::InitElement *rangeList, bool isb,  IOPCIConfigEntry * bridge);
   // IOReturn getResources( void ) override;;
    IOWorkLoop* getWorkLoop() const override;

    void constructAddressingProperties(IOPCIConfigEntry * device, OSDictionary * propTable);
    virtual IOReturn getInterruptType(int source, int *interruptType) override;
    virtual IOReturn registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refcon) override;
    virtual IOReturn enableInterrupt(int source) override;
    virtual IOReturn disableInterrupt(int source) override;
    virtual void handleInterrupt0(OSObject* owner, IOInterruptEventSource* src, int intCount);
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source);
    static void interruptHandler(OSObject *owner, IOInterruptEventSource *source, int count);
    
    uint32_t findPCICapability2(uint32_t capabilityID, uint32_t * found);

};






class VMDInterruptController : public IOInterruptController {
    OSDeclareDefaultStructors(VMDInterruptController);
    
public:
    // IOService/IOInterruptController overrides
    virtual bool init(IOService* provider);
    virtual void free() override;
    
    virtual IOReturn registerInterrupt(IOService *nub, int source,
                                      void *target, IOInterruptHandler handler,
                                      void *refCon) override;
                                      
    virtual IOInterruptAction getInterruptHandlerAddress(void) override;
    virtual IOReturn getInterruptType(IOService *nub, int source,
                                     int *interruptType) override ;
    
    
    VMDDriver *pvmd;
    UInt8 Cap;
    
    IOReturn enableInterrupt(IOService* nub, int source) override ;
    IOReturn disableInterrupt(IOService* nub, int source) override ;

       volatile UInt32* getChildMSIXEntry(VMDChildDevice* child, int source, IOMemoryMap** map);
    virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source) override;
    virtual IOReturn handleInterrupt2(void *refCon, IOService *nub, int source) ;
                                   
//private:
    struct ChildInterrupt {
        IOService* childNub;
        int childSource;
        int vmdSource;
        IOInterruptHandler handler;
        void* target;
        void* refCon;
    };
    IOWorkLoop *workLoop = nullptr;
    void programChildMSI(VMDChildDevice* child, int childSource,
                         IOInterruptVectorNumber vmdVector);

    void programChildMSIX(VMDChildDevice* child, int childSource,
                         IOInterruptVectorNumber vmdVector);
    bool ismsix;
    
    VMDChildDevice*      _vmdDevice;
    volatile UInt32*  _msixTable;
    IOMemoryMap*      _msixMap;
    OSArray*          _vectors;
    IOLock*           _lock;
    UInt32            _nextVector=1;
    uint32_t vmdVector = 2;
    uint32_t numVectors = 1;
};


class ChildInterruptRecord : public OSObject {
    OSDeclareDefaultStructors(ChildInterruptRecord);
public:
    struct ChildInterrupt {
        IOService* childNub;
        int childSource;
        int vmdSource;
        IOInterruptHandler handler;
        void* target;
        void* refCon;
    };
    
    ChildInterrupt data;
    
    static ChildInterruptRecord* create() {
        ChildInterruptRecord* rec = new ChildInterruptRecord;
        if (rec && rec->init()) {
            return rec;
        }
        if (rec) rec->release();
        return nullptr;
    }
    
    virtual void free() override {
        OSObject::free();
    }
};








#endif /* VMDDriver_h */
