#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <i386/cpuid.h>
#include <i386/proc_reg.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>

static UInt32 intelPackageTemperature()
{
    UInt32 id[4]{};
    do_cpuid(0, id);
    if (id[0] < 6 || id[1] != 0x756e6547 || id[3] != 0x49656e69 || id[2] != 0x6c65746e)
        return 0;
    do_cpuid(1, id);
    const UInt32 model = ((id[0] >> 12) & 0xf0) | ((id[0] >> 4) & 0x0f);
    if (((id[0] >> 8) & 0xf) != 6 || (model != 0x9a && model != 0xa5) ||
        (id[2] & (1u << 31)) || !(id[3] & (1u << 5)))
        return 0;
    do_cpuid(6, id);
    constexpr UInt32 required = (1u << 0) | (1u << 6);
    if ((id[0] & required) != required)
        return 0;

    const auto thermal = rdmsr64(0x1b1); // IA32_PACKAGE_THERM_STATUS
    if (!(thermal & (1ull << 31)))
        return 0;
    const UInt32 target = (rdmsr64(0x1a2) >> 16) & 0xff; // IA32_TEMPERATURE_TARGET
    const UInt32 delta = (thermal >> 16) & 0x7f;
    if (target < 60 || target > 125 || delta >= target)
        return 0;
    return target - delta;
}

class OmenMuxProbe : public IOService {
    OSDeclareDefaultStructors(OmenMuxProbe)
    char muxMethod[5]{};
    IOLock *muxLock = nullptr;
    IOWorkLoop *telemetryLoop = nullptr;
    IOTimerEventSource *telemetryTimer = nullptr;
    bool telemetryAttached = false;
    UInt8 fanCount = 0;
    void readFans(IOACPIPlatformDevice *device);
    static void sampleTelemetry(OSObject *owner, IOTimerEventSource *timer);
    IOReturn query(IOACPIPlatformDevice *device, UInt32 operation, UInt8 input, UInt8 *mode,
                   UInt32 command = 0x52, UInt32 inputLength = 4,
                   unsigned responseOffset = 0, bool reportMux = true,
                   UInt8 *nextByte = nullptr, UInt32 selector = 2);
    void readMux(IOACPIPlatformDevice *device, const OSData *descriptors);
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
#ifndef OMEN_MUX_READ_ONLY
    IOReturn setProperties(OSObject *properties) override;
#endif
};

OSDefineMetaClassAndStructors(OmenMuxProbe, IOService)

bool OmenMuxProbe::start(IOService *provider)
{
    auto *device = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!device || !IOService::start(provider))
        return false;

    muxLock = IOLockAlloc();
    if (!muxLock)
        return false;
#ifndef OMEN_MUX_READ_ONLY
    setProperty("OmenMuxWriteEnabled", true);
    setProperty("OmenMuxDirectWriteEnabled", true);
#endif

    if (const auto *dsdt = device->getACPITableData("DSDT"))
        setProperty("OmenMuxDSDT", dsdt);

    OSObject *result = nullptr;
    auto status = device->evaluateObject("_WDG", &result);
    setProperty("OmenMuxWDGStatus", static_cast<UInt32>(status), 32);
    if (status == kIOReturnSuccess) {
        if (auto *data = OSDynamicCast(OSData, result)) {
            setProperty("OmenMuxWDG", data);
            readMux(device, data);
        }
    }
    if (result)
        result->release();

    if (muxMethod[0]) {
        telemetryLoop = IOWorkLoop::workLoop();
        telemetryTimer = IOTimerEventSource::timerEventSource(this, sampleTelemetry);
        if (telemetryLoop && telemetryTimer &&
            telemetryLoop->addEventSource(telemetryTimer) == kIOReturnSuccess) {
            telemetryAttached = true;
            if (telemetryTimer->setTimeoutMS(2000) == kIOReturnSuccess)
                setProperty("OmenTelemetryLive", true);
        }
    }

    registerService();
    return true;
}

void OmenMuxProbe::readMux(IOACPIPlatformDevice *device, const OSData *descriptors)
{
    // ACPI WMI GUIDs use little-endian encoding for the first three fields.
    constexpr UInt8 biosGuid[] = {
        0x34, 0xf0, 0xb7, 0x5f, 0x63, 0x2c, 0xe9, 0x45,
        0xbe, 0x91, 0x3d, 0x44, 0xe2, 0xc7, 0x07, 0xe4,
    };
    constexpr unsigned descriptorSize = 20;
    const auto size = descriptors->getLength();
    if (size % descriptorSize)
        return;

    char method[] = "WM00";
    bool found = false;
    for (unsigned offset = 0; offset < size; offset += descriptorSize) {
        const auto *entry = static_cast<const UInt8 *>(
            descriptors->getBytesNoCopy(offset, descriptorSize));
        if (!entry || memcmp(entry, biosGuid, sizeof(biosGuid)) != 0)
            continue;
        // Require a binary method block with instance zero available.
        if (found || entry[18] == 0 || entry[19] != 0x02)
            return;
        for (unsigned index = 0; index < 2; ++index) {
            const auto character = entry[16 + index];
            if (!((character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_'))
                return;
            method[2 + index] = static_cast<char>(character);
        }
        found = true;
    }
    if (!found)
        return;

    memcpy(muxMethod, method, sizeof(muxMethod));
    setProperty("OmenMuxMethod", method);
#ifndef OMEN_MUX_READ_ONLY
    setProperty("OmenPerformanceControlEnabled", true);
#endif
    UInt8 mode = 0;
    if (query(device, 1, 0, &mode) == kIOReturnSuccess)
        setProperty("OmenMuxModeByte", mode, 8);

    readFans(device);
}

void OmenMuxProbe::readFans(IOACPIPlatformDevice *device)
{
    if (!fanCount) {
        UInt8 count = 0;
        if (query(device, 0x20008, 0, &count, 0x10, 1, 0, false) != kIOReturnSuccess ||
            count < 1 || count > 4)
            return;
        fanCount = count;
        setProperty("OmenFanCount", count, 8);
#ifndef OMEN_MUX_READ_ONLY
        if (count == 2)
            setProperty("OmenFanControlEnabled", true);
#endif
    }
    for (UInt8 fan = 0; fan < fanCount; ++fan) {
        UInt8 high = 0, low = 0;
        char property[] = "OmenFan0RPM";
        property[7] = static_cast<char>('0' + fan);
        if (query(device, 0x20008, fan, &high, 0x11, 1, 2, false, &low) == kIOReturnSuccess)
            setProperty(property, (static_cast<UInt32>(high) << 8) | low, 32);
        else
            removeProperty(property);
    }
}

void OmenMuxProbe::sampleTelemetry(OSObject *owner, IOTimerEventSource *timer)
{
    auto *probe = static_cast<OmenMuxProbe *>(owner);
    auto *device = OSDynamicCast(IOACPIPlatformDevice, probe->getProvider());
    if (!device)
        return;
    IOLockLock(probe->muxLock);
    probe->readFans(device);
    UInt8 value = 0;
    if (probe->query(device, 1, 0, &value, 0x4c, 4, 0, false) == kIOReturnSuccess)
        probe->setProperty("OmenPerformanceRaw", value, 8);
    if (probe->query(device, 0x20008, 0, &value, 0x26, 4, 0, false) == kIOReturnSuccess && value <= 1)
        probe->setProperty("OmenFanMaxState", value, 8);
    const auto temperature = intelPackageTemperature();
    if (temperature)
        probe->setProperty("OmenCPUPackageTemperature", temperature, 32);
    else
        probe->removeProperty("OmenCPUPackageTemperature");
    IOLockUnlock(probe->muxLock);
    if (timer->setTimeoutMS(2000) != kIOReturnSuccess)
        probe->removeProperty("OmenTelemetryLive");
}

IOReturn OmenMuxProbe::query(IOACPIPlatformDevice *device, UInt32 operation,
                           UInt8 input, UInt8 *mode, UInt32 command, UInt32 inputLength,
                           unsigned responseOffset, bool reportMux, UInt8 *nextByte, UInt32 selector)
{
    struct {
        UInt32 signature;
        UInt32 operation;
        UInt32 query;
        UInt32 inputLength;
        UInt8 data[128];
    } request{0x55434553, operation, command, inputLength, {input}};

    OSObject *arguments[] = {
        OSNumber::withNumber(UInt64{0}, 32), // WMI instance
        OSNumber::withNumber(UInt64{selector}, 32),
        OSData::withBytes(&request, sizeof(request)),
    };
    OSObject *result = nullptr;
    IOReturn outcome = kIOReturnNoMemory;
    if (arguments[0] && arguments[1] && arguments[2]) {
        auto status = device->evaluateObject(muxMethod, &result, arguments, 3);
        if (reportMux) setProperty(mode ? "OmenMuxReadStatus" : "OmenMuxWriteStatus", static_cast<UInt32>(status), 32);
        outcome = status;
        if (status == kIOReturnSuccess) {
            outcome = kIOReturnBadMedia;
            if (auto *data = OSDynamicCast(OSData, result)) {
                if (data->getLength() >= (mode ? 9u + responseOffset + (nextByte ? 1u : 0u) : 8u)) {
                    const auto *bytes = static_cast<const UInt8 *>(data->getBytesNoCopy());
                    UInt32 firmwareStatus;
                    memcpy(&firmwareStatus, bytes + 4, sizeof(firmwareStatus));
                    if (reportMux) setProperty(mode ? "OmenMuxFirmwareStatus" : "OmenMuxWriteFirmwareStatus",
                                firmwareStatus, 32);
                    if (firmwareStatus == 0) {
                        if (mode)
                            *mode = bytes[8 + responseOffset];
                        if (nextByte)
                            *nextByte = bytes[9 + responseOffset];
                        outcome = kIOReturnSuccess;
                    }
                }
            }
        }
    }
    for (auto *argument : arguments) {
        if (argument)
            argument->release();
    }
    if (result)
        result->release();
    return outcome;
}

void OmenMuxProbe::stop(IOService *provider)
{
    if (telemetryAttached) {
        telemetryTimer->disable();
        telemetryLoop->removeEventSource(telemetryTimer);
        telemetryTimer->cancelTimeout();
        telemetryAttached = false;
    }
    IOService::stop(provider);
}

void OmenMuxProbe::free()
{
    if (telemetryTimer)
        telemetryTimer->release();
    if (telemetryLoop)
        telemetryLoop->release();
    if (muxLock)
        IOLockFree(muxLock);
    IOService::free();
}

#ifndef OMEN_MUX_READ_ONLY
IOReturn OmenMuxProbe::setProperties(OSObject *properties)
{
    auto *dictionary = OSDynamicCast(OSDictionary, properties);
    if (dictionary && dictionary->getCount() == 1) {
        auto *profile = OSDynamicCast(OSNumber, dictionary->getObject("OmenPerformanceTarget"));
        if (profile) {
            const auto requested = profile->unsigned64BitValue();
            if (requested > 2)
                return kIOReturnBadArgument;
            auto *device = OSDynamicCast(IOACPIPlatformDevice, getProvider());
            if (!device || !muxLock || !muxMethod[0])
                return kIOReturnNotReady;
            IOLockLock(muxLock);
            UInt8 current = 0;
            auto outcome = query(device, 1, 0, &current, 0x4c, 4, 0, false);
            if (outcome == kIOReturnSuccess && current > 2)
                outcome = kIOReturnUnsupported;
            if (outcome == kIOReturnSuccess && current != requested)
                outcome = query(device, 2, static_cast<UInt8>(requested), nullptr,
                                0x4c, 4, 0, false, nullptr, 1);
            if (outcome == kIOReturnSuccess) {
                outcome = query(device, 1, 0, &current, 0x4c, 4, 0, false);
                if (outcome == kIOReturnSuccess) {
                    setProperty("OmenPerformanceRaw", current, 8);
                    if (current != requested)
                        outcome = kIOReturnNotReady;
                }
            }
            IOLockUnlock(muxLock);
            return outcome;
        }
        auto *fan = OSDynamicCast(OSNumber, dictionary->getObject("OmenFanMaxTarget"));
        if (fan) {
            if (fan->unsigned64BitValue() > 1)
                return kIOReturnBadArgument;
            auto *device = OSDynamicCast(IOACPIPlatformDevice, getProvider());
            if (!device || !muxLock)
                return kIOReturnNotReady;
            IOLockLock(muxLock);
            auto outcome = fanCount == 2
                ? query(device, 0x20008, fan->unsigned8BitValue(), nullptr, 0x27, 4, 0, false, nullptr, 1)
                : kIOReturnNotReady;
            IOLockUnlock(muxLock);
            return outcome;
        }
    }
    auto *target = dictionary ? OSDynamicCast(OSNumber, dictionary->getObject("OmenMuxTarget")) : nullptr;
    if (!target || dictionary->getCount() != 1 || target->unsigned64BitValue() > 1)
        return kIOReturnBadArgument;
    auto *device = OSDynamicCast(IOACPIPlatformDevice, getProvider());
    if (!device || !muxLock || !muxMethod[0])
        return kIOReturnNotReady;

    IOLockLock(muxLock);
    UInt8 current = 0;
    auto outcome = query(device, 1, 0, &current);
    const auto requested = target->unsigned8BitValue();
    if (outcome == kIOReturnSuccess) {
        if ((current & 0x7f) > 1) {
            outcome = kIOReturnUnsupported;
        } else if ((current & 0x7f) != requested || getProperty("OmenMuxPendingMode")) {
            outcome = query(device, 2, requested, nullptr);
            if (outcome == kIOReturnSuccess)
                setProperty("OmenMuxPendingMode", requested, 8);
        }
    }
    IOLockUnlock(muxLock);
    return outcome;
}
#endif

#include <mach/kmod.h>

static kern_return_t muxModuleStart(kmod_info_t *, void *)
{
    return KERN_SUCCESS;
}

static kern_return_t muxModuleStop(kmod_info_t *, void *)
{
    return KERN_SUCCESS;
}

extern "C" {
KMOD_EXPLICIT_DECL(local.omen.MuxProbe, "0.1.9", muxModuleStart, muxModuleStop)
}
