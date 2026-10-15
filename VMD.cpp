#include "VMD.hpp"

#define super IOService

OSDefineMetaClassAndStructors(VMDDriver, IOPCI2PCIBridge)
OSDefineMetaClassAndStructors(VMDChildDevice, IOPCIDevice)
OSDefineMetaClassAndStructors(VMDInterruptController, IOInterruptController);
OSDefineMetaClassAndStructors(ChildInterruptRecord, OSObject);


/*
const VMDDriver::VMDDeviceInfo VMDDriver::supportedDevices[] = {
    { 0x8086, 0x201d,  0 },
    { 0x8086, 0x28c0,  BUS_RESTRICT | VMD_FLAG_HAS_MEMBAR },
    { 0x8086, 0x467f,  BUS_RESTRICT | VMD_FLAG_MSI_REMAP | VMD_FLAG_HAS_MEMBAR },
    { 0x8086, 0x4c3d,  BUS_RESTRICT | VMD_FLAG_MSI_REMAP | VMD_FLAG_HAS_MEMBAR },
    { 0x8086, 0x7d0b,  BUS_RESTRICT | VMD_FLAG_MSI_REMAP | VMD_FLAG_HAS_MEMBAR },
    { 0x8086, 0x9a0b,  BUS_RESTRICT | VMD_FLAG_MSI_REMAP | VMD_FLAG_HAS_MEMBAR },
    { 0x8086, 0xa77f,  BUS_RESTRICT | VMD_FLAG_MSI_REMAP | VMD_FLAG_HAS_MEMBAR },
    { 0x8086, 0xad0b,  BUS_RESTRICT | VMD_FLAG_MSI_REMAP | VMD_FLAG_HAS_MEMBAR },
    { 0, 0, 0 }

};
*/

volatile uint8_t *vmdConfigBase { nullptr };
VMDDriver *vmdb;

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

OSArray * VMDDriver::decomposeAddressingProperties(const char *propName, OSData *data) {
   // auto decompose = [](const char *propName, OSData *data) -> OSArray* {
        if (!data) return nullptr;
        
        const UInt8 *bytes = static_cast<const UInt8*>(data->getBytesNoCopy());
        size_t length = data->getLength();
        OSArray *array = OSArray::withCapacity(length / 20); // 5 × 32-bit values
        
        for (size_t offset = 0; offset + 20 <= length; offset += 20) {
            IOPCIAddressSpace addrSpace;
            UInt32 physMid, physLo, lenHi, lenLo;
            
            // Little-endian to host conversion
            addrSpace.bits = OSSwapLittleToHostInt32(*reinterpret_cast<const UInt32*>(bytes + offset));
            physMid = OSSwapLittleToHostInt32(*reinterpret_cast<const UInt32*>(bytes + offset + 4));
            physLo = OSSwapLittleToHostInt32(*reinterpret_cast<const UInt32*>(bytes + offset + 8));
            lenHi = OSSwapLittleToHostInt32(*reinterpret_cast<const UInt32*>(bytes + offset + 12));
            lenLo = OSSwapLittleToHostInt32(*reinterpret_cast<const UInt32*>(bytes + offset + 16));
            
            OSDictionary *dict = OSDictionary::withCapacity(8);
            if (!dict) continue;
            
            // Decode space type
            const char *spaceType = "Unknown";
            switch (addrSpace.s.space) {
                case kIOPCI32BitMemorySpace:
                    spaceType = addrSpace.s.prefetch ? "PrefetchableMemory32" : "Memory32";
                    break;
                case kIOPCI64BitMemorySpace:
                    spaceType = addrSpace.s.prefetch ? "PrefetchableMemory64" : "Memory64";
                    break;
                case kIOPCIIOSpace:
                    spaceType = "IO";
                    break;
                case kIOPCIConfigSpace:
                    spaceType = "Config";
                    break;
            }
            
            // Construct values
            UInt64 address = (static_cast<UInt64>(physMid) << 32) | physLo;
            UInt64 length = (static_cast<UInt64>(lenHi) << 32) | lenLo;
            
            // Populate dictionary
            dict->setObject("space", OSString::withCString(spaceType));
            dict->setObject("address", OSNumber::withNumber(address, 64));
            dict->setObject("length", OSNumber::withNumber(length, 64));
            dict->setObject("prefetchable", addrSpace.s.prefetch ? kOSBooleanTrue : kOSBooleanFalse);
            dict->setObject("relocatable", addrSpace.s.reloc ? kOSBooleanTrue : kOSBooleanFalse);
            dict->setObject("register", OSNumber::withNumber(addrSpace.s.registerNum, 8));
            dict->setObject("bus", OSNumber::withNumber(addrSpace.s.busNum, 8));
            dict->setObject("device", OSNumber::withNumber(addrSpace.s.deviceNum, 5));
            dict->setObject("function", OSNumber::withNumber(addrSpace.s.functionNum, 3));
            
            array->setObject(dict);
            dict->release();
        }
        return array;
  //  };
}
/*
bool VMDDriver::init( OSDictionary *  propTable )
{


}
 
*/



/*
class VMDProbe : public IOService {
    OSDeclareDefaultStructors(VMDProbe);
    
    IOMemoryMap*          ecamMap;
    volatile uint32_t*    ecamBase;
    
public:
    virtual bool start(IOService* provider) override {
       // if (!super::start(provider)) return false;
        
        IOACPIPlatformDevice* vmd0 = OSDynamicCast(IOACPIPlatformDevice, provider);
        if (!vmd0) return false;
        
        // Map ECAM space (base address 0xC0000000 in this example)
        IOMemoryDescriptor* ecamDesc = IOMemoryDescriptor::withPhysicalAddress(
            0xC0000000, 0x10000000, kIODirectionInOut);
        if (!ecamDesc) return false;
        
        ecamMap = ecamDesc->map();
        if (!ecamMap) {
            ecamDesc->release();
            return false;
        }
        ecamBase = (volatile uint32_t*)ecamMap->getVirtualAddress();

        IOLog("Starting VMD0 device probe\n");
        
        if (1)
        {
        
        UInt8 uuidBytes[16] = {
        0xdd, 0xcd, 0xd9, 0x9c, // Time low (little-endian)
        0x45, 0x88,             // Time mid (little-endian)
        0xfd, 0x4a,             // Time high (little-endian)
        0x83, 0x92,             // Clock sequence (big-endian)
        0x31, 0xc4, 0xeb, 0x87, 0xca, 0xbd // Node (big-endian)
        };
        OSData *uuidData = OSData::withBytes(uuidBytes, sizeof(uuidBytes));
        
        OSArray *d3Params = OSArray::withCapacity(1);
        // VD3A = 0 (D0 state)
        d3Params->setObject( OSNumber::withNumber(0ULL, 32));
        // VD3A = 1=D3 power state  (low-power mode)
        
        OSObject *dsmArgsD3[4] = {
        uuidData,                           // Arg0: UUID
        OSNumber::withNumber(1ULL, 32),     // Arg1: Revision (0)
        OSNumber::withNumber(0x05ULL, 32),  // Arg2: Function Index 5 (VD3A)
        d3Params                            // Arg3: VD3A value
        };
        
            vmd0->evaluateObject("_DSM", nullptr, dsmArgsD3, 4);
        d3Params->release();
        
        }
        
        if (probeVMD0Children(vmd0)) {
            IOLog("Active devices found in VMD0 hierarchy\n");
        } else {
            IOLog("No active devices in VMD0 hierarchy\n");
        }
        
        return true;
    }
    
    virtual void free() override {
        if (ecamMap) ecamMap->release();
        //super::free();
    }
    
private:
    bool probeVMD0Children(IOACPIPlatformDevice* vmd0) {
        
       // OSIterator *childIterator = vmd0->getChildIterator(gIOACPIPlane);
        IORegistryIterator * iter=(IORegistryIterator*)vmd0->getChildIterator(gIODTPlane);;
        OSOrderedSet *       all;
        all = iter->iterateAll();
        iter->release();
        
        IOACPIPlatformDevice *child;
           // if (!childIterator) return false;

            bool foundActive = false;
            
           // while (OSObject *obj = all->getNextObject()) {
        for ( ; (child = (IOACPIPlatformDevice *) all->getFirstObject()); all->removeObject(child)){
               // IOACPIPlatformDevice *child = OSDynamicCast(IOACPIPlatformDevice, obj);
                if (!child) continue;

                const char *name = child->getName();
                IOLog("Checking child device: %s\n", name);
                
                if (!powerOnChildViaVMD0(vmd0, child)) {
                            IOLog("Failed to power on %s\n", child->getName());
                           // continue;
                        }

                if (isRPDevice(name)) {
                    if (!checkRootPort(child)) addChildToProvider(vmd0, child);;
                }
                else if (isPEGDevice(name)) {
                    if (!checkPEG(child)) addChildToProvider(vmd0, child);;
                }
                else if (isPRTDevice(name)) {
                    if (!checkPRT(child)) addChildToProvider(vmd0, child);;
                }
                else if (isVOLDevice(name)) {
                    if (!checkVOL(child)) addChildToProvider(vmd0, child);;
                }
            }

               // childIterator->release();
                return foundActive;
   
    }
    
    bool addChildToProvider(IOACPIPlatformDevice* provider, IOACPIPlatformDevice* child) {
        // 1. Get child's PCI address from ACPI _ADR
        uint64_t adr;
        if (child->evaluateInteger("_ADR", &adr) != kIOReturnSuccess) {
            IOLog("Failed to get _ADR for child device\n");
            return false;
        }

        // 2. Extract PCI location
        uint8_t bus = 0;  // Assuming VMD0 is bus 0
        uint8_t device = (adr >> 16) & 0xFF;
        uint8_t function = adr & 0xFF;

        // 3. Calculate ECAM offset
        uint32_t ecamOffset = (bus << 20) | (device << 15) | (function << 12);



        // 5. Create PCI device instance
        IOPCIDevice* pciChild = new IOPCIDevice;
           if (!pciChild || !pciChild->init(0)) {
               OSSafeReleaseNULL(pciChild);
               return false;
           }
        
        

        uint32_t cfgOffset = (bus << 20) | (device << 15) | (function << 12);
           
           // Read standard PCI headers
           uint32_t vendorDevice = OSReadLittleInt32(ecamBase, cfgOffset);
           uint32_t classRev = OSReadLittleInt32(ecamBase, cfgOffset + 0x08);
           
           pciChild->setProperty("vendor-id",    (vendorDevice & 0xFFFF),     16);
           pciChild->setProperty("device-id",    (vendorDevice >> 16),        16);
           pciChild->setProperty("class-code",   (classRev >> 24),            24);
           pciChild->setProperty("revision-id",  (classRev & 0xFF),           8);
           pciChild->setProperty("bus-number",   bus,                         8);
           pciChild->setProperty("device-number", device,                     8);
           pciChild->setProperty("function-number", function,                 8);

           // 4. Set ACPI properties
           pciChild->setProperty("acpi-path", child->getLocation());
           
           // 5. Create location string
           char location[32];
           snprintf(location, sizeof(location), "%x,%x,%x", bus, device, function);
           pciChild->setProperty("location", OSString::withCString(location));


        pciChild->attachToParent(provider, gIOServicePlane);
        //pciChild->registerService();

        return true;
    }

    bool powerOnChildViaVMD0(IOACPIPlatformDevice* vmd0, IOACPIPlatformDevice* child) {
        
        uint64_t childAdr=child->evaluateInteger("_ADR", &childAdr);

        UInt8 uuidBytes[16] = {
                0xdd, 0xcd, 0xd9, 0x9c, // Time low (little-endian)
                0x45, 0x88,             // Time mid (little-endian)
                0xfd, 0x4a,             // Time high (little-endian)
                0x83, 0x92,             // Clock sequence (big-endian)
                0x31, 0xc4, 0xeb, 0x87, 0xca, 0xbd // Node (big-endian)
            };
            OSData *uuidData = OSData::withBytes(uuidBytes, sizeof(uuidBytes));

        OSArray *powerOnParams = OSArray::withCapacity(1);
        powerOnParams->setObject(OSNumber::withNumber((UInt32)childAdr, 32));

        
            OSObject *dsmArgsPowerOff[4] = {
                uuidData,                           // Arg0: UUID
                OSNumber::withNumber(0ULL, 32),     // Arg1: Revision (0)
                OSNumber::withNumber(0x03ULL, 32),  // Arg2: Function Index 3 (VLOff)
                powerOnParams                       // Arg3: Package of 32 integers
            };

        vmd0->evaluateObject("_DSM", nullptr, dsmArgsPowerOff, 4);
        
        OSObject * dsmArgsPowerOn[4] = {
            uuidData,                           // Arg0: UUID
            OSNumber::withNumber(0ULL, 32),     // Arg1: Revision (0)
            OSNumber::withNumber(0x02ULL, 32),  // Arg2: Function Index 2 (VLON)
            powerOnParams                       // Arg3: Package of 32 integers
        };
        IOReturn ret =vmd0->evaluateObject("_DSM", nullptr, dsmArgsPowerOn, 4);
        
        powerOnParams->release();
        
        return ret;
    }


    // Device Type Checkers
    bool isRPDevice(const char* name) {
        if (strncmp(name, "RP", 2) != 0) return false;
        if (strlen(name) != 4) return false;
        
        char d1 = name[2];
        char d2 = name[3];
        
        // IOKit-safe digit check
        bool d1Valid = (d1 >= '0' && d1 <= '9');
        bool d2Valid = (d2 >= '0' && d2 <= '9');
        if (!d1Valid || !d2Valid) return false;
        
        int num = (d1 - '0') * 10 + (d2 - '0');
        return (num >= 1 && num <= 24);
    }

    bool isPEGDevice(const char* name) {
        if (strncmp(name, "PEG", 3) != 0) return false;
        if (strlen(name) != 4) return false;
        char d = name[3];
        return (d >= '0' && d <= '3');  // Already IOKit-safe
    }

    bool isPRTDevice(const char* name) {
        if (strncmp(name, "PRT", 3) != 0) return false;
        if (strlen(name) != 4) return false;
        char d = name[3];
        return (d >= '0' && d <= '7');  // Direct comparison
    }

    bool isVOLDevice(const char* name) {
        if (strncmp(name, "VOL", 3) != 0) return false;
        if (strlen(name) != 4) return false;
        char d = name[3];
        return (d >= '0' && d <= '3');  // Direct comparison
    }


    bool checkRootPort(IOACPIPlatformDevice* rp) {
        UInt32 id3c, lasx;
        //if (rp->evaluateInteger("ID3C", &id3c) == kIOReturnSuccess && id3c == 0) {
            if (rp->evaluateInteger("LASX", &lasx) == kIOReturnSuccess && lasx) {
                IOLog("Root Port %s link active\n", rp->getName());
                return true;
            }
       // }
        return false;
    }

    bool checkPEG(IOACPIPlatformDevice* peg) {
        UInt32 id3c=0;
       // if (peg->evaluateInteger("ID3C", &id3c) != kIOReturnSuccess) return false;
        
        if (id3c == 0) {
            UInt32 lasx;
            if (peg->evaluateInteger("LASX", &lasx) == kIOReturnSuccess && lasx) {
                IOLog("%s link active\n", peg->getName());
                return true;
            }
            
            // Fallback to PCI config check
            uint64_t adr;
            if (peg->evaluateInteger("_ADR", &adr) == kIOReturnSuccess) {
                uint8_t bus = (adr >> 16) & 0xFF;
                uint8_t dev = (adr >> 11) & 0x1F;
                uint8_t func = adr & 0x07;
                
                uint32_t cfgOffset = (bus << 20) | (dev << 15) | (func << 12);
                uint32_t vendorId = OSReadLittleInt32(ecamBase, cfgOffset);
                if (vendorId != 0xFFFFFFFF) {
                    IOLog("%s PCI device present (VID: 0x%04X)\n", peg->getName(), vendorId & 0xFFFF);
                    return true;
                }
            }
        }
        return false;
    }

    bool checkPRT(IOACPIPlatformDevice* prt) {
        UInt32 sd3c;
        if (prt->evaluateInteger("SD3C", &sd3c) == kIOReturnSuccess && sd3c == 0) {
            IOLog("%s in active state\n", prt->getName());
            return true;
        }
        return false;
    }

    bool checkVOL(IOACPIPlatformDevice* vol) {
        UInt32 rtd3 = 0, vpr = 0;
        vol->evaluateInteger("RTD3", &rtd3);
        
        if (rtd3) {
            char resName[5];
            snprintf(resName, sizeof(resName), "V%cPR", vol->getName()[3]);
            if (vol->evaluateInteger(resName, &vpr) == kIOReturnSuccess && vpr) {
                IOLog("%s power resource active\n", vol->getName());
                return true;
            }
        } else {
            UInt32 status;
            if (vol->evaluateInteger("_STA", &status) != kIOReturnSuccess) status = 0x0F;
            if (status & 0x1) return true;
        }
        return false;
    }
};

OSDefineMetaClassAndStructors(VMDProbe, IOService);
*/


IOWorkLoop *VMDDriver::getWorkLoop() const {

    return workLoop;
}



bool VMDDriver::interruptFilter(OSObject *owner,  IOFilterInterruptEventSource *source)
{

   /* VMDDriver *device = (VMDDriver*)(owner);
    if (!device)
        return false;

    int interruptType;
    
    if (source->getIntIndex()==0) return false;
    device->getInterruptType(source->getIntIndex(), &interruptType);
    if (interruptType & kIOInterruptTypePCIMessaged) return true;
    if (interruptType & kIOInterruptTypePCIMessagedX) return true;
    
    return false;*/
    return true;
}

void VMDDriver::interruptHandler(OSObject *owner,  IOInterruptEventSource *source,
         int count)
{

   // int intCoun=source->getIntIndex();
  //
   // if (intCoun>=1) panic ("x");
    
    VMDDriver *device = static_cast<VMDDriver*>(owner);
    if (!device)
        return ;
    
    device->handleInterrupt0(owner,source,count);
    //panic("x");
}

void VMDDriver::handleInterrupt0( OSObject* owner, IOInterruptEventSource* src, int intCount )
{
    
   // IOInterruptState ints = IOSimpleLockLockDisableInterrupt( lock );
    int intCoun=src->getIntIndex();
  
   // if (intCoun>1) panic ("x");
    
   // if (intCoun>2)
  //  src->normalInterruptOccurred(nullptr, (IOService *)owner, 0);
    
    if (intCoun==0) {
       return ;
   }
    if (intCoun==1) {
        //IOPCI2PCIBridge::handleInterrupt(src,intCount);
       // childs[0]->interruptController->handleInterrupt2(nullptr,childs[0],1);
       return ;
   }

/*
    for (int i=0;i<12;i++){
            if (childs[i] && childs[i]->interruptController) {
               /* int vi=  childs[i]->vmdVector;
                int vf=  childs[i]->numVectors;
                if (intCoun>=vi && (intCoun<(vi+vf)))
                {
                   // childs[i]->interruptController->handleInterrupt2(nullptr,childs[i],intCoun);
                    //break;
               // }
            }
        }*/
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
}



IOPCIAddressSpace VMDDriver::getBridgeSpace( void )
{
    return aBridgeSpace;
}

bool VMDDriver::addBridgePrefetchableMemoryRange( addr64_t start, addr64_t length )
{
    return IOPCIBridge::addBridgePrefetchableMemoryRange(start,length);
}

void VMDDriver::configure1( IOService * provider )
{
    //setProperty("domain-id", 0x10000,32);
    
    pciDevice = OSDynamicCast(IOPCIDevice, provider);

    if (!pciDevice) return;
    
    workLoop = IOWorkLoop::workLoop();
    workLoop->retain();
    lock = IOSimpleLockAlloc();
    childLock=IORecursiveLockAlloc();
    
    uint16_t reg0;
    
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
    
    aBridgeSpace=space;

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
       addBridgeMemoryRange(address, length, true);
    
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
    
     address=0x6003100000+0x2000;
     length=0x100000-0x2000;
     addBridgePrefetchableMemoryRange(address, length);
    
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
        
    
    
   // command_gate = IOCommandGate::commandGate(this);
   // workLoop->addEventSource(command_gate);
    
    pciDevice->configWrite32(kIOPCIConfigBaseAddress0,  0);
    //pciDevice->configWrite32(kIOPCIConfigBaseAddress4,  0x03102000);
    pciDevice->configWrite32(kIOPCIConfigBaseAddress4,  0x03100000);
    
    //pciDevice->reset(kIOPCIDeviceResetTypeHotReset);
    
    pciDevice->removeProperty("IOPCIResourced");
    pciDevice->getResources();
    //pciDevice->relocate(false);
    pciDevice->retain();
    pciDevice->open(this);
    
    pciDevice->setBusMasterEnable(true);
    pciDevice->setMemoryEnable(true);
    //pciDevice->setIOEnable(true);
    
    barMaps[0]=pciDevice->mapDeviceMemoryWithIndex(0);
    //barMaps[1]=pciDevice->mapDeviceMemoryWithIndex(1);
    //barMaps[2]=pciDevice->mapDeviceMemoryWithIndex(2);
    
    vmdConfigBase = (volatile uint8_t *) ((barMaps[0]->getVirtualAddress()) );
    vmdConfigBase1=vmdConfigBase;
    
    
 
    

    uint8_t cap;
    if (pciDevice->findPCICapability( kIOPCICapabilityIDMSIX, &cap)) {
        
        UInt16 ctrl = pciDevice->configRead16(cap + 2);
        UInt32 numVectors0 = (ctrl & 0x7FF) + 1;
        
        pciDevice->configureInterrupts(
            kIOInterruptTypePCIMessagedX,
            numVectors0,
            numVectors0
        );
        
        numVectors=numVectors0;
        
        UInt32 table  = pciDevice->configRead32(cap + 4);
        UInt8 bar    = (kIOPCIConfigBaseAddress0 + ((table & 7) << 2));
        table &= ~7;
        
       /* _msixMap = pciDevice->mapDeviceMemoryWithRegister(bar);
        if (!_msixMap) return ;
        _msixTable = (volatile UInt32*)(_msixMap->getAddress() + table);
        
       /* table = pciDevice->configRead32(cap + 8);
        bar = kIOPCIConfigBaseAddress0 + ((table & 7) << 2);
        table &= ~7;
        msiPBAMap = pciDevice->mapDeviceMemoryWithRegister(bar);
        msiPBATable= (volatile UInt32*)(msiPBAMap->getAddress() + table);*/

        controllerInt=IONew(IOInterruptEventSource*, numVectors0);
        for (int i = 0; i < 1; i++) {
            
            
            controllerInt[i] = IOInterruptEventSource::interruptEventSource(
                this,
                OSMemberFunctionCast(IOInterruptEventAction, this, &VMDDriver::handleInterrupt0),provider,i);

          /*  controllerInt[i] = IOFilterInterruptEventSource::filterInterruptEventSource(this,
                                                                                             (IOInterruptEventAction) &VMDDriver::interruptHandler,
                                                                                             (IOFilterInterruptEventSource::Filter) &VMDDriver::interruptFilter,
                                                                                        provider, i);*/
            

            if (controllerInt[i]) {
                workLoop->addEventSource(controllerInt[i]);
                controllerInt[i]->enable();
            }
        }

        ctrl = pciDevice->configRead16(cap + 2);
        ctrl |= (1 << 15);
        
        pciDevice->configWrite16(cap + 2, ctrl);

        ctrl = pciDevice->configRead16(kIOPCIConfigCommand);
        ctrl |= kIOPCICommandInterruptDisable | kIOPCICommandBusMaster;
        pciDevice->configWrite16(kIOPCIConfigCommand, ctrl);
        pciDevice->setProperty("IOPCIMSIMode", kOSBooleanTrue);
        
    }
    
    
    
    OSArray *r1=decomposeAddressingProperties("reg",(OSData*)provider->getProperty("reg"));
    OSArray *r2=decomposeAddressingProperties("assigned-addresses",(OSData*)provider->getProperty("assigned-addresses"));
        
    OSDictionary *pr1=OSDictionary::withCapacity(2);
        pr1->setObject("regDEC2",r1);
        pr1->setObject("assigned-addressesDEC2",r2);
        provider->setProperty("DEC2",pr1);
        
        UInt8 *configBytes = (UInt8 *)IOMalloc(0x100);
        for (UInt16 offset = 0; offset < 0x100; offset++) {
            configBytes[offset] = pciDevice->configRead8(offset);
        }
        pciDevice->setProperty("cfgspace", configBytes, 0x100);
    pr1->release();
    r1->release();
    r2->release();
    
    
}



void VMDDriver::scanb( IOService * provider, int ii )
{
    
        IOPCIAddressSpace space;
        
        for (uint16_t bus = busStart2; bus <=busEnd2; bus++)
        {
            for (uint8_t dev = 0; dev < 32; dev++)
            {
                unsigned int busnr_ecam = bus - busStart;
                UInt32 DCFB0 = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(dev , 0), 0);
                struct _PCI_COMMON_HEADER *pc0=(struct _PCI_COMMON_HEADER*)(vmdConfigBase1+DCFB0);
                uint8_t functions = (pc0->HeaderType & PCI_HEADER_TYPE_MFD) ? 8 : 1;
                for (uint8_t fn = 0; fn < functions; fn++) {
                    
                    space.bits=0;
                    space.s.busNum = bus;
                    space.s.deviceNum=dev;
                    space.s.functionNum=fn;
                    
                    UInt32 DCFB = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(dev , fn), 0);
                    UInt32 DCFB2 = (busnr_ecam << 24)+ (dev<<16) + (fn);
                    UInt32 bacpi = (busnr_ecam << 24) + ((UInt32)PCI_DEVFN(dev , fn) << 16) + 0x8000FFFFU;
                    UInt32 vacpi;
                    if (DCFB>=0xe0000 && DCFB<0x100000) vacpi = (DCFB<<4) + 0x8000FFFFU;
                    if (DCFB>=0x100000) vacpi = (DCFB>>4)+ 0x80ffffU;
                    if (DCFB<0xe0000) vacpi = (DCFB<<4) + 0xFFF0U;
                    
                    bacpi &=0xFFFFFFFF;
                    vacpi &=0xFFFFFFFF;
                    
                    
                    struct _PCI_COMMON_HEADER *pc=(struct _PCI_COMMON_HEADER*)(vmdConfigBase1+DCFB);
                    UInt32 vendorID = pc->VendorID;
                    UInt32 deviceID = pc->DeviceID;
                    UInt32 classCode = ((static_cast<uint32_t>(pc->BaseClass) << 16) |
                                        (static_cast<uint32_t>(pc->SubClass) << 8) |
                                        static_cast<uint32_t>(pc->ProgIf));
                    UInt32 revID = pc->RevisionID;
                    UInt32 subvendorID = pc->u.type0.SubVendorID;
                    UInt32 subID = pc->u.type0.SubSystemID;
                    //if (pc->RevisionID==*(" ")) revID=0;
                    
                    if (vendorID == 0) continue;
                    if (vendorID == 0x0000FFFF) continue;
                    if (deviceID == 0) continue;
                    if (deviceID == 0x0000FFFF) continue;
                    if (classCode == 0) continue;
                    if (classCode == 0x00FFFFFF) continue;
                    
                    if (pc->Command==0) ++fNextID;
                    if (pc->Command==0)  continue;
                    
                    if (classCode == 0x010601) {
                        // continue;
                    }
                    
                    if (classCode == 0x010802) {
                         //classCode=0x0108ff;
                        //  vendorID =0x8086;
                        //  deviceID =0x2522;
                       //  continue;
                    }
                    
                    
                    char fullPath[128];
                    char nacpi[128];
                    char nloc[128];
                    char sdevn[128];
                    char sdebug[128];
                    char sacpi[128];
                    char compatBuf[128];
                    char *out;
                    char sbdf[128];
                    char sloc[128];
                    
                    snprintf(sdevn, sizeof("pcivvvv,pppp"), "pci%x,%x", vendorID, deviceID);
                    snprintf(sdebug, sizeof("bb:dd:ff"), "%u:%u:%u", bus,dev,fn);
                    snprintf(sbdf, sizeof("bb:dd:ff"), "%x:%x:%x", bus,dev,fn);
                    snprintf(sloc, sizeof("bb,dd,ff"), "%x,%x,%x", bus,dev,fn);
                    snprintf(sacpi, sizeof("pcivvvv,pppp@bb,dd,ff"), "pci%x,%x@%x,%x,%x", vendorID, deviceID,bus,dev,fn);
                    
                    out = compatBuf;
                    out += snprintf(out, sizeof("pcivvvv,pppp"), "pci%x,%x", vendorID, deviceID) + 1;
                    out += snprintf(out, sizeof("pciclass,cccccc"), "pciclass,%06x", classCode) + 1;
                    static uint8_t builtin[] = {0x00};
                    
                    snprintf(fullPath, sizeof(fullPath), "%x", bacpi);
                    snprintf(nloc, sizeof(nloc), "%s%d", "VDD", ii);
                    snprintf(nacpi, sizeof(nacpi), "%s@%s", nloc, fullPath);
                    //snprintf(sloc, sizeof(sloc), "%x",  vacpi);

                   // OSIterator * iter=provider->getProvider()->getProvider()->getChildIterator( gIODTPlane );
                    OSIterator * iter=pciDevice->getChildIterator( gIODTPlane );
                    IORegistryEntry *  child;
                    iter->reset();
                    OSNumber *nnacpi;
                    IOACPIPlatformDevice *acpiDevice;
                    bool ok=false;

                    while ((child = (IORegistryEntry *) iter->getNextObject())){
                        if (!child->getProperty("acpi-device")) continue;;
                        acpiDevice= (IOACPIPlatformDevice*)child->getProperty("acpi-device");
                        if (!acpiDevice->getProperty("_ADR")) continue;
                        nnacpi=(OSNumber*)acpiDevice->getProperty("_ADR");
                        if (nnacpi->unsigned32BitValue()==vacpi) {
                            //acpiDevice->setProperty("acpi-pm",true);
                            //acpiDevice->setPowerState(kIOPCIDeviceOnState, this);
                            ok=true;
                            snprintf(nloc, sizeof(nloc), "%s", child->getName());
                            snprintf(nacpi, sizeof(nacpi), "%s@%s", nloc, child->getLocation());
                            out += snprintf(out, sizeof(nloc), "%s", nloc) + 1;
                            break;
                        }
                    }
                    iter->release();
                    
                   childs[ii]= new VMDChildDevice;
                    
                    VMDChildDevice *t = childs[ii];
                    if (((pc->HeaderType & 0x7f) == 1)) t->isBridge=true;
                    
                    t->pvmd=this;
                    t->space=space;
                   // t->busStart=busStart;
                    t->lock=IOSimpleLockAlloc();
                    t->childLock=IORecursiveLockAlloc();
                    t->classCode=classCode;

                        t->acpiDevice=acpiDevice;
                        t->dtEntry=child;
                        //child->setProperty("vendor-id", OSData::withBytes( &vendorID, sizeof(UInt32)) );
                        t->init(child,gIODTPlane);
                        t->start(provider);

                    
                    initializeNub(t,t->getPropertyTable());
                    
                    /*
                    
                    t->setProperty("vendor-id", OSData::withBytes( &vendorID, sizeof(UInt32)) );
                    t->setProperty("device-id", OSData::withBytes( &deviceID, sizeof(UInt32)) );
                    t->setProperty("class-code", OSData::withBytes( &classCode, sizeof(UInt32)) );
                    t->setProperty("revision-id", OSData::withBytes( &revID, sizeof(UInt32) ) );
                    
                    
                    if ((pc->HeaderType & 0x7f) == 0) {
                        t->setProperty("subsystem-vendor-id", OSData::withBytes( &subvendorID, sizeof(UInt32)) );
                        t->setProperty("subsystem-id", OSData::withBytes( &subID, sizeof(UInt32)) );
                    }
                    
                    // t->setProperty("AAPL,slot-name", const_cast<char *>("built-in"), 9);
                    t->setProperty("built-in", builtin, arrsize(builtin));

                    
                  /*  t->setProperty("name", const_cast<char *>(sdevn), strlen(sdevn)+1);
                    t->setProperty("IOName",sdevn);
                    t->setProperty("compatible", OSData::withBytes( compatBuf, static_cast<unsigned int>(out - compatBuf) ) );
                    t->setProperty("pcidebug", sdebug);
                  //  t->setProperty("BDF", sbdf);
                    
                    
                    // t->setProperty("IOPCIConfigured", kOSBooleanTrue);
                    //  t->setProperty("IOPCIResourced", kOSBooleanTrue);
                    
                    
                    if ((pc->HeaderType & 0x7f) == 1){
                        
                        t->setProperty("name", const_cast<char *>("pci-bridge"), sizeof("pci-bridge"));
                        t->setProperty("IOName","pci-bridge");
                        
                        UInt32              lengthLo=3;
                        lengthLo = 3;
                        OSData *prop = OSData::withBytes(&lengthLo, sizeof(lengthLo));
                        if (prop)
                        {
                            t->setProperty("#address-cells", prop );
                            prop->release();
                        }
                        lengthLo = 2;
                        prop = OSData::withBytes(&lengthLo, sizeof(lengthLo));
                        if (prop)
                        {
                            t->setProperty("#size-cells", prop );
                            prop->release();
                        }
                    }
                    
                    */
                    
                   /* IODeviceMemory::InitElement rangeList[7] = {{0}};
                    UInt32 rangeCount = 0;
                    
                    uint16_t commandRegister;
                    commandRegister= t->configRead16( kIOPCIConfigurationOffsetCommand);
                    t->configWrite16( kIOPCIConfigurationOffsetCommand, commandRegister & ~( kIOPCICommandMemorySpace | kIOPCICommandBusLead));*/
                    
                    /* if (((pc->HeaderType & 0x7f) == 1)){
                     //pciDeviceB=t;
                     
                     IOPCIScalar  start;
                     IOPCIScalar  end;
                     UInt32 baselim32 = 0x0000FFF0; // closed range
                     
                     start = 0x50000000;
                     end   = 0x50000000 + 0x100000 - 1;
                     baselim32 = ((start >> 16) & 0xFFF0) | (end & 0xFFF00000);
                     t->configWrite32( kPCI2PCIMemoryRange, baselim32);
                     // t->setProperty("Model Number", const_cast<char *>("APPLE SSD AP0512R"), 18);
                     // t->setProperty("Firmware Revision", const_cast<char *>("387.100."), 9);
                     }
                     
                     if (classCode == 0x010802) {
                     t->configWrite32( kIOPCIConfigBaseAddress0, 0x50000000);
                     //pciDeviceC=t;
                     }*/
                    
                    
                   /* commandRegister = t->configRead16( kIOPCIConfigurationOffsetCommand);
                    t->configWrite16( kIOPCIConfigurationOffsetCommand, commandRegister | ( kIOPCICommandMemorySpace | kIOPCICommandBusLead));*/
                    

                    t->setName( sdevn);
                   // ii++;
                    publishNub(t,ii++);
                    t->setLocation( sloc );
                   
                    //t->removeProperty("IOPCIResourced");
                    // t->getResources();
                     //t->relocate(false);
                    
                    //saveDeviceState(t);
                    
                    

                    //resolveLegacyInterrupts2(provider->getProvider(),t,1);
                    
                     uint8_t cap;
                     if (0/*(pc->HeaderType & 0x7f) == 0*/)
                     {
                         if (0/*t->findPCICapability(kIOPCICapabilityIDMSIX,&cap)*/) {
                            // t->reset(kIOPCIDeviceResetTypeHotReset);
                             
                             UInt16 ctrl;
                             ctrl = t->configRead16( cap + 2);
                             UInt32 numVectors0 = (ctrl & 0x7FF) + 1;

                             t->interruptController = new VMDInterruptController;
                             t->interruptController->pvmd = this;
                             t->interruptController->numVectors=numVectors0;
                             if (!t->interruptController || !t->interruptController->init(t)) {
                                 OSSafeReleaseNULL(t->interruptController);
                                 return ;
                             }
                             t->interruptController->vmdVector=this->nextVector;
                             t->interruptController->_nextVector=this->nextVector;
                             t->interruptController->ismsix=true;
                             
                             t->vmdVector=this->nextVector;
                             t->numVectors=numVectors0;
                             
                            /* t->configureInterrupts(
                                                    kIOInterruptTypePCIMessagedX,
                                                    numVectors0,
                                                    numVectors0
                                                    );*/
                             
                             t->controllerInt=IONew(IOInterruptEventSource*, numVectors0);
                             
                             /*OSArray * controllers = OSArray::withCapacity(numVectors0);
                              OSArray * specifiers  = OSArray::withCapacity(numVectors0);
                              t->_interruptSources=IONew(IOInterruptSource, numVectors0);
                              t->_numInterruptSources=numVectors0;
                              char loc[128];
                              snprintf(loc, sizeof(nloc), "%s%d", "VMDInterruptController",ii);
                              OSSymbol *name= (OSSymbol*)OSSymbol::withCString(loc);
                              getPlatform()->registerInterruptController(name,t->interruptController);*/
                             
                             for (int i=0;i<numVectors0;i++){
                                 
                                 t->interruptController->programChildMSIX(t,i,t->vmdVector+i);
                                 
                                 UInt32 table  = t->configRead32(cap + 4);
                                 UInt8 bar    = (kIOPCIConfigBaseAddress0 + ((table & 7) << 2));
                                 table &= ~7;
                                 
                                 IOMemoryMap *map  = t->mapDeviceMemoryWithRegister(bar);
                                 
                                 volatile UInt32* entry = (volatile UInt32*)(map->getAddress() + table+i*16);
                                 
                                 UInt32 msiData = entry[2];
                                 UInt32 msiAddress = entry[0];;
                                 UInt32 msiAddressHi = entry[1];
                                 UInt32 msienab = entry[3];
                                 
                                 t->setProperty("msiData", msiData,32);
                                 t->setProperty("msiAddress", msiAddress,32);
                                 t->setProperty("msiAddressHi", msiAddressHi,32);
                                 t->setProperty("msienab", msienab,32);
                                 map->release();
                                 
                                 //  controllers->setObject(OSString::withCString( "IOPCIMessagedInterruptController"));
                                 /*  controllers->setObject(OSString::withCString( loc));
                                  
                                  UInt32            controllerIndex=msiData;
                                  UInt32            interruptFlags=kIOInterruptTypeEdge | kIOInterruptTypePCIMessaged|kIOInterruptTypePCIMessagedX;
                                  OSData* specData = OSData::withCapacity(2 * sizeof(UInt32));
                                  specData->appendBytes(&controllerIndex, sizeof(controllerIndex));
                                  specData->appendBytes(&interruptFlags,  sizeof(interruptFlags));
                                  
                                  specifiers->setObject(specData);
                                  
                                  t->_interruptSources[i].interruptController=t->interruptController;
                                  t->_interruptSources[i].vectorData=specData;*/
                                 

                                 
                                 /* t->controllerInt[i] = IOFilterInterruptEventSource::filterInterruptEventSource(t,
                                  (IOInterruptEventAction) &VMDChildDevice::interruptHandler,
                                  (IOFilterInterruptEventSource::Filter) &VMDChildDevice::interruptFilter,
                                  t, msiData);
                                  t->workLoop->addEventSource(controllerInt[i]);
                                  t->controllerInt[i]->enable();*/
                                 
                             }
                             // t->setProperty(gIOInterruptControllersKey, controllers);
                             // t->setProperty(gIOInterruptSpecifiersKey,  specifiers);
                             this->nextVector-=numVectors0;

                         }
                         else
                             if (t->findPCICapability(kIOPCICapabilityIDMSI,&cap)) {
                                 
                                 
                                 
                                 
                                 
                                 UInt16 ctrl;
                                 
                                 ctrl = t->configRead16( cap + 2);
                                 UInt32 numVectors0 = 1 << (0x7 & (ctrl >> 1));

                                 /*t->configureInterrupts(
                                                        kIOInterruptTypePCIMessaged,
                                                        numVectors0,
                                                        numVectors0
                                                        );*/
                                 /*
                                 
                                 t->interruptController = new VMDInterruptController;
                                 t->interruptController->pvmd = this;
                                 t->interruptController->numVectors=numVectors0;
                                 if (!t->interruptController || !t->interruptController->init(t)) {
                                     OSSafeReleaseNULL(t->interruptController);
                                     return ;
                                 }
                                 t->interruptController->vmdVector=this->nextVector;
                                 t->interruptController->_nextVector=this->nextVector;
                                 t->interruptController->ismsix=false;
                                 
                                 t->vmdVector=this->nextVector;
                                 t->numVectors=numVectors0;
                                 */
                                 
                                /* IOMemoryMap* map = nullptr;
                                 volatile UInt32* entry = t->interruptController->getChildMSIXEntry(t, 0, &map);
                                 UInt32 msiAddress=entry[0];
                                 map->release();
                                 
                                 uint16_t  control=t->configRead16( cap + 2);
                                 
                                 bool is64bit = control & 0x0080;

                                 UInt32 msiAddressHi = 0;//_msixTable[vmdVector * 4 + 1];
                                 UInt32 msiData = this->nextVector;//_msixTable[vmdVector * 4 + 2];
                                 
                                 t->configWrite32(cap + PCI_MSI_ADDRESS_LO, msiAddress);
                                 if (is64bit) {
                                     t->configWrite32(cap + PCI_MSI_ADDRESS_HI, msiAddressHi);
                                 }
                                 
                                 t->configWrite16(cap + (is64bit ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32), msiData);
                                 

                                 
                                 t->configureInterrupts(
                                                        kIOInterruptTypePCIMessaged,
                                                        1,
                                                        1
                                                        );*/
                                 
                                 
                                 
                                 t->controllerInt=IONew(IOInterruptEventSource*, numVectors0);
                                 
                                 /* t->_interruptSources=IONew(IOInterruptSource, numVectors0);
                                  t->_numInterruptSources=numVectors0;
                                  OSArray * controllers = OSArray::withCapacity(numVectors0);
                                  OSArray * specifiers  = OSArray::withCapacity(numVectors0);
                                  char loc[128];
                                  snprintf(loc, sizeof(nloc), "%s%d", "VMDInterruptController",ii);
                                  OSSymbol *name= (OSSymbol*)OSSymbol::withCString(loc);
                                  getPlatform()->registerInterruptController(name,t->interruptController);*/
                                 
                                 
                                // t->interruptController->programChildMSI(t,0,t->vmdVector);
                                 
                                 for (int i=0;i<1;i++){
                                     
                                     
                                    /* if (i==0) controllers->setObject(OSString::withCString( "io-apic-0"));
                                     else *///controllers->setObject(OSString::withCString( "IOPCIMessagedInterruptController"));
                                     // controllers->setObject(OSString::withCString( loc));
                                      
                                    /*  UInt32            controllerIndex=msiData;
                                      UInt32            interruptFlags=kIOInterruptTypeEdge | kIOInterruptTypePCIMessaged;
                                    /* if (i==0){
                                         controllerIndex=0x10;
                                         interruptFlags=7;
                                     }
                                      OSData* specData = OSData::withCapacity(2 * sizeof(UInt32));
                                      specData->appendBytes(&controllerIndex, sizeof(controllerIndex));
                                      specData->appendBytes(&interruptFlags,  sizeof(interruptFlags));
                                      
                                      specifiers->setObject(specData);
                                      //t->_interruptSources[i].interruptController=t->interruptController;;
                                      t->_interruptSources[i].vectorData=specData;*/
                                     

                                       /*  t->controllerInt[i] = IOFilterInterruptEventSource::filterInterruptEventSource(t,
                                                                                                                        (IOInterruptEventAction) &VMDChildDevice::interruptHandler,
                                                                                                                        (IOFilterInterruptEventSource::Filter) &VMDChildDevice::interruptFilter,
                                                                                                                        t, i);
                                         t->workLoop->addEventSource(t->controllerInt[i]);
                                         t->controllerInt[i]->enable();*/

                                     
                                 }
                                //  t->setProperty(gIOInterruptControllersKey, controllers);
                                //  t->setProperty(gIOInterruptSpecifiersKey,  specifiers);
                                 this->nextVector-=1;//numVectors0;

                             }
                         
                     }
                    
                    
                     
                    
                    
                    if (0)
                    {
                        IOACPIPlatformDevice *acpiDevice =     (IOACPIPlatformDevice*)provider;
                        OSObject **r1,**r2;
                        UInt8 uuidBytes[16] = {
                                0xdd, 0xcd, 0xd9, 0x9c, // Time low (little-endian)
                                0x45, 0x88,             // Time mid (little-endian)
                                0xfd, 0x4a,             // Time high (little-endian)
                                0x83, 0x92,             // Clock sequence (big-endian)
                                0x31, 0xc4, 0xeb, 0x87, 0xca, 0xbd // Node (big-endian)
                            };
                            OSData *uuidData = OSData::withBytes(uuidBytes, sizeof(uuidBytes));
                        OSArray *pdevices = OSArray::withCapacity(1);
                       // pdevices->setObject(OSNumber::withNumber(nnacpi->unsigned32BitValue(), 32));

                        
                            OSObject *dsmArgsPowerOff[4] = {
                                uuidData,                           // Arg0: UUID
                                OSNumber::withNumber(1, 32),     // Arg1: Revision (0)
                                OSNumber::withNumber(3, 32),  // Arg2: Function Index 3 (VLOff)
                                pdevices                      // Arg3: Package of 32 integers
                            };
                        acpiDevice->evaluateObject("_DSM", r1, dsmArgsPowerOff, 4);
                        
                        OSObject * dsmArgsPowerOn[4] = {
                            uuidData,                           // Arg0: UUID
                            OSNumber::withNumber(1, 32),     // Arg1: Revision (0)
                            OSNumber::withNumber(2, 32),  // Arg2: Function Index 2 (VLON)
                            pdevices                     // Arg3: Package of 32 integers
                        };
                        acpiDevice->evaluateObject("_DSM", r2, dsmArgsPowerOn, 4);
                        pdevices->release();
                        t->setProperty("r1", r1);
                        t->setProperty("r2", r2);
                        
                    }

                }
            }
        }
    
    
   
    //changePowerStateTo(kIOPCIDeviceOnState);
    /*
    for (int i=0;i<12;i++){
        
        if (childs[i]) {

            //childs[i]->changePowerStateTo(kIOPCIDeviceOnState);
            //childs[i]->registerService();
            UInt8 *configBytes = (UInt8 *)IOMalloc(0x100);
             for (UInt16 offset = 0; offset < 0x100; offset++) {
                 configBytes[offset] = childs[i]->configRead8(offset);
             }
            childs[i]->setProperty("cfgspace", configBytes, 0x100);
        }
                
    }
    */
    
    
    
}
/*
IOReturn VMDChildDevice::getResources( void ){
    
   
    return IOPCIDevice::getResources();
}
*/



bool IOPCI2PCIBridge::start(IOService *provider)
{
    
    vmdb=  (VMDDriver*)this;
    vmdb->configure1(provider);
    
    IOPCIConfigShadow * shadow;
    IOPCIConfigSave   * saved;

    shadow = configShadow(fBridgeDevice);
    saved  = &shadow->configSave;
    
    for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
    {
        saved->savedConfig[i]=fBridgeDevice->configRead32( i * 4);
    }
    
    /*fBridgeDevice->enablePCIPowerManagement(kPCIPMCPMESupportFromD3Cold|kPCIPMCPMESupportFromD3Hot|kPCIPMCPMESupportFromD0);
    fBridgeDevice->setProperty(kIOPCIExpressASPMDefaultKey,2,32);
    fBridgeDevice->reserved->expressASPMDefault = 2;
    setDeviceASPMState(fBridgeDevice, this, fBridgeDevice->reserved->expressASPMDefault);
    */
    
    
    if (1) {
        IOACPIPlatformDevice *acpiDevice =     (IOACPIPlatformDevice*)provider;
        
        UInt8 uuidBytes[16] = {
            0xdd, 0xcd, 0xd9, 0x9c, // Time low (little-endian)
            0x45, 0x88,             // Time mid (little-endian)
            0xfd, 0x4a,             // Time high (little-endian)
            0x83, 0x92,             // Clock sequence (big-endian)
            0x31, 0xc4, 0xeb, 0x87, 0xca, 0xbd // Node (big-endian)
        };
        OSData *uuidData = OSData::withBytes(uuidBytes, sizeof(uuidBytes));
        
        OSArray *d3Params = OSArray::withCapacity(1);
        int i=1;
        d3Params->setObject( OSNumber::withNumber(i, 32));
        
        //VD3A is a capability flag, not a state register
        // Don't set it to 0 unless you want to permanently disable D3 management
        //  Leave it at 1 for normal power management
        
        OSObject *dsmArgsD3[4] = {
            uuidData,                           // Arg0: UUID
            OSNumber::withNumber(1, 32),     // Arg1: Revision (0)
            OSNumber::withNumber(5, 32),  // Arg2: Function Index 5 (VD3A)
            d3Params                            // Arg3: VD3A value
        };
        
        acpiDevice->evaluateObject("_DSM", nullptr, dsmArgsD3, 4);
        d3Params->release();
    }
    
    //IOPCIHostBridge *base=(IOPCIHostBridge*)IORegistryEntry::fromPath("IOService:/AppleACPIPlatformExpert/PC00@0/AppleACPIPCI");
    
    IOPCIConfigEntry * bridge= fBridgeDevice->reserved->configEntry;
        IOPCIRange *       range;
        uint64_t           start, size;
    

        bridge->vendorProduct = 0x9a0b8086;
        
    //bridge->id           =fBridgeDevice->reserved->configEntry->id;
      //  bridge->id           = 0x10000;
        bridge->classCode    = 0x060000;
        bridge->headerType   = kPCIHeaderType1;
        bridge->secBusNum    = firstBusNum();
        bridge->subBusNum    = lastBusNum();
        bridge->subDeviceNum = 0;
        bridge->endDeviceNum = 31;
        
        bridge->space        = getBridgeSpace();
    
    
   /* bridge->dtNub           = provider;
    bridge->dtEntry         = bridge->dtNub;
        bridge->acpiDevice      = (IOACPIPlatformDevice*)provider;
        bridge->hostBridge      = fBridgeDevice->reserved->configEntry->hostBridge;
        bridge->hostBridgeEntry = fBridgeDevice->reserved->configEntry->hostBridgeEntry;*/
        bridge->isHostBridge    = true;
        bridge->isBridge        = true;
        bridge->supportsHotPlug = kPCIStatic;
        
    bridge->hostBridge      = (IOPCIHostBridge*)vmdb;
    bridge->hostBridgeEntry =bridge;
    bridge->rootPortEntry = bridge;
       bridge->expressEndpointMaxReadRequestSize = -1;
   
        bridge->expressCapBlock=0;

        bridge->dtEntry->setProperty(kIOPCIIgnoreLinkStatusKey,true);
        bridge->dtEntry->setProperty(kIOPCIExpressASPMDefaultKey,2,32);
    //fBridgeDevice->setProperty(kIOPCILinkChangeKey,true);
    //fBridgeDevice->setProperty(kIOPCIHotPlugKey,true);
    //configShadow(fBridgeDevice)->hpType=kPCIStatic;
        
        range = new IOPCIRange;
        start = bridge->secBusNum;
        size  = bridge->subBusNum - bridge->secBusNum + 1;
        bzero(range, sizeof(*range));
        range->type         = kIOPCIResourceTypeBusNumber;
        range->start        = start;
        range->proposedSize = size;
        range->totalSize    = size;
        range->end          = start;
        range->alignment    = kPCIBridgeBusNumberAlignment;
        range->maxAddress   = 0xFFFFFFFF;
        range->allocations  = (IOPCIRange *) &range->end;
        range->size = range->proposedSize;
        range->end  = range->start + range->size;
        
        bridge->ranges[kIOPCIRangeBridgeBusNumber] = range;
    
    
    
    
    
    bool ok, hasHostMemory, hasHost64;
    for (int type = hasHostMemory = hasHost64 = 0; type < kIOPCIResourceTypeCount; type++)
    {
        IOPCIRange * thisRange;
        for (thisRange = IOPCIBridge::reserved->rangeLists[type];
             thisRange;
             thisRange = thisRange->next)
        {
            start = thisRange->start;
            size  = thisRange->size;

           // ok = IOPCIRangeListAddRange(&bridge->ranges[BRN(type)],type, start, size);

            range = new IOPCIRange;
            bzero(range, sizeof(*range));
            range->type         = type;
            range->start        = start;
            range->proposedSize = size;
            range->totalSize    = size;
            range->end          = start;
            range->alignment    = kPCIBridgeMemoryAlignment;
            range->maxAddress   = 0xFFFFFFFF;
            range->allocations  = (IOPCIRange *) &range->end;
            range->size = range->proposedSize;
            range->end  = range->start + range->size;
            
            bridge->ranges[BRN(type)] = range;

          //  if (!ok) continue;
            if ((kIOPCIResourceTypeMemory == type) || (kIOPCIResourceTypePrefetchMemory == type))
            {
                hasHostMemory = true;
                hasHost64 |= (0 != (start >> 32));
            }
        }
    }
    
    
    
    
    OSArray *            array;
    IOMemoryDescriptor * mem;
    array = fBridgeDevice->getDeviceMemory();
    uint32_t             idx, type;
    IOPCIAddressSpace   space0;
    for ( idx = 0; idx<6; idx++)  bridge->ranges[idx] = nullptr;
    
    for ( idx = 0; (mem = (IOMemoryDescriptor *) array->getObject(idx)); idx++)
    {
        type  = mem->getTag();
        IOPCIScalar start = mem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
        IOPCIScalar size  = mem->getLength();
        space0.bits=type;
        type=space0.s.registerNum-kIOPCIConfigBaseAddress0;
        
        range = new IOPCIRange;
        bzero(range, sizeof(*range));
        range->type         = kIOPCIResourceTypeMemory;
        range->start        = start;
        range->proposedSize = size;
        range->totalSize    = size;
        range->end          = start;
        range->alignment    = kPCIBridgeMemoryAlignment;
        range->maxAddress   = 0xFFFFFFFF;
        range->allocations  = (IOPCIRange *) &range->end;
        range->size = range->proposedSize;
        range->end  = range->start + range->size;
        
        bridge->ranges[type] = range;
    }
    
    //gIOPCIFlags |=kIOPCIConfiguratorAER|kIOPCIConfiguratorIOLog;
    
   // if ( (CPUID_FEATURE_VMM & cpuid_features()))
    //    gIOPCIFlags &= ~kIOPCIConfiguratorPFM64;
    
    /*
    vmdb->bridge=bridge;
    vmdb->busStart2=0xe0;
    vmdb->busEnd2=0xe0;
    vmdb->scanb(provider,0);
    
    
    vmdb->bridge=bridge;
    vmdb->busStart2=0xe1;
    vmdb->busEnd2=0xe1;
    vmdb->scanb(provider,2);
    */
    
   // detach(provider);
    
   
    IOPCI2PCIBridge::start(provider);
    fBridgeDevice->setPowerState(kIOPCIDeviceOnState, this);
    //changePowerStateTo(kIOPCIDeviceOnState);
    
    configOpParams cp = {.device = fBridgeDevice, .op = kConfigOpScan, .result = nullptr};
    configOp(&cp);
    
    
   
    //fBridgeDevice->findPCICapability( kIOPCIPCIExpressCapability, (UInt8*)&bridge->expressCapBlock);
    
    IOPCIConfigEntry * child = NULL;
    
    for (child = bridge->child; child; child = child->peer)
    {
        VMDChildDevice* bnub=(VMDChildDevice*)child->dtNub;
        if (bnub->configRead16(kIOPCIConfigCommand)==0) continue;
        
        UInt32 busnr_ecam=child->space.s.busNum-0xe0;
         UInt32 DCFB = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(child->space.s.deviceNum , child->space.s.functionNum), 0);
         UInt32 DCFB2 = (busnr_ecam << 24)+ (child->space.s.deviceNum<<16) + (child->space.s.functionNum);
         UInt32 vacpi;
         if (DCFB>=0xe0000 && DCFB<0x100000) vacpi = (DCFB<<4) + 0x8000FFFFU;
         if (DCFB>=0x100000) vacpi = (DCFB>>4)+ 0x80ffffU;
         if (DCFB<0xe0000) vacpi = (DCFB<<4) + 0xFFF0U;
         vacpi &=0xFFFFFFFF;
         

         //OSIterator * iter=fBridgeDevice->getProvider()->getProvider()->getChildIterator( gIODTPlane );
         OSIterator * iter=fBridgeDevice->getChildIterator( gIODTPlane );
         IORegistryEntry *  child0;
         iter->reset();
         OSNumber *nnacpi;
         IOACPIPlatformDevice *acpiDevice;
         bool ok=false;

         while ((child0 = (IORegistryEntry *) iter->getNextObject())){
             if (!child0->getProperty("acpi-device")) continue;;
             acpiDevice= (IOACPIPlatformDevice*)child0->getProperty("acpi-device");
             if (!acpiDevice->getProperty("_ADR")) continue;
             nnacpi=(OSNumber*)acpiDevice->getProperty("_ADR");
             if (nnacpi->unsigned32BitValue()==vacpi) {
                 ok=true;
                 break;
             }
         }
         iter->release();

        if (ok){
             child->acpiDevice=acpiDevice;
             child->dtNub->setProperty("acpi-device",child0->getProperty("acpi-device"));
             child->dtNub->setProperty("acpi-path",child0->getProperty("acpi-path"));
         }
        
           
        //vmdb->vacpi(bnub->space,false);
        //vmdb->vacpi(bnub->space,true);
        //bnub->setPowerState(kIOPCIDeviceOnState, this);
        if (child->isBridge) {
            //bnub->setProperty(kIOPCILinkChangeKey,true);
            //bnub->setProperty(kIOPCIHotPlugKey,true);
            //configShadow(bnub)->hpType=kPCIStatic;
            
            bnub->reset(kIOPCIDeviceResetTypeHotReset);
            IOSleep(100);
            
            VMDChildDevice *bnub2=  (VMDChildDevice*)bnub->getChildEntry(gIODTPlane);
            IOPCIConfigEntry * child2 = bnub2->reserved->configEntry;
            
            UInt32 busnr_ecam=child2->space.s.busNum-0xe0;
             UInt32 DCFB = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(child2->space.s.deviceNum , child2->space.s.functionNum), 0);
             UInt32 DCFB2 = (busnr_ecam << 24)+ (child2->space.s.deviceNum<<16) + (child2->space.s.functionNum);
             UInt32 vacpi;
             if (DCFB>=0xe0000 && DCFB<0x100000) vacpi = (DCFB<<4) + 0x8000FFFFU;
             if (DCFB>=0x100000) vacpi = (DCFB>>4)+ 0x80ffffU;
             if (DCFB<0xe0000) vacpi = (DCFB<<4) + 0xFFF0U;
             vacpi &=0xFFFFFFFF;
             
             //OSIterator * iter=fBridgeDevice->getProvider()->getProvider()->getChildIterator( gIODTPlane );
             OSIterator * iter=fBridgeDevice->getChildIterator( gIODTPlane );
             IORegistryEntry *  child0;
             iter->reset();
             OSNumber *nnacpi;
             IOACPIPlatformDevice *acpiDevice;
             bool ok=false;

             while ((child0 = (IORegistryEntry *) iter->getNextObject())){
                 if (!child0->getProperty("acpi-device")) continue;;
                 acpiDevice= (IOACPIPlatformDevice*)child0->getProperty("acpi-device");
                 if (!acpiDevice->getProperty("_ADR")) continue;
                 nnacpi=(OSNumber*)acpiDevice->getProperty("_ADR");
                 if (nnacpi->unsigned32BitValue()==vacpi) {
                     ok=true;
                     break;
                 }
             }
             iter->release();

            
            /*if (ok){
                 child2->acpiDevice=acpiDevice;
                 child2->dtNub->setProperty("acpi-device",child0->getProperty("acpi-device"));
                 child2->dtNub->setProperty("acpi-path",child0->getProperty("acpi-path"));
             }*/
            
            //vmdb->vacpi(bnub2->space,false);
            //vmdb->vacpi(bnub2->space,true);
            //bnub2->setPowerState(kIOPCIDeviceOnState, bnub);
        }
    }
    
    
    for (child = bridge->child; child; child = child->peer)
    {
        VMDChildDevice* bnub=(VMDChildDevice*)child->dtNub;
        
        if (bnub->configRead16(kIOPCIConfigCommand)==0) continue;
        
        
       UInt8 expressCapability;
       /* if( bnub->findPCICapability(kIOPCIPCIExpressCapability, &expressCapability))
        {
            
            if (expressCapability) {
                uint16_t ltr = VMD_BIOS_PM_QUIRK_LTR;
                uint32_t ltr_reg;
                uint32_t pos;
                
                // pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);
                pos = bnub->configRead16(expressCapability + PCI_EXT_CAP_ID_LTR);
                if (pos){
                    ltr_reg=bnub->configRead32( pos + PCI_LTR_MAX_SNOOP_LAT);
                    if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK))){}
                    else{
                        ltr_reg = (ltr << 16) | ltr;
                        bnub->configWrite32( pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg);
                    }
                }
                
            }
        }*/
        

        
        UInt8 Cap=0;
        bnub->findPCICapability(kIOPCICapabilityIDMSI, &Cap);
        if (Cap && !child->isBridge)
        {
            
            if (!bnub->getProperty("IOPCIMSIMode"))
                bnub->configureInterrupts(
                    kIOInterruptTypePCIMessaged,
                    1,
                    1
                );
            
            //static uint8_t builtin[] = {0x00};
            //bnub->setProperty("built-in", builtin, arrsize(builtin));
            
           
            OSArray *        liveSpecs;
            liveSpecs = OSDynamicCast(OSArray,bnub->getPropertyTable()->getObject(gIOInterruptSpecifiersKey));
            OSData *         spec;
            spec = OSDynamicCast(OSData, liveSpecs->getObject(0));
            uint32_t vmdVector = *((uint32_t *) spec->getBytesNoCopy());
            
            
            IOInterruptVector *bnubVector= bnub->reserved->msiVectors;
            
            IOService *nub=bnubVector->nub;
            int source=bnubVector->source;
            void *target=bnubVector->target;
            IOInterruptHandler handler=bnubVector->handler;
            void *refCon=bnubVector->refCon;
            
            fBridgeDevice->registerInterrupt(vmdVector, (OSObject *)nub, (IOInterruptAction)handler,refCon);
            //fBridgeDevice->enableInterrupt(vmdVector);
            
            
           /* int i=vmdVector;
                
            vmdb->controllerInt[i] = IOInterruptEventSource::interruptEventSource(vmdb,
                                                                                   OSMemberFunctionCast(IOInterruptEventSource::Action,
                                                                                                        vmdb,
                                                                                                        &VMDDriver::handleInterrupt0),
                                                                                   provider,
                                                                                                           vmdVector);
            if (vmdb->controllerInt[i]) {
                vmdb->workLoop->addEventSource(vmdb->controllerInt[i]);
                    vmdb->controllerInt[i]->enable();
                }*/
                
            
            IOPCIConfigShadow * shadow;
            IOPCIConfigSave   * saved;

            shadow = configShadow(fBridgeDevice);
            saved=&shadow->configSave;
            
                
               /* uint16_t  control=bnub->configRead16( Cap + 2);
            control &= ~1;
            bnub->configWrite16(Cap + 2, control);
            control=bnub->configRead16( Cap + 2);
            
                bool is64bit = control & 0x0080;
                
                 UInt32 msiAddress = saved->savedMSIAddress0;
                 UInt32 msiAddressHi = saved->savedMSIAddress1;
                 UInt32 msiData = vmdVector;
                 
                 bnub->configWrite32(Cap + PCI_MSI_ADDRESS_LO, msiAddress);
                 if (is64bit) {
                 bnub->configWrite32(Cap + PCI_MSI_ADDRESS_HI, msiAddressHi);
                 }
                 
                 bnub->configWrite16(Cap + (is64bit ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32), msiData);
                 
                control |= 1;
                bnub->configWrite16(Cap + 2, control);
                
                control = bnub->configRead16(kIOPCIConfigCommand);
                control |= kIOPCICommandInterruptDisable | kIOPCICommandBusMaster;
                bnub->configWrite16(kIOPCIConfigCommand, control);*/
                //bnub->setProperty("IOPCIMSIMode", kOSBooleanTrue);
            
            IOPCIConfigShadow * shadow2;
            IOPCIConfigSave   * saved2;
            
            shadow2 = configShadow(bnub);
            saved2  = &shadow2->configSave;
            
           /* saved2->savedMSIAddress0=saved->savedMSIAddress0;
            saved2->savedMSIAddress1=saved->savedMSIAddress1;
            saved2->savedMSIData=vmdVector;*/
            
            for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
            {
                saved2->savedConfig[i]=bnub->configRead32( i * 4);
            }
            //saveDeviceState(bnub);
        }
        
        
             
        if (child->isBridge)
        {
           /* bnub->enablePCIPowerManagement(kPCIPMCPMESupportFromD3Cold|kPCIPMCPMESupportFromD3Hot|kPCIPMCPMESupportFromD0);
            bnub->setProperty(kIOPCIExpressASPMDefaultKey,2,32);
            bnub->reserved->expressASPMDefault = 2;
            setDeviceASPMState(bnub, this, bnub->reserved->expressASPMDefault);*/
            
            OSArray *r1=vmdb->decomposeAddressingProperties("reg",(OSData*)bnub->getProperty("reg"));
            OSArray *r2=vmdb->decomposeAddressingProperties("ranges",(OSData*)bnub->getProperty("ranges"));
            
            OSDictionary* pr1=OSDictionary::withCapacity(2);
            pr1->setObject("regDEC2",r1);
            pr1->setObject("rangesDEC2",r2);
            bnub->setProperty("DEC2",pr1);
            pr1->release();
            r1->release();
            r2->release();
            
            IOPCIConfigShadow * shadow2;
            IOPCIConfigSave   * saved2;
            
            shadow2 = configShadow(bnub);
            saved2  = &shadow2->configSave;
            
            for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
            {
                saved2->savedConfig[i]=bnub->configRead32( i * 4);
            }
            //saveDeviceState(bnub);
            
            VMDChildDevice *bnub2=  (VMDChildDevice*)bnub->getChildEntry(gIODTPlane);
            IOPCIConfigEntry * child2 = bnub2->reserved->configEntry;
            
            if (bnub2)
            {
                
                //bnub2->removeProperty("IOReportLegend");
                //bnub2->removeProperty(gIOInterruptSpecifiersKey);
                //bnub2->removeProperty(gIOInterruptControllersKey);
                
                bnub2->configureInterrupts(
                    kIOInterruptTypePCIMessaged,
                    1,
                    1
                );

                UInt8 Cap=0;
                bnub2->findPCICapability(kIOPCICapabilityIDMSI, &Cap);
                
                
                UInt8 expressCapability;
                
               /* if( bnub2->findPCICapability(kIOPCIPCIExpressCapability, &expressCapability))
                {
                    
                    if (expressCapability) {
                        uint16_t ltr = VMD_BIOS_PM_QUIRK_LTR;
                        uint32_t ltr_reg;
                        uint32_t pos;
                        
                        // pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);
                        pos = bnub2->configRead16(expressCapability + PCI_EXT_CAP_ID_LTR);
                        if (pos){
                            ltr_reg=bnub2->configRead32( pos + PCI_LTR_MAX_SNOOP_LAT);
                            if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK))){}
                            else{
                                ltr_reg = (ltr << 16) | ltr;
                                bnub2->configWrite32( pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg);
                            }
                        }
                        
                    }
                }*/
                
                //static uint8_t builtin[] = {0x00};
                //bnub2->setProperty("built-in", builtin, arrsize(builtin));
                
                /*bnub2->enablePCIPowerManagement(kPCIPMCPMESupportFromD3Cold|kPCIPMCPMESupportFromD3Hot|kPCIPMCPMESupportFromD0);
                bnub2->setProperty(kIOPCIExpressASPMDefaultKey,2,32);
                bnub2->reserved->expressASPMDefault = 2;
                setDeviceASPMState(bnub2, this, bnub2->reserved->expressASPMDefault);*/
                
                OSArray *        liveSpecs;
                liveSpecs = OSDynamicCast(OSArray,bnub2->getPropertyTable()->getObject(gIOInterruptSpecifiersKey));
                OSData *         spec;
                spec = OSDynamicCast(OSData, liveSpecs->getObject(0));
                uint32_t vmdVector = *((uint32_t *) spec->getBytesNoCopy());
                
                
                IOInterruptVector *bnubVector= bnub2->reserved->msiVectors;
                
                IOService *nub=bnubVector->nub;
                int source=bnubVector->source;
                void *target=bnubVector->target;
                IOInterruptHandler handler=bnubVector->handler;
                void *refCon=bnubVector->refCon;
                
                fBridgeDevice->registerInterrupt(vmdVector, (OSObject *)nub, (IOInterruptAction)handler,refCon);
                //fBridgeDevice->enableInterrupt(vmdVector);

                
               /* int i=vmdVector;
                vmdb->controllerInt[i] = IOInterruptEventSource::interruptEventSource(vmdb,
                                                                                       OSMemberFunctionCast(IOInterruptEventSource::Action,
                                                                                                            vmdb,
                                                                                                            &VMDDriver::handleInterrupt0),
                                                                                       provider,
                                                                                                               vmdVector);
                    
                if (vmdb->controllerInt[i]) {
                    vmdb->workLoop->addEventSource(vmdb->controllerInt[i]);
                        vmdb->controllerInt[i]->enable();
                    }*/

                   /* IOInterruptEventSource * fBridgeInterruptSource= IOInterruptEventSource::interruptEventSource(this,
                                                                                          OSMemberFunctionCast(IOInterruptEventSource::Action,
                                                                                                               this,
                                                                                                               &VMDDriver::handleInterrupt0),
                                                                                          provider,
                                                                                                                  vmdVector);
                    getWorkLoop()->addEventSource(fBridgeInterruptSource);
                    fBridgeInterruptSource->enable();*/
                
                
                IOPCIConfigShadow * shadow;
                IOPCIConfigSave   * saved;

                shadow = configShadow(fBridgeDevice);
                saved=&shadow->configSave;
                
                    
                /*    uint16_t  control=bnub2->configRead16( Cap + 2);
                control &= ~1;
                bnub2->configWrite16(Cap + 2, control);
                control=bnub2->configRead16( Cap + 2);
                
                    bool is64bit = control & 0x0080;
                    
                     UInt32 msiAddress = saved->savedMSIAddress0;
                     UInt32 msiAddressHi = saved->savedMSIAddress1;
                     UInt32 msiData = vmdVector;
                     
                     bnub2->configWrite32(Cap + PCI_MSI_ADDRESS_LO, msiAddress);
                     if (is64bit) {
                     bnub2->configWrite32(Cap + PCI_MSI_ADDRESS_HI, msiAddressHi);
                     }
                     
                     bnub2->configWrite16(Cap + (is64bit ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32), msiData);
                     
                    control |= 1;
                    bnub2->configWrite16(Cap + 2, control);
                
                    control = bnub2->configRead16(kIOPCIConfigCommand);
                    control |= kIOPCICommandInterruptDisable | kIOPCICommandBusMaster;
                    bnub2->configWrite16(kIOPCIConfigCommand, control);*/
                    //bnub2->setProperty("IOPCIMSIMode", kOSBooleanTrue);
                
                IOPCIConfigShadow * shadow2;
                 IOPCIConfigSave   * saved2;
                 
                 shadow2 = configShadow(bnub2);
                 saved2  = &shadow2->configSave;
                 
                /* saved2->savedMSIAddress0=saved->savedMSIAddress0;
                 saved2->savedMSIAddress1=saved->savedMSIAddress1;
                 saved2->savedMSIData=vmdVector;*/
                
                
                for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
                {
                    saved2->savedConfig[i]=bnub2->configRead32( i * 4);
                }
                
                OSArray *r1=vmdb->decomposeAddressingProperties("reg",(OSData*)bnub2->getProperty("reg"));
                OSArray *r2=vmdb->decomposeAddressingProperties("assigned-addresses",(OSData*)bnub2->getProperty("assigned-addresses"));
                
                OSDictionary* pr1=OSDictionary::withCapacity(2);
                pr1->setObject("regDEC2",r1);
                pr1->setObject("assigned-addressesDEC2",r2);
                bnub2->setProperty("DEC2",pr1);
                pr1->release();
                r1->release();
                r2->release();
                
                
                
                //saveDeviceState(bnub2);
                
                UInt8 *configBytes = (UInt8 *)IOMalloc(0x100);
                for (UInt16 offset = 0; offset < 0x100; offset++) {
                    configBytes[offset] = bnub2->configRead8(offset);
                }
                bnub2->setProperty("cfgspace", configBytes, 0x100);
                
                bnub2->setProperty("savedConfig", bnub2->savedConfig,4* kIOPCIConfigShadowRegs);
                
               

            }
            
        }
        else
        {

            OSArray *r1=vmdb->decomposeAddressingProperties("reg",(OSData*)bnub->getProperty("reg"));
            OSArray *r2=vmdb->decomposeAddressingProperties("assigned-addresses",(OSData*)bnub->getProperty("assigned-addresses"));
            
            OSDictionary* pr1=OSDictionary::withCapacity(2);
            pr1->setObject("regDEC2",r1);
            pr1->setObject("assigned-addressesDEC2",r2);
            bnub->setProperty("DEC2",pr1);
            pr1->release();
            r1->release();
            r2->release();
            
        }
        
        UInt8 *configBytes = (UInt8 *)IOMalloc(0x100);
        for (UInt16 offset = 0; offset < 0x100; offset++) {
            configBytes[offset] = bnub->configRead8(offset);
        }
        bnub->setProperty("cfgspace", configBytes, 0x100);
        
        //bnub->setProperty("savedConfig", bnub->savedConfig,4* kIOPCIConfigShadowRegs);
        
    }
    
    //fBridgeDevice->reset(kIOPCIDeviceResetTypeHotReset);
    //allocateBridgeInterrupts(provider);
    //startBridgeInterrupts(provider);
    
/*
    int i=0;

    vmdb->controllerInt[i] =  IOInterruptEventSource::interruptEventSource(vmdb,
                                                                            OSMemberFunctionCast(IOInterruptEventSource::Action,
                                                                                                 vmdb,
                                                                                                 &VMDDriver::handleInterrupt0),
                                                                            provider,
                                                                                                    0);

    if (vmdb->controllerInt[i]) {
        vmdb->workLoop->addEventSource(vmdb->controllerInt[i]);
        vmdb->controllerInt[i]->enable();
    }
    */
    

    //allocateBridgeInterrupts(provider);
    //startBridgeInterrupts(provider);
    
    
   /* for (child = bridge->child; child; child = child->peer)
    {
        VMDChildDevice* bnub=(VMDChildDevice*)child->dtNub;
        if (bnub->configRead16(kIOPCIConfigCommand)==0) continue;
       // restoreDeviceState(bnub);
        bnub->setPowerState(kIOPCIDeviceOnState, this);
        if (child->isBridge) {
            VMDChildDevice *bnub2=  (VMDChildDevice*)bnub->getChildEntry(gIODTPlane);
            //restoreDeviceState(bnub2);
            bnub2->setPowerState(kIOPCIDeviceOnState, bnub);
            //((IOACPIPlatformDevice*)child2->acpiDevice)->evaluateObject("_PR0");
        }
    }
    */
    
    
    
    
    registerService();
    return true;
}

/*
bool VMDDriver::start(IOService *provider) {
    
    
    const char *  n=provider->getName();
    if (!strcmp(n,"VMD0")) {
        configure1(provider);
    }
   // else configure3(provider);
    
    registerService();
    return true;

}
*/

void VMDDriver::vacpi(IOPCIAddressSpace space, bool on) {

    UInt32 busnr_ecam=space.s.busNum-0xe0;
     UInt32 DCFB = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), 0);
     UInt32 DCFB2 = (busnr_ecam << 24)+ (space.s.deviceNum<<16) + (space.s.functionNum);
     UInt32 vacpi;
     if (DCFB>=0xe0000 && DCFB<0x100000) vacpi = (DCFB<<4) + 0x8000FFFFU;
     if (DCFB>=0x100000) vacpi = (DCFB>>4)+ 0x80ffffU;
     if (DCFB<0xe0000) vacpi = (DCFB<<4) + 0xFFF0U;
     vacpi &=0xFFFFFFFF;
     


     //OSIterator * iter=fBridgeDevice->getProvider()->getProvider()->getChildIterator( gIODTPlane );
     OSIterator * iter=pciDevice->getChildIterator( gIODTPlane );
     IORegistryEntry *  child0;
     iter->reset();
     OSNumber *nnacpi;
     IOACPIPlatformDevice *acpiDevice;
     bool ok=false;

     while ((child0 = (IORegistryEntry *) iter->getNextObject())){
         if (!child0->getProperty("acpi-device")) continue;;
         acpiDevice= (IOACPIPlatformDevice*)child0->getProperty("acpi-device");
         if (!acpiDevice->getProperty("_ADR")) continue;
         nnacpi=(OSNumber*)acpiDevice->getProperty("_ADR");
         if (nnacpi->unsigned32BitValue()==vacpi) {
             ok=true;
             break;
         }
     }
     iter->release();
    
     UInt8 uuidBytes[16] = {
     0xdd, 0xcd, 0xd9, 0x9c, // Time low (little-endian)
     0x45, 0x88,             // Time mid (little-endian)
     0xfd, 0x4a,             // Time high (little-endian)
     0x83, 0x92,             // Clock sequence (big-endian)
     0x31, 0xc4, 0xeb, 0x87, 0xca, 0xbd // Node (big-endian)
     };
     OSData *uuidData = OSData::withBytes(uuidBytes, sizeof(uuidBytes));
     OSArray *pdevices = OSArray::withCapacity(1);
     pdevices->setObject(OSNumber::withNumber(nnacpi->unsigned32BitValue(), 32));
     
     
     OSObject *dsmArgsPowerOff[4] = {
     uuidData,                           // Arg0: UUID
     OSNumber::withNumber(1, 32),     // Arg1: Revision (0)
     OSNumber::withNumber(on ? 2:3, 32),  // Arg2: Function Index 3 (VLOff)
     pdevices                      // Arg3: Package of 32 integers
     };
     acpiDevice->evaluateObject("_DSM", nullptr, dsmArgsPowerOff, 4);
     
     /*OSObject * dsmArgsPowerOn[4] = {
     uuidData,                           // Arg0: UUID
     OSNumber::withNumber(1, 32),     // Arg1: Revision (0)
     OSNumber::withNumber(2, 32),  // Arg2: Function Index 2 (VLON)
     pdevices                     // Arg3: Package of 32 integers
     };
     acpiDevice->evaluateObject("_DSM", nullptr, dsmArgsPowerOn, 4);*/
     pdevices->release();

    
}

void VMDDriver::free() {
    IOService::free();
}

#pragma mark - Helper Methods


UInt8 VMDDriver::firstBusNum( void )
{
    return busStart;
}
UInt8 VMDDriver::lastBusNum( void )
{
    return busEnd;
}

UInt32 VMDDriver::findPCICapability(IOPCIAddressSpace space, UInt32 capabilityID, UInt8 * found )
{
    uint32_t data;
    uint32_t offset, expressCap, firstOffset;

    data = expressCap = firstOffset = 0;
    if (found)
    {
        firstOffset = *found;
        *found = 0;
    }

    if (0 == ((kIOPCIStatusCapabilities << 16)
              & (configRead32(space, kIOPCIConfigCommand))))
        return (0);

    if (capabilityID >= 0x100)
    {
        if (!findPCICapability( space,kIOPCIPCIExpressCapability, (UInt8*)&expressCap)) return (0);

        capabilityID = -capabilityID;
        offset = 0x100;
        while (offset)
        {
            data = configRead32(space, offset);
            if (capabilityID == (data & 0xffff))
            {
                if (!firstOffset)
                {
                    if (found)
                        *found = offset;
                    break;
                }
                if (offset == firstOffset) firstOffset = 0;
            }
            offset = (data >> 20) & 0xfff;
            
            if (space.s.busNum>=0xe0)
            if ((offset > 0x100) || (offset & 3))
                offset = 0;
            
            if (space.s.busNum<0xe0)
            if ((offset < 0x100) || (offset & 3))
                offset = 0;
        }
    }
    else
    {
        offset = (0xff & configRead32(space, kIOPCIConfigCapabilitiesPtr));
        if (offset & 3)
            offset = 0;
        while (offset)
        {
            data = configRead32(space, offset);
            if (capabilityID == (data & 0xff))
            {
                if (!firstOffset)
                {
                    if (found)
                        *found = offset;
                    break;
                }
                if (offset == firstOffset) firstOffset = 0;
            }
            offset = (data >> 8) & 0xff;
            if (offset & 3)
                offset = 0;
            
            if (space.s.busNum>=0xe0)
            if (offset>0xff) offset = 0;
        }
    }

    return (offset ? data : 0);
    
}



/*
IODeviceMemory * VMDDriver::ioDeviceMemory( void )
{
    return (pciDevice->getDeviceMemoryWithIndex(1));
}
*/
/*

IODeviceMemory * VMDChildDevice::ioDeviceMemory( void )
{
    return (parent->ioDeviceMemory());
}*/

void VMDChildDevice::stop(IOService *provider) {
    IOService::stop(provider);
}



void VMDChildDevice::configurePCISpace(const char *propName, IOService *pciDevice,
                       IOPCIAddressSpace addrSpace,
                       UInt64 address,
                       UInt64 length)
{
    bool isExtended = addrSpace.s.t;
    UInt8 spaceType = addrSpace.s.space;
    UInt32 regOffset;


    if(isExtended) {
        //addrSpace.es.busNum = pciDevice->getBusNumber();
        //addrSpace.es.deviceNum = pciDevice->getDeviceNumber();
        //addrSpace.es.functionNum = pciDevice->getFunctionNumber();
        addrSpace.es.registerNumExtended=(uint32_t)address;
        regOffset = addrSpace.es.registerNum;
    } else {
        //addrSpace.s.busNum = pciDevice->getBusNumber();
        //addrSpace.s.deviceNum = pciDevice->getDeviceNumber();
        //addrSpace.s.functionNum = pciDevice->getFunctionNumber();
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
                
                switch(spaceType) {
                    case kIOPCI32BitMemorySpace:
                        addressLo |= 0x0;
                        break;
                    case kIOPCI64BitMemorySpace:
                        addressLo |= 0x4;
                        if(addrSpace.s.prefetch) {
                            addressLo |= 0x8;
                        }
                        break;
                    default:
                        break;
                }
                
                
              //  OSSwapHostToLittleInt32
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
    configEntry[0] = OSSwapHostToLittleInt32(addrSpace.bits);
    
    if(spaceType == kIOPCIIOSpace && address==0) {
        configEntry[1] = 0;
        configEntry[2] = 0;
    } else {
        configEntry[1] = OSSwapHostToLittleInt32(static_cast<UInt32>(address >> 32));
        configEntry[2] = OSSwapHostToLittleInt32(static_cast<UInt32>(address & 0xFFFFFFFF));
    }
    configEntry[3] = OSSwapHostToLittleInt32(static_cast<UInt32>(length >> 32));
    configEntry[4] = OSSwapHostToLittleInt32(static_cast<UInt32>(length & 0xFFFFFFFF));

    OSData *existing = OSDynamicCast(OSData, pciDevice->getProperty(propName));
    OSData *updated = existing ? OSData::withData(existing) : OSData::withCapacity(0);
    
    if(updated) {
        updated->appendBytes(configEntry, sizeof(configEntry));
        pciDevice->setProperty(propName, updated);
        updated->release();
    }

}


IOWorkLoop *VMDChildDevice::getWorkLoop() const {

    return workLoop;
}
/*
IOReturn VMDChildDevice::getResources( void )
{

    
    return IOPCIDevice::getResources();
}*/





bool VMDChildDevice::start(IOService *provider) {
    if (!workLoop) {
        bridge=pvmd->bridge;
        this->retain();
        workLoop = IOWorkLoop::workLoop();
        workLoop->retain();
        
        PMinit();
        //IOPCIRegisterPowerDriver(this, false);
        provider->joinPMtree(this);
        
        static uint8_t builtin[] = {0x00};
        setProperty("built-in", builtin, arrsize(builtin));
        
        IOPCIConfigEntry *child= new IOPCIConfigEntry;
        reserved->configEntry= child;
        reserved->hostBridge=bridge->hostBridge;
        parent=bridge->hostBridge;
        
        child->deviceState = kPCIDeviceStateAttached
        | kPCIDeviceStateRangesProbed|kPCIDeviceStateAllocatedBus|kPCIDeviceStateAllocated|kPCIDeviceStateTotalled|kPCIDeviceStateConfigurationDone;
        
        //child->id              = ++pvmd->fNextID;
        child->space           = space;
        child->hostBridge      = bridge->hostBridge;
        child->hostBridgeEntry = bridge->hostBridgeEntry;
        child->headerType      = configRead8( kIOPCIConfigHeaderType) & 0x7f;
        child->classCode       = configRead32( kIOPCIConfigRevisionID) >> 8;
        child->vendorProduct   = configRead32( kIOPCIConfigVendorID);
        
        child->acpiDevice=acpiDevice;
        child->dtNub           = this;
        child->dtEntry         = dtEntry;
        
        bool ok;
        if (bridge->isHostBridge)
        {
            child->rootPortEntry = child;
        }
        else
        {
            child->rootPortEntry = bridge->rootPortEntry;
        }
        switch (child->headerType)
        {
            case kPCIHeaderType0:
                break;

            case kPCIHeaderType1:
            case kPCIHeaderType2:
                child->isBridge     = true;
                child->subDeviceNum = 0;
                child->endDeviceNum = 31;
                break;

            default:
                ok = false;
                break;
        }
        
        uint16_t commandRegister;
        commandRegister= configRead16( kIOPCIConfigurationOffsetCommand);
        configWrite16( kIOPCIConfigurationOffsetCommand, commandRegister & ~( kIOPCICommandIOSpace| kIOPCICommandMemorySpace | kIOPCICommandBusLead));
        
        

        if (isBridge)
        {
            //reset(kIOPCIDeviceResetTypeHotReset);
            UInt16 bridgeControl = configRead16(kPCI2PCIBridgeControl);
            configWrite16(kPCI2PCIBridgeControl, bridgeControl | (1 << 6));
            IOSleep(10);
            configWrite16(kPCI2PCIBridgeControl, bridgeControl);
            
            configWrite32( kPCI2PCIPrimaryBus, 0xe0);
            configWrite32( kPCI2PCIUpperIORange, 0);
            configWrite16( kPCI2PCIIORange, 0x00f0);
            configWrite32( kPCI2PCIPrefetchUpperLimit,  0);
            configWrite32( kPCI2PCIPrefetchMemoryRange, 0x0000FFF0);
            configWrite32( kPCI2PCIPrefetchUpperBase,  0);
            
            configWrite32( kPCI2PCIMemoryRange, 0x50105010);
        }
        
        if (classCode == 0x010601) {
            configWrite32( kIOPCIConfigBaseAddress0, configRead32( kIOPCIConfigBaseAddress5));
            configWrite32( kIOPCIConfigBaseAddress1, configRead32( kIOPCIConfigBaseAddress0)+0x2800);
            configWrite32( kIOPCIConfigBaseAddress5, configRead32( kIOPCIConfigBaseAddress0)+0x2000);
        }
        
        commandRegister = configRead16( kIOPCIConfigurationOffsetCommand);
        configWrite16( kIOPCIConfigurationOffsetCommand, commandRegister | ( kIOPCICommandIOSpace|kIOPCICommandMemorySpace | kIOPCICommandBusLead));
        
        /*IOPCIConfigShadow * shadow;
        IOPCIConfigSave   * saved;

        shadow = configShadow(this);
        saved  = &shadow->configSave;
        
        for (int i = 0; i < kIOPCIConfigEPShadowRegs; i++)
        {
            saved->savedConfig[i]=configRead32( i * 4);
        }*/
        
       // if (kPCIDeviceStateRangesProbed & child->deviceState) {
            IODeviceMemory::InitElement rangeList[7] = {{0}};
            UInt32 rangeCount = 0;
            rangeCount =decmem(rangeList, child->isBridge, child);
            if (!child->isBridge) setmem(child->hostBridge);
       //}
        
        if (child->isBridge) enablePCIPowerManagement(kPCIPMCPMESupportFromD3Cold|kPCIPMCPMESupportFromD3Hot|kPCIPMCPMESupportFromD0);
        setProperty(kIOPCIExpressASPMDefaultKey,2,32);
        reserved->expressASPMDefault = 2;
        //child->hostBridge->setDeviceASPMState(this, child->hostBridge, reserved->expressASPMDefault);
        
      /*  UInt8 expressCapability;
        
        if( findPCICapability(kIOPCIPCIExpressCapability, &expressCapability))
        {
            
            if (expressCapability) {
                uint16_t ltr = VMD_BIOS_PM_QUIRK_LTR;
                uint32_t ltr_reg;
                uint32_t pos;
                
                // pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);
                pos = configRead16(expressCapability + PCI_EXT_CAP_ID_LTR);
                if (pos){
                    ltr_reg=configRead32( pos + PCI_LTR_MAX_SNOOP_LAT);
                    if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK))){}
                    else{
                        ltr_reg = (ltr << 16) | ltr;
                        configWrite32( pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg);
                    }
                }
                
            }
        }

        if (findPCICapability( kIOPCIPCIExpressCapability, (UInt8 *)&child->expressCapBlock))
        {
            child->linkCaps = configRead32( child->expressCapBlock + 0x0c);
            if (child->isBridge)
            {
                uint32_t expressCaps, linkControl, slotCaps = kSlotCapHotplug;
            
                expressCaps = configRead16( child->expressCapBlock + 0x02);
                linkControl = configRead16( child->expressCapBlock + 0x10);

                if (0x100 & expressCaps)
                {
                    slotCaps = configRead32( child->expressCapBlock + 0x14);
                    child->commandCompleted = (kSlotCapNoCommandCompleted & slotCaps) == 0;
                    child->powerController = (kSlotCapPowerController & slotCaps) != 0;
                }

                if ((0x60 == (0xf0 & expressCaps))      // downstream port
                     || (0x40 == (0xf0 & expressCaps))) // or root port
                {
                    if ((kLinkCapDataLinkLayerActiveReportingCapable & child->linkCaps)
                     && (kSlotCapHotplug & slotCaps))
                    {
                        child->linkInterrupts = true;
                    }
                }
        
                child->expressCaps        = expressCaps;


            }
            child->expressDeviceCaps1 = configRead32( child->expressCapBlock + 0x04);
            child->expressDeviceCaps2 = configRead32( child->expressCapBlock + 0x24);
            child->expressMaxPayload  = (child->expressDeviceCaps1 & 7);
        }

       // if (kIOPCIConfiguratorAER & gIOPCIFlags)
       // {
            findPCICapability( kIOPCIExpressErrorReportingCapability, (UInt8 *)&child->aerCapBlock);
       // }

       // bridgeAddChild(bridge, child);
        IOPCIConfigEntry ** prev = &bridge->child;
        IOPCIConfigEntry *  next;

        // tailq
        while ((next = *prev))
        {
            prev = &next->peer;
        }
        *prev = child;
        child->parent = bridge;

        //bridge->deviceState |= kPCIDeviceStateChildChanged | kPCIDeviceStateChildAdded;
        
        //checkCacheLineSize(child);
        if (kPCIStatic == (kPCIHPTypeMask & child->parent->supportsHotPlug)){}
        else
        {
            uint8_t cacheLineSize, cls, was;
            cacheLineSize = 0x40;
            cls = configRead8( kIOPCIConfigCacheLineSize);
            was = cls;
            
            if ((cls >= cacheLineSize) && ((cls % cacheLineSize) == 0)){}
            else
            {
                configWrite8( kIOPCIConfigCacheLineSize, cacheLineSize);
                cls = configRead8( kIOPCIConfigCacheLineSize);
                if (cls != cacheLineSize)
                {
                    configWrite8( kIOPCIConfigCacheLineSize, 0);
                }
            }
        }*/
        attachToParent(provider, gIODTPlane);
        
       /* if (child->isBridge){
            OSArray *r1=pvmd->decomposeAddressingProperties("reg",(OSData*)getProperty("reg"));
            OSArray *r2=pvmd->decomposeAddressingProperties("ranges",(OSData*)getProperty("ranges"));
            
            OSDictionary* pr1=OSDictionary::withCapacity(2);
            pr1->setObject("regDEC2",r1);
            pr1->setObject("rangesDEC2",r2);
            setProperty("DEC2",pr1);
            pr1->release();
            r1->release();
            r2->release();
            
        }
        else
        {
            
            OSArray *r1=pvmd->decomposeAddressingProperties("reg",(OSData*)getProperty("reg"));
            OSArray *r2=pvmd->decomposeAddressingProperties("assigned-addresses",(OSData*)getProperty("assigned-addresses"));
            
            OSDictionary* pr1=OSDictionary::withCapacity(2);
            pr1->setObject("regDEC2",r1);
            pr1->setObject("assigned-addressesDEC2",r2);
            setProperty("DEC2",pr1);
            pr1->release();
            r1->release();
            r2->release();
        }
            
            
            UInt8 *configBytes = (UInt8 *)IOMalloc(0x100);
            for (UInt16 offset = 0; offset < 0x100; offset++) {
                configBytes[offset] = configRead8(offset);
            }
            setProperty("cfgspace", configBytes, 0x100);
            
            setProperty("savedConfig", savedConfig,4* kIOPCIConfigShadowRegs);*/
        
    }
       // command_gate = IOCommandGate::commandGate(this);
      //  workLoop->addEventSource(command_gate);
    //registerService();
    return true;
}


void VMDChildDevice::free() {

    IOService::free();
}

void VMDChildDevice::constructAddressingProperties(IOPCIConfigEntry * device, OSDictionary * propTable)
{
    IOPCIRange *         range;
    IOPCIPhysicalAddress regData;
    OSString *           string;
    OSData *             prop;
    OSData *             regProp;
    OSData *             assignedProp;
    OSData *             rangeProp;
    char                 tuple[64];
    size_t               len;

   /* len = snprintf(tuple, sizeof(tuple), "%u:%u:%u", PCI_ADDRESS_TUPLE(device));
    if (device->isBridge && (len < sizeof(tuple)))
    {
        snprintf(&tuple[len], sizeof(tuple) - len, "(%u:%u)", device->secBusNum, device->subBusNum);
    }
    string = OSString::withCString(tuple);
    if (string)
    {
        propTable->setObject("pcidebug", string);
        string->release();
    }*/

    assignedProp = OSData::withCapacity(32);
    regProp      = OSData::withCapacity(32);
    rangeProp    = OSData::withCapacity(32);
    if (!assignedProp || !regProp || !rangeProp)
        return;

    regData.physHi  = device->space;
    regData.physMid = 0;
    regData.physLo  = 0;
    regData.lengthHi  = 0;
    regData.lengthLo  = 0;
    regProp->appendBytes(&regData, sizeof(regData));

    for (uint32_t i = 0; i < kIOPCIRangeCount; i++)
    {
        static const uint8_t barRegisters[kIOPCIRangeExpansionROM + 1] = {
            kIOPCIConfigBaseAddress0, kIOPCIConfigBaseAddress1, kIOPCIConfigBaseAddress2,
            kIOPCIConfigBaseAddress3, kIOPCIConfigBaseAddress4, kIOPCIConfigBaseAddress5,
            kIOPCIConfigExpansionROMBase
        };

        range = device->ranges[i];
        if (!range)
            continue;
        if (kIOPCIResourceTypeBusNumber == range->type)
            continue;

        regData.physHi = device->space;
        switch (range->type)
        {
          case kIOPCIResourceTypeMemory:
            regData.physHi.s.space    = kIOPCI32BitMemorySpace;
            break;
          case kIOPCIResourceTypePrefetchMemory:
            regData.physHi.s.space    = kIOPCI32BitMemorySpace;
            regData.physHi.s.prefetch = 1;
            break;

          case kIOPCIResourceTypeIO:
            regData.physHi.s.space    = kIOPCIIOSpace;
            break;
        }

        regData.physMid  = 0;
        regData.physLo   = 0;
        regData.lengthHi = (range->size >> 32ULL);
        regData.lengthLo = static_cast<UInt32>(range->size);
        if (i <= kIOPCIRangeExpansionROM)
        {
            regData.physHi.s.registerNum = barRegisters[i];
            if (range->start)
            {
                IOPCIPhysicalAddress assignedData;
                assignedData = regData;
                assignedData.physHi.s.reloc = 1;
                assignedData.physMid = (range->start >> 32ULL);
                assignedData.physLo  = static_cast<UInt32>(range->start);
                assignedProp->appendBytes(&assignedData, sizeof(assignedData));
            }
            // reg gets requested length
            regData.lengthHi             = (range->proposedSize >> 32ULL);
            regData.lengthLo             = static_cast<UInt32>(range->proposedSize);
            regProp->appendBytes(&regData, sizeof(regData));
        }
        else
        {
            regData.physHi.s.reloc       = 1;
            regData.physMid              = (range->start >> 32ULL);
            regData.physLo               = static_cast<UInt32>(range->start);
            regData.physHi.s.busNum      = 0;
            regData.physHi.s.deviceNum   = 0;
            regData.physHi.s.functionNum = 0;
            regData.physHi.s.registerNum = 0;
            rangeProp->appendBytes(&regData, sizeof(regData.physHi) + sizeof(regData.physMid) + sizeof(regData.physLo));
            rangeProp->appendBytes(&regData, sizeof(regData));
        }
    }
    propTable->setObject("reg", regProp);
    regProp->release();
    if (assignedProp->getLength())
        propTable->setObject("assigned-addresses", assignedProp);
    assignedProp->release();
    if (rangeProp->getLength())
    {
        propTable->setObject("ranges", rangeProp);

        /*regData.lengthLo = 3;
        prop = OSData::withBytes(&regData.lengthLo, sizeof(regData.lengthLo));
        if (prop)
        {
            propTable->setObject("#address-cells", prop );
            prop->release();
        }
        regData.lengthLo = 2;
        prop = OSData::withBytes(&regData.lengthLo, sizeof(regData.lengthLo));
        if (prop)
        {
            propTable->setObject("#size-cells", prop );
            prop->release();
        }*/
    }
    rangeProp->release();
}

UInt32 VMDChildDevice::decmem(IODeviceMemory::InitElement *rangeList, bool isb,  IOPCIConfigEntry * bridge)
{
    uint32_t        barNum, nextBarNum;
    uint32_t        value;
    uint64_t        value64, saved, upper, barMask;
    uint64_t        start, size;
    uint8_t        type;
    uint8_t         barOffset;
    bool            clean64;
    uint32_t lastBarNum=PCI_TYPE0_ADDRESSES-1;
    uint32_t resetMask=0;
    
    IOPCIAddressSpace space=this->space;
    uint8_t bus=space.s.busNum;
    uint8_t dev=space.s.deviceNum;
    uint8_t fn=space.s.functionNum;
    
    UInt32 rangeCount=0;
    space.s.registerNum=0;
    space.s.space=kIOPCIConfigSpace;
    configurePCISpace("reg", this,space,0,0);
    
    if (isb) lastBarNum=PCI_TYPE1_ADDRESSES-1;
    
    if (!isb)
    {
        
        for (barNum = 0; barNum <= lastBarNum; barNum = nextBarNum)
        {
            barOffset  = kIOPCIConfigBaseAddress0 + barNum * 4;
            nextBarNum = barNum + 1;
            value64    = (-1ULL << 32);
            
            space.bits=0;
            space.s.busNum=bus;
            space.s.deviceNum=dev;
            space.s.functionNum=fn;
            space.s.registerNum=barOffset;
            space.s.space=kIOPCI32BitMemorySpace;
            space.s.reloc=1;
            space.s.prefetch=0;
            
            saved = configRead32( barOffset);
            configWrite32( barOffset, 0xFFFFFFFF);
            value = configRead32( barOffset);
            configWrite32( barOffset, static_cast<uint32_t>(saved));
            
            if (value == 0) continue;
            
            clean64 = false;
            if (value & 1)
            {
                barMask = 0x3;
                type = kIOPCIResourceTypeIO;
                space.s.space=kIOPCIIOSpace;
                if ((value & 0xFFFF0000) == 0) value |= 0xFFFF0000;
            }
            else
            {
                barMask = 0xf;
                if (value & 0x8){
                    type = kIOPCIResourceTypePrefetchMemory;
                    space.s.prefetch=1;
                }
                else{
                    type = kIOPCIResourceTypeMemory;
                }
                
                switch (value & 6)
                {
                    case 2:
                    case 0:
                    case 6:
                        type = kIOPCIResourceTypeMemory;
                        break;
                        
                    case 4:
                        
                        clean64 = ((kIOPCIResourceTypePrefetchMemory == type) || (0 == space.s.busNum));
                        if (!clean64) configWrite32(space, barOffset + 4, 0);
                        else
                        {
                            space.s.space=kIOPCI64BitMemorySpace;
                            upper = configRead32( barOffset + 4);
                            saved |= (upper << 32);
                            configWrite32( barOffset + 4, 0xFFFFFFFF);
                            value64 = configRead32( barOffset + 4);
                            value64 <<= 32;
                            configWrite32( barOffset + 4, static_cast<uint32_t>(upper));
                        }
                        nextBarNum = barNum + 2;
                        break;
                }
            }
            
            start    = (resetMask & (1 << type)) ? 0 : (saved & ~barMask);
            value   &= ~barMask;
            value64 |= value;
            size     = -value64;
            
            if (size > MAX_BAR_SIZE)
            {
                size &= 0xFFFFFFFF;
                clean64 = false;
                nextBarNum--;
            }
            if (start == value64)
            {
                continue;
            }
            if (start==0) space.s.reloc=0;
            rangeList[rangeCount] =  {
                .start = start,
                .length = size,
                .tag = space.bits
            };
            
            IOPCIRange *       range;
            range = new IOPCIRange;
            bzero(range, sizeof(*range));
            range->type         = type;
            range->start        = start;
            range->proposedSize = size;
            range->totalSize    = size;
            range->end          = start;
            range->alignment    = size;
            range->maxAddress   = 0xFFFFFFFF;
            range->allocations  = (IOPCIRange *) &range->end;
            range->size = range->proposedSize;
            range->end  = range->start + range->size;
            
            if (clean64)
            {
                range->flags |= kIOPCIRangeFlagBar64;
            }
            bridge->ranges[barNum] = range;
            rangeCount++;
            
             if (!isb) {
             configurePCISpace("reg", this,space,0,size);
             configurePCISpace("assigned-addresses", this,space,start,size);
             }
             if (isb) {
             configurePCISpace("ranges", this,space,start,size);
             }
            
            
            
             /*if (!isb) {
             OSArray *r1=pvmd->decomposeAddressingProperties("reg",(OSData*)t->getProperty("reg"));
             OSDictionary *pr1=OSDictionary::withCapacity(2);
             pr1->setObject("regDEC2",r1);
             if (rangeCount){
             OSArray *r2=pvmd->decomposeAddressingProperties("assigned-addresses",(OSData*)t->getProperty("assigned-addresses"));
             pr1->setObject("assigned-addressesDEC2",r2);
             }
             t->setProperty("DEC2",pr1);
             }*/
            
        }
    }

    if (isb)
    {
        
        space.bits=0;
        space.s.reloc=1;
        space.s.busNum=0;
        space.s.deviceNum=0;
        space.s.functionNum=0;
        space.s.registerNum=0;
        space.s.prefetch=0;
        
        uint64_t start, end, size;
        
        uint16_t memBase = configRead16( kIOPCIConfigMemoryBase);
        uint16_t memLimit = configRead16( kIOPCIConfigMemoryLimit);
        
        start = (memBase & 0xFFF0) << 16;
        end   = ((memLimit & 0xFFF0) << 16) | 0xFFFFF;
        
        if (memBase <= memLimit && start < end)
        {
            size = end - start + 1;
            space.s.space=kIOPCI32BitMemorySpace;
            
            rangeList[rangeCount++] =  {
                .start = start,
                .length = size,
                .tag = space.bits
            };
            IOPCIRange *range = new IOPCIRange;
            bzero(range, sizeof(*range));
            range->type         = kIOPCIResourceTypeMemory;
            range->start        = start;
            range->proposedSize = size;
            range->totalSize    = size;
            range->end          = start;
            range->alignment    = kPCIBridgeMemoryAlignment;
            range->maxAddress   = 0xFFFFFFFF;
            range->allocations  = (IOPCIRange *) &range->end;
            range->size = range->proposedSize;
            range->end  = range->start + range->size;
            
            bridge->ranges[kIOPCIRangeBridgeMemory] = range;
           /* ranges[kIOPCIRangeBridgeMemory] = IOPCIRangeAlloc();
            IOPCIRange *range = ranges[kIOPCIRangeBridgeMemory];
            IOPCIRangeInitAlloc(range, kIOPCIResourceTypeMemory, start, size, kPCIBridgeMemoryAlignment);*/
           configurePCISpace("ranges", this,space,start,size);
        }
        
        space.s.space=kIOPCI32BitMemorySpace;
        space.s.prefetch=1;
        uint32_t prefetchBase = configRead32( kIOPCIConfigPrefetchableMemoryBase);
        uint32_t prefetchLimit = configRead32( kIOPCIConfigPrefetchableMemoryLimit);
        
        bool is64Bit = (prefetchBase & 0x01);
        start = (prefetchBase & 0xFFF0) << 16;
        end   = ((prefetchLimit & 0xFFF0) << 16) | 0xFFFFF;
        
        if (is64Bit)
        {
            uint32_t upperBase = configRead32( kIOPCIConfigPrefetchableBaseUpper);
            uint32_t upperLimit = configRead32( kIOPCIConfigPrefetchableLimitUpper);
            
            start |= (uint64_t)upperBase << 32;
            end   |= (uint64_t)upperLimit << 32;
        }
        
        if ((prefetchBase & 0x0F) <= (prefetchLimit & 0x0F) && start < end)
        {
            size = end - start + 1;
            rangeList[rangeCount++] =  {
                .start = start,
                .length = size,
                .tag = space.bits
            };
            IOPCIRange *range = new IOPCIRange;
            bzero(range, sizeof(*range));
            range->type         = kIOPCIResourceTypePrefetchMemory;
            range->start        = start;
            range->proposedSize = size;
            range->totalSize    = size;
            range->end          = start;
            range->alignment    = kPCIBridgeMemoryAlignment;
            range->maxAddress   = 0xFFFFFFFF;
            range->allocations  = (IOPCIRange *) &range->end;
            range->size = range->proposedSize;
            range->end  = range->start + range->size;
            
            bridge->ranges[kIOPCIRangeBridgePFMemory] = range;
            /*ranges[kIOPCIRangeBridgePFMemory] = IOPCIRangeAlloc();
            IOPCIRange *range = ranges[kIOPCIRangeBridgePFMemory];
            IOPCIRangeInitAlloc(range, kIOPCIResourceTypePrefetchMemory, start, size, kPCIBridgeMemoryAlignment);*/
            configurePCISpace("ranges", this,space,start,size);
        }
        
        space.s.space=kIOPCIIOSpace;
        space.s.prefetch=0;
        uint8_t ioBase = configRead8( kIOPCIConfigIOBase);
        uint8_t ioLimit = configRead8( kIOPCIConfigIOLimit);
        bool is32BitIO = (configRead16( kIOPCIConfigBridgeControl) & 0x01);
        
        start = (ioBase & 0xF0) << 8;
        end   = (ioLimit & 0xF0) << 8 | 0xFFF;
        
        if (is32BitIO)
        {
            space.s.prefetch=1;
            uint16_t ioUpperBase = configRead16( kIOPCIConfigIOBaseUpper);
            uint16_t ioUpperLimit = configRead16( kIOPCIConfigIOLimitUpper);
            
            start |= (uint64_t)ioUpperBase << 16;
            end   |= (uint64_t)ioUpperLimit << 16 | 0xFFFF;
        }
        
        if ((ioBase & 0x0F) <= (ioLimit & 0x0F) && start < end)
        {
            size = end - start + 1;
            rangeList[rangeCount++] =  {
                .start = start,
                .length = size,
                .tag = space.bits
            };
            IOPCIRange *range = new IOPCIRange;
            bzero(range, sizeof(*range));
            range->type         = kIOPCIResourceTypeIO;
            range->start        = start;
            range->proposedSize = size;
            range->totalSize    = size;
            range->end          = start;
            range->alignment    = kPCIBridgeIOAlignment;
            range->maxAddress   = 0xFFFFFFFF;
            range->allocations  = (IOPCIRange *) &range->end;
            range->size = range->proposedSize;
            range->end  = range->start + range->size;
            
            bridge->ranges[kIOPCIRangeBridgeIO] = range;
            /*ranges[kIOPCIRangeBridgeIO] = IOPCIRangeAlloc();
            IOPCIRange *range = ranges[kIOPCIRangeBridgeIO];
            IOPCIRangeInitAlloc(range, kIOPCIResourceTypeIO, start, size, kPCIBridgeMemoryAlignment);*/
            configurePCISpace("ranges", this,space,start,size);
        }
        
      /*  OSArray *r1=pvmd->decomposeAddressingProperties("reg",(OSData*)t->getProperty("reg"));
        OSDictionary *pr1=OSDictionary::withCapacity(2);
        pr1->setObject("regDEC2",r1);
        if (rangeCount){
            OSArray *r2=pvmd->decomposeAddressingProperties("ranges",(OSData*)t->getProperty("ranges"));
            pr1->setObject("rangesDEC2",r2);
        }
        t->setProperty("DEC2",pr1);*/
        
    }
    return rangeCount;
}


void VMDChildDevice::setmem(IOPCIBridge *   hostBridge)
{

    IOPCIAddressSpace  *flags;
    IOPhysicalAddress64 phys;
    IOPhysicalLength64 len;
    VMDChildDevice *t=this;
    
    
    OSArray *           array;
    IORegistryEntry *   parentEntry;
    OSData *            addressProperty;
    UInt32              cells = 5;
    int                 i, num;
    UInt32 *            reg;
    
    addressProperty = (OSData *) getProperty( "assigned-addresses" );

    if (!addressProperty) return;

    reg = (UInt32 *) addressProperty->getBytesNoCopy();
    num = addressProperty->getLength() / (sizeof(UInt32) * cells);

    array = OSArray::withCapacity(num);
    
        for (i = 0; i < num; i++)
        {
            phys = ((IOPhysicalAddress64) reg[1] << 32) | reg[2];
            len = ((IOPhysicalLength64) reg[3] << 32) | reg[4];
            
            flags = (IOPCIAddressSpace *) reg;
            
            IOMemoryDescriptor *    md;
            IOMemoryDescriptor *    ioMemory;
            IOSubMemoryDescriptor * subMem;
            IOAddressRange             range;
            bool                    ok;
            unsigned int            idx;

            for (idx = 0;
                 (md = (IODeviceMemory *) array->getObject(idx))
                      && (flags->s.registerNum != (md->getTag() & 0xff));
                 idx++) {}

            if (md)
            {
                md->retain();
                md->redirect(TASK_NULL, true);

                if ((subMem = OSDynamicCast(IOSubMemoryDescriptor, md)))
                {
                    ok = subMem->initSubRange(hostBridge->ioDeviceMemory(), phys, len, (IODirection) kIOMemoryThreadSafe);
                }
                else
                {
                    range.address = phys;
                    range.length  = len;
                    ok = md->initWithOptions(&range, 1, 0, TASK_NULL,
                                             kIOMemoryTypePhysical64
                                             // | kIOMemoryRedirected
                                             | kIODirectionNone
                                             | kIOMemoryMapperNone,
                                             NULL);
                }
                if (!ok) continue;
                md->redirect(TASK_NULL, false);
            }
            else
            {
                if (kIOPCIIOSpace == flags->s.space)
                {
                    if (!(ioMemory = hostBridge->ioDeviceMemory()))
                        md = 0;
                    else
                    {
                        phys &= 0x00ffffff; // seems bogus
                        md = IOSubMemoryDescriptor::withSubRange(ioMemory, phys, len, kIOMemoryThreadSafe);
                        if (md == 0)
                        {
                            /* didn't fit */
                            md = IOMemoryDescriptor::withAddressRange(
                                        phys + ioMemory->getPhysicalSegment(0, 0, kIOMemoryMapperNone),
                                        len, kIODirectionNone | kIOMemoryHostOnly, NULL );
                        }
                    }
                }
                else
                {
                    md = IOMemoryDescriptor::withAddressRange(
                                        phys, len, kIODirectionNone | kIOMemoryMapperNone, NULL);
                }
                ok = array->setObject(md);
            }

            if (md)
            {
                md->setTag( flags->bits );
                md->release();
            }
            else
                ok = false;
            
            reg += cells;
        }
    
        if (array->getCount())
            t->setProperty( gIODeviceMemoryKey, array);

        array->release();
    
    

}






UInt32 IOPCIDevice::extendedFindPCICapability( UInt32 capabilityID, IOByteCount * found  )
{
    uint32_t data;
    uint32_t offset, expressCap, firstOffset;

    data = expressCap = firstOffset = 0;
    if (found)
    {
        firstOffset = *found;
        *found = 0;
    }

    if (0 == ((kIOPCIStatusCapabilities << 16)
              & (configRead32( kIOPCIConfigCommand))))
        return (0);

    if (capabilityID >= 0x100)
    {
        if (!extendedFindPCICapability( kIOPCIPCIExpressCapability, (IOByteCount*)&expressCap)) return (0);

        capabilityID = -capabilityID;
        offset = 0x100;
        while (offset)
        {
            data = configRead32( offset);
            if (capabilityID == (data & 0xffff))
            {
                if (!firstOffset)
                {
                    if (found)
                        *found = offset;
                    break;
                }
                if (offset == firstOffset) firstOffset = 0;
            }
            offset = (data >> 20) & 0xfff;
            
            if (space.s.busNum>=0xe0)
            if ((offset > 0x100) || (offset & 3))
                offset = 0;
            
            if (space.s.busNum<0xe0)
            if ((offset < 0x100) || (offset & 3))
                offset = 0;
        }
    }
    else
    {
        offset = (0xff & configRead32( kIOPCIConfigCapabilitiesPtr));
        if (offset & 3)
            offset = 0;
        while (offset)
        {
            data = configRead32( offset);
            if (capabilityID == (data & 0xff))
            {
                if (!firstOffset)
                {
                    if (found)
                        *found = offset;
                    break;
                }
                if (offset == firstOffset) firstOffset = 0;
            }
            offset = (data >> 8) & 0xff;
            if (offset & 3)
                offset = 0;
            
            if (space.s.busNum>=0xe0)
            if (offset>0xff) offset = 0;
        }
    }

    return (offset ? data : 0);
    
}

UInt32 IOPCIDevice::findPCICapability( UInt8 capabilityID, UInt8 * found )
{
    
    uint32_t data;
    uint32_t offset;
    
    if (found)
        *found = 0;

   if (0 == ((kIOPCIStatusCapabilities << 16)
              & (configRead32( kIOPCIConfigCommand))))
        return (0);
    

        offset = (0xff & configRead32( kIOPCIConfigCapabilitiesPtr));
        if (offset & 3)
            offset = 0;
        while (offset)
        {
            data = configRead32( offset);
            if (capabilityID == (data & 0xff))
            {
                    *found = offset;
                    break;
            }
            offset = (data >> 8) & 0xff;
            if (offset & 3)
                offset = 0;
            
            if (space.s.busNum>=0xe0)
            if (offset>0xff) offset = 0;
        }

    return (offset ? data : 0);
}

UInt32 IOPCIDevice::extendedConfigRead32( IOByteCount offset )
{
    return configRead32(space,offset);
}
UInt16 IOPCIDevice::extendedConfigRead16( IOByteCount offset )
{
    return configRead16(space,offset);
}
UInt8 IOPCIDevice::extendedConfigRead8( IOByteCount offset )
{
    return configRead8(space,offset);
}
void IOPCIDevice::extendedConfigWrite32( IOByteCount offset, UInt32 data )
{
    configWrite32(space,offset,data);
}
void IOPCIDevice::extendedConfigWrite16( IOByteCount offset, UInt16 data )
{
    configWrite16(space,offset,data);
}
void IOPCIDevice::extendedConfigWrite8( IOByteCount offset, UInt8 data )
{
    configWrite8(space,offset,data);
}

UInt32 IOPCIDevice::configRead32(IOPCIAddressSpace space, UInt8 offset) {
    if (!configAccess(false)) return (0xFFFFFFFF);
    if (space.s.busNum<0xe0) return IOPCIDevice::configRead32( space,  offset );
    IOInterruptState ints;
    //ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
    UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
    UInt32 ret= OSReadLittleInt32(vmdConfigBase , address);
    //IOSimpleLockUnlockEnableInterrupt( lock, ints );
    return ret;
}

void IOPCIDevice::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) {
    if (!configAccess(true)) return;
    if (space.s.busNum<0xe0) return IOPCIDevice::configWrite32( space,  offset  ,data);
    IOInterruptState ints;
    //ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        OSWriteLittleInt32(vmdConfigBase , address,data);
        OSReadLittleInt32(vmdConfigBase , address);
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
    
}

UInt16 IOPCIDevice::configRead16(IOPCIAddressSpace space, UInt8 offset) {
    if (!configAccess(false)) return (0xFFFFFFFF);
    if (space.s.busNum<0xe0) return IOPCIDevice::configRead16( space,  offset );
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
    UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
    UInt16 ret= OSReadLittleInt16(vmdConfigBase , address);
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
    return ret;

}

void IOPCIDevice::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) {
    if (!configAccess(true)) return;
    if (space.s.busNum<0xe0) return IOPCIDevice::configWrite16( space,  offset  ,data);
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        OSWriteLittleInt16(vmdConfigBase , address,data);
        OSReadLittleInt16(vmdConfigBase , address);
  //  IOSimpleLockUnlockEnableInterrupt( lock, ints );
    
}

UInt8 IOPCIDevice::configRead8( IOPCIAddressSpace space, UInt8 offset ) {
    if (!configAccess(false)) return (0xFFFFFFFF);
    if (space.s.busNum<0xe0) return IOPCIDevice::configRead8( space,  offset );
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        UInt8 ret= (*((volatile uint8_t *)(vmdConfigBase + address)));
      //  IOSimpleLockUnlockEnableInterrupt( lock, ints );
        return ret;

}

void IOPCIDevice::configWrite8( IOPCIAddressSpace space,UInt8 offset, UInt8 data ) {
    if (!configAccess(true)) return;
    if (space.s.busNum<0xe0) return IOPCIDevice::configWrite8( space,  offset ,data );
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        *((volatile uint8_t *)(vmdConfigBase + address)) = data;
        UInt8 ret= (*((volatile uint8_t *)(vmdConfigBase + address)));
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
}

UInt32 IOPCI2PCIBridge::configRead32( IOPCIAddressSpace space, UInt8 offset )
{
    if (space.s.busNum<0xe0) return IOPCI2PCIBridge::configRead32( space,  offset );
    IOInterruptState ints;
    //ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
    UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
    UInt32 ret= OSReadLittleInt32(vmdConfigBase , address);
    //IOSimpleLockUnlockEnableInterrupt( lock, ints );
    return ret;
}
void IOPCI2PCIBridge::configWrite32( IOPCIAddressSpace space,
                                    UInt8 offset, UInt32 data )
{
    if (space.s.busNum<0xe0) return IOPCI2PCIBridge::configWrite32( space,  offset  ,data);
    IOInterruptState ints;
    //ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        OSWriteLittleInt32(vmdConfigBase , address,data);
        OSReadLittleInt32(vmdConfigBase , address);
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
}
UInt16 IOPCI2PCIBridge::configRead16( IOPCIAddressSpace space, UInt8 offset )
{
    if (space.s.busNum<0xe0) return IOPCI2PCIBridge::configRead16( space,  offset );
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
    UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
    UInt16 ret= OSReadLittleInt16(vmdConfigBase , address);
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
    return ret;
}
void IOPCI2PCIBridge::configWrite16( IOPCIAddressSpace space,
                                    UInt8 offset, UInt16 data )
{
    if (space.s.busNum<0xe0) return IOPCI2PCIBridge::configWrite16( space,  offset  ,data);
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        OSWriteLittleInt16(vmdConfigBase , address,data);
        OSReadLittleInt16(vmdConfigBase , address);
  //  IOSimpleLockUnlockEnableInterrupt( lock, ints );
}
UInt8 IOPCI2PCIBridge::configRead8( IOPCIAddressSpace space, UInt8 offset )
{
    if (space.s.busNum<0xe0) return IOPCI2PCIBridge::configRead8( space,  offset );
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        UInt8 ret= (*((volatile uint8_t *)(vmdConfigBase + address)));
      //  IOSimpleLockUnlockEnableInterrupt( lock, ints );
        return ret;
}
void IOPCI2PCIBridge::configWrite8( IOPCIAddressSpace space,
                                    UInt8 offset, UInt8 data )
{
    if (space.s.busNum<0xe0) return IOPCI2PCIBridge::configWrite8( space,  offset ,data );
    IOInterruptState ints;
   // ints = IOSimpleLockLockDisableInterrupt( lock );
    unsigned int busnr_ecam = space.s.busNum - 0xe0;
        UInt32 address = PCIE_ECAM_OFFSET(busnr_ecam, PCI_DEVFN(space.s.deviceNum , space.s.functionNum), offset);
        *((volatile uint8_t *)(vmdConfigBase + address)) = data;
        UInt8 ret= (*((volatile uint8_t *)(vmdConfigBase + address)));
   // IOSimpleLockUnlockEnableInterrupt( lock, ints );
}



void VMDDriver::resetDomain() {
    IOLog("VMDDriver: Resetting domain\n");

    for (uint16_t bus = 0; bus < 32; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t address = PCIE_ECAM_OFFSET(bus, PCI_DEVFN(dev , 0), 0);
            volatile uint8_t *base = vmdConfigBase1 +address;
            uint8_t hdrType = base[PCI_HEADER_TYPE];
            uint8_t functions = (hdrType & PCI_HEADER_TYPE_MFD) ? 8 : 1;
            for (uint8_t fn = 0; fn < functions; fn++) {
                
                address = PCIE_ECAM_OFFSET(bus, PCI_DEVFN(dev , fn), 0);
                
                base = vmdConfigBase1 + address;
                hdrType = base[PCI_HEADER_TYPE] & PCI_HEADER_TYPE_MASK;

                if (hdrType != PCI_HEADER_TYPE_BRIDGE ||
                    (OSReadLittleInt16(base , PCI_CLASS_DEVICE) != PCI_CLASS_BRIDGE_PCI))
                    continue;

                //panic("xxx");
                OSWriteLittleInt32(base , PCI_IO_BASE_UPPER16,0x0000ffff);
                OSWriteLittleInt16(base , PCI_IO_BASE, 0x00f0);
                OSWriteLittleInt32(base , PCI_IO_BASE_UPPER16, 0);

                OSWriteLittleInt32(base , PCI_MEMORY_BASE, 0x0000fff0);

                OSWriteLittleInt32(base , PCI_PREF_LIMIT_UPPER32, 0);
                OSWriteLittleInt32(base , PCI_PREF_MEMORY_BASE, 0x0000fff0);
                OSWriteLittleInt32(base , PCI_PREF_BASE_UPPER32, 0xffffffff);
            }
        }
    }
}




void VMDDriver::stop(IOService* provider) {
    IOService::stop(provider);

}






bool VMDChildDevice::interruptFilter(OSObject *owner,  IOFilterInterruptEventSource *source)
{

   /* VMDChildDevice *device = (VMDChildDevice*)(owner);
    if (!device)
        return false;

    int interruptType;
    
   // if (source->getIntIndex()==0) return false;
    device->getInterruptType(source->getIntIndex(), &interruptType);
    if (interruptType & kIOInterruptTypePCIMessaged) return true;
    if (interruptType & kIOInterruptTypePCIMessagedX) return true;
    
    return false;*/
    return true;
}

void VMDChildDevice::interruptHandler(OSObject *owner,  IOInterruptEventSource *source,
         int count)
{

   VMDChildDevice *device = static_cast<VMDChildDevice*>(owner);
    if (!device)
        return ;
    
    device->handleInterrupt0(owner,source,count);
}

void VMDChildDevice::handleInterrupt0(OSObject* owner, IOInterruptEventSource* src, int intCount)
{
   if (!interruptController)  return;
    int source=src->getIntIndex();
    interruptController->handleInterrupt2(nullptr,this,source);
    
}


IOReturn VMDChildDevice::getInterruptType(int source, int *interruptType) {
    
    return IOService::getInterruptType( source,interruptType);
}


IOReturn VMDChildDevice::disableInterrupt(int source) {
   // if (source<vmdVector) return kIOReturnError;
    return  IOService::disableInterrupt( source);
    return interruptController->disableInterrupt(this, source);
}

IOReturn VMDChildDevice::enableInterrupt(int source) {
   // if (source<vmdVector) return kIOReturnError;
    return  IOService::enableInterrupt( source);
    return interruptController->enableInterrupt(this, source);
}

IOReturn VMDChildDevice::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refcon) {
   // if (source<vmdVector) return kIOReturnError;
  //  interruptController->registerInterrupt(this,source,target,(IOInterruptHandler)handler ,refcon);
    return   IOService::registerInterrupt(source,target,handler ,refcon);
    IOReturn r=interruptController->registerInterrupt(this,source,target,(IOInterruptHandler)handler ,refcon);
    return r;
}






uint32_t VMDChildDevice::findPCICapability2(uint32_t capabilityID, uint32_t * found)
{
    uint32_t data;
    uint32_t offset, expressCap, firstOffset;

    data = expressCap = firstOffset = 0;
    if (found)
    {
        firstOffset = *found;
        *found = 0;
    }

   /* if (0 == ((kIOPCIStatusCapabilities << 16)
              & (configRead32( kIOPCIConfigCommand))))
        return (0);*/

    if (0)
    {

    }
    else
    {
        offset = (0xff & configRead32( kIOPCIConfigCapabilitiesPtr));
        if (offset & 3)
            offset = 0;
        while (offset)
        {
            data = configRead32( offset);
            if (capabilityID == (data & 0xff))
            {
                if (!firstOffset)
                {
                    if (found)
                        *found = offset;
                    break;
                }
                if (offset == firstOffset) firstOffset = 0;
            }
            offset = (data >> 8) & 0xff;
            if (offset & 3)
                offset = 0;
            
            if (offset>0xff) offset = 0;
        }
    }

    return (offset ? data : 0);
}





bool VMDInterruptController::init(IOService* provider) {
    if (!IOInterruptController::init()) return false;
    //workLoop = IOWorkLoop::workLoop();
    _vmdDevice = OSDynamicCast(VMDChildDevice, provider);

    // Initialize data structures
    _vectors = OSArray::withCapacity(numVectors);
    _lock = IOLockAlloc();
    
    _msixMap = pvmd->_msixMap;
    _msixTable = pvmd->_msixTable;
    
    if (!Cap)
    _vmdDevice->findPCICapability(kIOPCICapabilityIDMSI, &Cap);
    
    return true;
}

void VMDInterruptController::free() {
    if (_msixMap) {
        _msixMap->release();
        _msixMap = nullptr;
    }
    OSSafeReleaseNULL(_vectors);
    if (_lock) IOLockFree(_lock);
    IOInterruptController::free();
}

volatile UInt32* VMDInterruptController::getChildMSIXEntry(VMDChildDevice* child, int source, IOMemoryMap** map) {
    UInt8 msixCap;
    if (!child->findPCICapability(kIOPCICapabilityIDMSIX, &msixCap)) {
       // IOLog("VMD: Child device lacks MSI-X capability\\n");
        return nullptr;
    }
    
    
    // Read MSI-X message control to get table size
    UInt16 msgCtrl = child->configRead16( msixCap + 2);
    UInt16 tableSize = (msgCtrl & 0x7FF) + 1;
    if (source >= tableSize) {
       // IOLog("VMD: Child source %d exceeds MSI-X table size %d\\n", source, tableSize);
        return nullptr;
    }
    
    UInt32 table  = child->configRead32(msixCap + 4);
    UInt8 bar    = (kIOPCIConfigBaseAddress0 + ((table & 7) << 2));
    table &= ~7;
    
    *map  = child->mapDeviceMemoryWithRegister(bar);
    if (!*map ) return nullptr;
    volatile UInt32* entry = (volatile UInt32*)((*map)->getAddress() + table + source * 16);
    
    return entry;
}

IOReturn VMDInterruptController::enableInterrupt(IOService* nub, int source) {
   /* int *interruptType=0;
    getInterruptType(nub,source,interruptType);
    if (!interruptType) return kIOReturnSuccess;
    */
    _vmdDevice->setProperty("enableInterrupt", source,32);
    
   // _msixTable[3+vectorNumber*4] = 0;

    if (0/*_vmdDevice->findPCICapability(kIOPCICapabilityIDMSIX, &msixCap)*/) {
      //  if (source>vmdVector) source-=vmdVector;
        IOMemoryMap* map = nullptr;
        volatile UInt32* entry = getChildMSIXEntry(_vmdDevice, source, &map);
        //volatile UInt32* entry = (volatile UInt32*)(_vmdDevice->_msixTable + source * 16);
        if (!entry) return kIOReturnError;
        entry[3] = 0;  // Unmask
        map->release();
        return kIOReturnSuccess;
    } else if (Cap) {
        // MSI has no per-vector masking, enable at device level
        UInt16 ctrl = _vmdDevice->configRead16(Cap + 2);
        ctrl|=1;
       // _vmdDevice->configWrite16(Cap + 2, ctrl);
        return kIOReturnSuccess;
    }
    
    return kIOReturnUnsupported;
}

IOReturn VMDInterruptController::disableInterrupt(IOService* nub, int source) {
/*
    int *interruptType=0;
    getInterruptType(nub,source,interruptType);
    if (!interruptType) return kIOReturnSuccess;
    */
    _vmdDevice->setProperty("disableInterrupt", source,32);

    if (0/*_vmdDevice->findPCICapability(kIOPCICapabilityIDMSIX, &msixCap)*/) {
       // if (source>vmdVector) source-=vmdVector;
        IOMemoryMap* map = nullptr;
        volatile UInt32* entry = getChildMSIXEntry(_vmdDevice, source, &map);
        //volatile UInt32* entry = (volatile UInt32*)(_vmdDevice->_msixTable + source * 16);
        if (!entry) return kIOReturnError;
        entry[3] = 1;  // Mask
        map->release();
        return kIOReturnSuccess;
    } else if (Cap) {
        // MSI has no per-vector masking, disable entirely
        UInt16 ctrl = _vmdDevice->configRead16(Cap + 2);
        ctrl &= ~1;
       // _vmdDevice->configWrite16(Cap + 2, ctrl);
        return kIOReturnSuccess;
    }
    
    return kIOReturnUnsupported;
}

IOReturn VMDInterruptController::registerInterrupt(IOService *nub, int source,
                                                  void *target, IOInterruptHandler handler,
                                                  void *refCon) {
    
   /* int *interruptType=0;
    getInterruptType(nub,source,interruptType);
    if (!interruptType) return kIOReturnSuccess;
    */
    IOLockLock(_lock);
    
        
    if (!Cap)
    _vmdDevice->findPCICapability(kIOPCICapabilityIDMSI, &Cap);
    
    OSData            *vectorData;
    IOInterruptSource *interruptSources;
    
    interruptSources = _vmdDevice->_interruptSources;
    vectorData = interruptSources[0].vectorData;
    vmdVector = *(IOInterruptVectorNumber *)vectorData->getBytesNoCopy();
   // vmdVector =3;
    
    for (unsigned i = 0; i < _vectors->getCount(); i++) {
        ChildInterruptRecord* rec0 = (ChildInterruptRecord*)_vectors->getObject(i);
        if (rec0 && rec0->data.childSource == source && rec0->data.vmdSource == vmdVector)
        {
            IOLockUnlock(_lock);
            return kIOReturnSuccess;
        }
    }
    
    
    ChildInterruptRecord* rec = ChildInterruptRecord::create();
    if (!rec) {
        IOLockUnlock(_lock);
        return kIOReturnNoMemory;
    }
    
    
    if (_vmdDevice->getProperty("registerInterrupt"))
    _vmdDevice->setProperty("registerInterrupt2", source,32);
    
    _vmdDevice->setProperty("registerInterrupt", source,32);
    
    
    rec->data.childNub = nub;
    rec->data.childSource = source;
    rec->data.handler = handler;
    rec->data.target = target;
    rec->data.refCon = refCon;
    rec->data.vmdSource=vmdVector;
    _vectors->setObject(rec);
    rec->release();
    
    uint16_t control;
    
    programChildMSI(_vmdDevice, 0,vmdVector);
    

    rec->data.handler(
        rec->data.target,
        rec->data.refCon,
        rec->data.childNub,    // Child's nub (e.g., IOPCIDevice)
        rec->data.childSource  // Child's original source (e.g., 0)
    );
    
   // UInt8 msixCap, msiCap;
  //  bool hasMSIX = false;//_vmdDevice->findPCICapability(kIOPCICapabilityIDMSIX, &msixCap);
 //   bool hasMSI = _vmdDevice->findPCICapability(kIOPCICapabilityIDMSI, &msiCap);
       
   
   // control= _vmdDevice->configRead16( Cap + 2);

   /* if ()
    {
        control |= (1 << 15);
    }
    else
    {*/
      //  control |= 1;
   // }
    
  /*  _vmdDevice->configWrite16(Cap + 2, control);

    control = _vmdDevice->configRead16(kIOPCIConfigCommand);
    control |= kIOPCICommandInterruptDisable | kIOPCICommandBusMaster;
    _vmdDevice->configWrite16(kIOPCIConfigCommand, control);
    _vmdDevice->setProperty("IOPCIMSIMode", kOSBooleanTrue);
    */
    
   for (int i=0;i<12;i++){
        
        if (_vmdDevice) {
            
            UInt8 *configBytes = (UInt8 *)IOMalloc(0x100);
             for (UInt16 offset = 0; offset < 0x100; offset++) {
                 configBytes[offset] = _vmdDevice->configRead8(offset);
             }
            _vmdDevice->setProperty("cfgspace", configBytes, 0x100);
        }
                
    }

   // _nextVector++;
    IOLockUnlock(_lock);
    return kIOReturnSuccess;
}

IOInterruptAction VMDInterruptController::getInterruptHandlerAddress(void) {
    return OSMemberFunctionCast(IOInterruptAction, this, &VMDInterruptController::handleInterrupt);
}

IOReturn VMDInterruptController::getInterruptType(IOService *nub, int source,
                                                 int *interruptType) {
   
    if (interruptType!=nullptr) *interruptType=0;
    if (source<vmdVector) return kIOReturnSuccess;
    source-=vmdVector;
    if (source>=numVectors) return kIOReturnError;
    if (interruptType!=nullptr) *interruptType=kIOInterruptTypePCIMessaged;
    return kIOReturnSuccess;;
}

void VMDInterruptController::programChildMSI(VMDChildDevice* child, int childSource,
                                             IOInterruptVectorNumber vmdVector) {

    
    /*OSData            *vectorData;
    IOInterruptSource *interruptSources;
    
    interruptSources = child->_interruptSources;
    vectorData = interruptSources[childSource].vectorData;
    vmdVector = *(IOInterruptVectorNumber *)vectorData->getBytesNoCopy();
    */
    //child->setProperty("programChildMSI", childSource,32);
    //child->setProperty("programChildMSIvectorNumber", vmdVector,32);
    
    uint16_t  control=child->configRead16( Cap + 2);
    
    bool is64bit = control & 0x0080;

    UInt32 msiAddress = _msixTable[vmdVector * 4];
    UInt32 msiAddressHi = _msixTable[vmdVector * 4 + 1];
    UInt32 msiData = _msixTable[vmdVector * 4 + 2];
    
    child->configWrite32(Cap + PCI_MSI_ADDRESS_LO, msiAddress);
    if (is64bit) {
        child->configWrite32(Cap + PCI_MSI_ADDRESS_HI, msiAddressHi);
    }
    
    child->configWrite16(Cap + (is64bit ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32), msiData);

}

void VMDInterruptController::programChildMSIX(VMDChildDevice* child, int childSource,
                                             IOInterruptVectorNumber vmdVector) {
    
    /*UInt8 cap;
    if (!child->findPCICapability(kIOPCICapabilityIDMSIX, &cap)) {
      //  IOLog("VMD: Child lacks MSI capability\n");
        return;
    }*/
    
    
    IOMemoryMap* map = nullptr;
    volatile UInt32* entry = getChildMSIXEntry(child, childSource, &map);
    if (!entry) return ;
    
  /*  struct msi_msg *msg=(struct msi_msg *) entry;
        memset(msg, 0, sizeof(*msg));
        msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;
        msg->arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW;
        msg->arch_addr_lo.destid_0_7 = _msixTable[vmdVector * 4 + 2];*/
    
    UInt32 msiAddress = _msixTable[vmdVector * 4];     // Address from VMD's table
    UInt32 msiAddressHi = _msixTable[vmdVector * 4 + 1];
    UInt32 msiData = _msixTable[vmdVector * 4 + 2];    // Data/vector*/
    
    entry[3] = 1;
    entry[0] = msiAddress;      // Address Low
    entry[1] = msiAddressHi;  // Address High
    entry[2] = msiData;
    map->release();
    
}

IOReturn VMDInterruptController::handleInterrupt(void *refCon, IOService *nub, int source) {

    for (unsigned i = 0; i < _vectors->getCount(); i++) {
        ChildInterruptRecord* rec = (ChildInterruptRecord*)_vectors->getObject(i);
        if (rec && rec->data.childSource == source)
        {
           // if (source==0) panic("x");
                rec->data.handler(
                    rec->data.target,
                    rec->data.refCon,
                    rec->data.childNub,    // Child's nub (e.g., IOPCIDevice)
                    rec->data.childSource  // Child's original source (e.g., 0)
                );
            break;
        }
            }
    

    return kIOReturnSuccess;
}


IOReturn VMDInterruptController::handleInterrupt2(void *refCon, IOService *nub, int source) {

    for (unsigned i = 0; i < _vectors->getCount(); i++) {
        ChildInterruptRecord* rec = (ChildInterruptRecord*)_vectors->getObject(i);
        if (rec->data.vmdSource == source)
        {
            // panic("x");
                rec->data.handler(
                    rec->data.target,
                    rec->data.refCon,
                    rec->data.childNub,    // Child's nub (e.g., IOPCIDevice)
                    rec->data.childSource  // Child's original source (e.g., 0)
                );
            break;
        }
            }
    

    return kIOReturnSuccess;
}


