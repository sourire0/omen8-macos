#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/libkern.h>

class HPIPC4SharedOpenProbe final : public IOService {
    OSDeclareDefaultStructors(HPIPC4SharedOpenProbe)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    static bool hasAppleHDAChild(IOPCIDevice *device) noexcept;
    static uint32_t openClientCount(IOPCIDevice *device,
                                    bool *appleHDAOpen) noexcept;
    IOPCIDevice *pci_ = nullptr;
    bool opened_ = false;
};

OSDefineMetaClassAndStructors(HPIPC4SharedOpenProbe, IOService)

bool HPIPC4SharedOpenProbe::hasAppleHDAChild(IOPCIDevice *device) noexcept {
    if (!device) return false;
    OSIterator *children = device->getChildIterator(gIOServicePlane);
    if (!children) return false;
    bool found = false;
    while (OSObject *object = children->getNextObject()) {
        IOService *service = OSDynamicCast(IOService, object);
        if (service && service->getMetaClass() &&
            service->getMetaClass()->getClassName() &&
            !strcmp(service->getMetaClass()->getClassName(),
                    "AppleHDAController")) {
            found = true;
            break;
        }
    }
    children->release();
    return found;
}

uint32_t HPIPC4SharedOpenProbe::openClientCount(
    IOPCIDevice *device, bool *appleHDAOpen) noexcept {
    if (appleHDAOpen) *appleHDAOpen = false;
    if (!device) return 0xffffffffU;
    OSIterator *clients = device->getOpenClientIterator();
    if (!clients) return 0xffffffffU;
    uint32_t count = 0;
    while (OSObject *object = clients->getNextObject()) {
        IOService *service = OSDynamicCast(IOService, object);
        if (!service) continue;
        ++count;
        const OSMetaClass *meta = service->getMetaClass();
        const char *name = meta ? meta->getClassName() : nullptr;
        if (appleHDAOpen && name &&
            !strcmp(name, "AppleHDAController"))
            *appleHDAOpen = true;
    }
    clients->release();
    return count;
}

bool HPIPC4SharedOpenProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    pci_ = OSDynamicCast(IOPCIDevice, provider);
    if (!pci_) {
        setProperty("Probe.Result", "provider is not IOPCIDevice");
        return true;
    }

    const uint32_t vendorDevice = pci_->configRead32(kIOPCIConfigVendorID);
    const uint16_t subsystemVendor =
        pci_->configRead16(kIOPCIConfigSubSystemVendorID);
    const uint16_t subsystem =
        pci_->configRead16(kIOPCIConfigSubSystemID);
    const bool target = vendorDevice == 0x51c88086U &&
        subsystemVendor == 0x103cU && subsystem == 0x8a13U;
    setProperty("Probe.ReadOnly", true);
    setProperty("Probe.PciIdentityVerified", target);
    if (!target) {
        setProperty("Probe.Result", "PCI identity mismatch; no open attempted");
        return true;
    }

    const bool hdaBefore = hasAppleHDAChild(pci_);
    bool ownerBefore = false;
    const uint32_t clientsBefore = openClientCount(pci_, &ownerBefore);
    setProperty("Probe.AppleHDAChildBefore", hdaBefore);
    setProperty("Probe.OpenClientCountBefore", clientsBefore, 32);
    setProperty("Probe.AppleHDAOpenBefore", ownerBefore);
    if (!hdaBefore) {
        setProperty("Probe.Result", "AppleHDAController not active; skipped");
        return true;
    }

    // No kIOServiceSeize, BAR mapping, PCI command writes, or MMIO access.
    opened_ = pci_->open(this, 0, nullptr);
    const bool openSucceeded = opened_;
    setProperty("Probe.OpenWithoutSeizeSucceeded", openSucceeded);
    const bool hdaAfter = hasAppleHDAChild(pci_);
    bool ownerAfter = false;
    const uint32_t clientsAfter = openClientCount(pci_, &ownerAfter);
    setProperty("Probe.AppleHDAChildAfterOpen", hdaAfter);
    setProperty("Probe.OpenClientCountAfterOpen", clientsAfter, 32);
    setProperty("Probe.AppleHDAOpenAfterOpen", ownerAfter);
    if (opened_) {
        pci_->close(this, 0);
        opened_ = false;
    }
    const bool hdaAfterClose = hasAppleHDAChild(pci_);
    setProperty("Probe.AppleHDAChildAfterClose", hdaAfterClose);
    const char *result = !openSucceeded
        ? "open without seize failed"
        : (!hdaAfter || !hdaAfterClose
            ? "AppleHDA disappeared during open; inspect diagnostics"
            : "non-seize open completed; AppleHDA stayed active");
    setProperty("Probe.Result", result);
    return true;
}

void HPIPC4SharedOpenProbe::stop(IOService *provider) {
    if (opened_ && pci_) {
        pci_->close(this, 0);
        opened_ = false;
    }
    pci_ = nullptr;
    IOService::stop(provider);
}
