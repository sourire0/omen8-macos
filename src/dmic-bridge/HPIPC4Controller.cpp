#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/kmod.h>
#include "HdaControllerStatusReader.hpp"
#include "Cavs25CodeLoaderStreamPlan.hpp"
#include "Cavs25CodeLoaderSetupPlan.hpp"
#include "Cavs25CodeLoaderMmioExecutor.hpp"
#include "Cavs25RomCommandExecutor.hpp"
#include "FirmwareImageHashKernel.hpp"
#include "FirmwareManifestKernel.hpp"
#include "FirmwareKernelData.hpp"
#include "Cavs25MappedDma.hpp"
#include "Cavs25BootLifecycle.hpp"
#include "Cavs25RomStatusReader.hpp"
#include "DmicCaptureDmaConfig.hpp"
#include "DmicCapturePeriodPublisher.hpp"
#include "HdaCaptureMappedDma.hpp"
#include "HdaCapturePositionPlan.hpp"
#include "HdaCaptureRegisterPlan.hpp"
#include "HdaCaptureResetPlan.hpp"
#include "HdaDmicCapturePlan.hpp"
#include "DmicSharedCaptureRing.hpp"
#include "DmicKernelSession.hpp"
#include "DmicKernelCaptureRuntime.hpp"
#include "DmicCapturePeriodPump.hpp"
#include "DmicCaptureInterruptService.hpp"
#include "DmicCaptureInterruptBinding.hpp"
#include "AppleHDAStreamLease.hpp"
#include "Ipc4MmioAdapter.hpp"

extern "C" kern_return_t HPIPC4_start(kmod_info_t *, void *);
extern "C" kern_return_t HPIPC4_stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(cn.laobamac.SimpleHDA.DmicBridge, "0.1.0", HPIPC4_start, HPIPC4_stop)

kern_return_t HPIPC4_start(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

kern_return_t HPIPC4_stop(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}

constexpr bool kAppleHdaOnlyMode = true;
constexpr uint32_t kTargetPciId = 0x51c88086;
constexpr uint16_t kTargetSubsystemVendor = 0x103c;
constexpr uint16_t kTargetSubsystem = 0x8a13;
constexpr uint64_t kHdaBarMinimum = 0x4000;
constexpr uint64_t kDspBarMinimum = 0x100000;
constexpr uint32_t kIpcTimeoutMilliseconds = 500;
constexpr uint32_t kIpcPollIntervalMicroseconds = 100;

namespace {
struct LoaderRoutingChange {
    volatile uint32_t *ppctl;
    volatile uint32_t *spibControl;
    volatile uint32_t *spibPosition;
    volatile const uint32_t *streamControl;
    volatile uint32_t *streamCbl;
    volatile uint16_t *streamLvi;
    volatile const uint32_t *streamBdl;
    uint32_t streamBit;
    uint32_t firmwareBytes;
    bool enable;
    uint32_t ppctlBefore;
    uint32_t ppctlAfter;
    uint32_t spibControlBefore;
    uint32_t spibPositionBefore;
    uint32_t cblBefore;
    uint16_t lviBefore;
};

IOReturn changeLoaderRouting(OSObject *, void *context, void *, void *, void *) {
    auto &change = *static_cast<LoaderRoutingChange *>(context);
    OSSynchronizeIO();
    if ((*change.streamControl & 3U) != 0) return kIOReturnBusy; // SRST or RUN
    const uint32_t ppctl = *change.ppctl;
    const uint32_t spibControl = *change.spibControl;
    constexpr uint32_t kProcessingEnabled = 1U << 30;
    if (ppctl == UINT32_MAX || spibControl == UINT32_MAX ||
        !(ppctl & kProcessingEnabled))
        return kIOReturnNotReady;
    if (change.enable) {
        // The AppleHDA 600.2 allocator emits one BDL entry per 4 KiB page.
        // This image ends on a page boundary, so a shorter LVI/CBL can
        // describe the exact transfer using the controller's existing BDL.
        constexpr uint32_t kBdlPageBytes = 4096;
        change.cblBefore = *change.streamCbl;
        change.lviBefore = *change.streamLvi;
        const uint32_t descriptorCount = static_cast<uint32_t>(change.lviBefore) + 1U;
        if (descriptorCount > 256 ||
            change.cblBefore != descriptorCount * kBdlPageBytes ||
            change.firmwareBytes == 0 || change.firmwareBytes > change.cblBefore ||
            (change.firmwareBytes % kBdlPageBytes) != 0 ||
            (change.streamBdl[0] == 0 && change.streamBdl[1] == 0))
            return kIOReturnNotReady;
        change.ppctlBefore = ppctl;
        change.spibControlBefore = spibControl;
        change.spibPositionBefore = *change.spibPosition;
        if (change.spibPositionBefore == UINT32_MAX) return kIOReturnNotReady;
    }
    const uint32_t ppBit = change.enable ? change.streamBit
                                        : change.ppctlBefore & change.streamBit;
    const uint32_t spibBit = change.enable ? change.streamBit
                                          : change.spibControlBefore & change.streamBit;
    const uint32_t nextPpctl = (ppctl & ~change.streamBit) | ppBit;
    const uint32_t nextSpibControl = (spibControl & ~change.streamBit) | spibBit;
    const uint32_t position = change.enable ? change.firmwareBytes
                                           : change.spibPositionBefore;
    const uint32_t cbl = change.enable ? change.firmwareBytes : change.cblBefore;
    const uint16_t lvi = change.enable
        ? static_cast<uint16_t>(change.firmwareBytes / 4096U - 1U) : change.lviBefore;
    *change.ppctl = nextPpctl;
    *change.streamCbl = cbl;
    *change.streamLvi = lvi;
    *change.spibPosition = position;
    *change.spibControl = nextSpibControl;
    OSSynchronizeIO();
    change.ppctlAfter = *change.ppctl;
    return change.ppctlAfter == nextPpctl && *change.spibControl == nextSpibControl &&
           *change.spibPosition == position && *change.streamCbl == cbl &&
           *change.streamLvi == lvi ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn triggerFirmwareLoader(OSObject *, void *context, void *, void *, void *) {
    const auto &change = *static_cast<const LoaderRoutingChange *>(context);
    auto *control = reinterpret_cast<volatile uint8_t *>(
        const_cast<volatile uint32_t *>(change.streamControl));
    constexpr uint8_t kReset = 1U;
    constexpr uint8_t kRun = 2U;
    constexpr uint8_t kInterruptBits = 0x1cU;
    OSSynchronizeIO();
    const uint8_t before = *control;
    if (before == UINT8_MAX || (before & kReset)) return kIOReturnNotReady;
    if (change.enable &&
        ((before & kRun) || *change.streamCbl != change.firmwareBytes ||
         *change.streamLvi != change.firmwareBytes / 4096U - 1U ||
         !(*change.ppctl & change.streamBit) ||
         !(*change.spibControl & change.streamBit) ||
         *change.spibPosition != change.firmwareBytes))
        return kIOReturnNotReady;

    // Byte access to SDnCTL avoids the adjacent write-one-to-clear SDnSTS.
    const uint8_t requested = change.enable ? before | kRun | kInterruptBits
        : before & static_cast<uint8_t>(~(kRun | kInterruptBits));
    *control = requested;
    for (uint32_t poll = 0; poll < 100; ++poll) {
        OSSynchronizeIO();
        const uint8_t observed = *control;
        if (observed != UINT8_MAX && (observed & kRun) == (requested & kRun))
            return kIOReturnSuccess;
        IODelay(10);
    }
    return kIOReturnTimeout;
}

struct CaptureRoutingChange {
    volatile uint32_t *ppctl;
    volatile const uint32_t *streamControl;
    uint32_t streamBit;
    uint32_t streamTag;
    bool enable;
    bool ownsBit;
};

IOReturn changeCaptureRouting(OSObject *, void *context, void *, void *, void *) {
    auto &change = *static_cast<CaptureRoutingChange *>(context);
    OSSynchronizeIO();
    const uint32_t control = *change.streamControl;
    const uint32_t ppctl = *change.ppctl;
    if ((control & 0x00f00003U) != (change.streamTag << 20) ||
        ppctl == UINT32_MAX || !(ppctl & (1U << 30)))
        return kIOReturnNotReady;
    if (change.enable && (ppctl & change.streamBit)) return kIOReturnSuccess;
    const uint32_t next = change.enable ? ppctl | change.streamBit
                                        : ppctl & ~change.streamBit;
    *change.ppctl = next;
    OSSynchronizeIO();
    const uint32_t observed = *change.ppctl;
    if (observed != UINT32_MAX)
        change.ownsBit = (observed & change.streamBit) != 0;
    return observed == next ? kIOReturnSuccess : kIOReturnIOError;
}
} // namespace

class AppleHDADmicBridge final : public IOService {
    OSDeclareDefaultStructors(AppleHDADmicBridge)

public:
    bool init(OSDictionary *dictionary = nullptr) override;
    void free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties,
                           IOUserClient **handler) override;

    IOReturn startPublishedCapture() noexcept;
    IOReturn stopPublishedCapture() noexcept;
    void quarantineCaptureClientLoss() noexcept;
    void releaseUserClientLease() noexcept {
        __atomic_store_n(&userClientOpen_, false, __ATOMIC_RELEASE);
    }
    IOBufferMemoryDescriptor *sharedCaptureRing() noexcept {
        return captureRing_.memoryDescriptor();
    }

private:
    bool ownerServicePresent(IOPCIDevice *device) noexcept;
    bool controllerProviderReady() noexcept;
    bool pciFunctionInD0(IOPCIDevice *device) noexcept;
    bool preparePostResetReservation() noexcept;
    bool prepareCaptureRegisterSetupPlan() noexcept;
    bool prepareInputCaptureHda() noexcept;
    bool prepareLinkStream() noexcept;
    void auditMultiLink(bool afterRun) noexcept;
    bool enableFirmwareTrace() noexcept;
    void drainFirmwareTrace() noexcept;
    void settleFirmwareTrace() noexcept;
    void snapshotFirmwareTrace() noexcept;
    bool setLinkStreamRunning(bool running) noexcept;
    bool programCaptureHdaSetup() noexcept;
    bool prepareCodeLoaderStream() noexcept;
    bool advanceRomToInitDone() noexcept;
    bool advanceFirmwareToLoaderStopped() noexcept;
    bool advanceBootToFirmwareReady() noexcept;
    bool serviceFirmwareReadyNotification() noexcept;
    bool prepareFirmwareReadyTransport() noexcept;
    bool initializeCaptureSession() noexcept;
    bool resetStoppedCaptureSession() noexcept;
    bool initializeCaptureRuntime() noexcept;
    bool installCaptureInterruptBinding() noexcept;
    bool rebaseCapturePositionBeforeInterruptEnable() noexcept;
    bool clearPendingCaptureIocBeforeStart() noexcept;
    uint32_t readAppleHdaCaptureControl() noexcept;
    bool setAppleHdaCaptureRouting(bool enable) noexcept;
    void handleCaptureInterrupt(
        sof::ipc4::dmic::kernel::CaptureInterruptServiceResult result,
        sof::ipc4::kernel::HdaCaptureTriggerAction action) noexcept;
    static void captureInterruptAction(
        void *context,
        sof::ipc4::dmic::kernel::CaptureInterruptServiceResult result,
        sof::ipc4::kernel::HdaCaptureTriggerAction action);
    void stopCaptureAfterStartFailure() noexcept;
    static bool handleIpc4Notification(
        void *context,
        const sof::ipc4::HeaderFields *header) noexcept;
    bool releaseResources() noexcept;

    IOPCIDevice *pci_;
    IOService *appleHdaController_;
    IOMemoryMap *hdaMap_;
    IOMemoryMap *dspMap_;
    IOMemoryMap *dspWritableMap_;
    IOMemoryMap *hdaWritableMap_;
    sof::ipc4::kernel::HdaControllerStatusSnapshot hdaSnapshot_;
    sof::ipc4::kernel::HdaControllerStatusSnapshot postResetSnapshot_;
    sof::ipc4::kernel::HdaInputStreamAllocationPlan allocationPlan_;
    sof::ipc4::kernel::HdaCaptureResetSequence inputResetSequence_;
    sof::ipc4::kernel::HdaPostResetInputStreamPlan postResetInputPlan_;
    sof::ipc4::kernel::HdaInputStreamReservationSequence
        inputReservationSequence_;
    sof::ipc4::kernel::HdaCaptureSetupPlan captureSetupPlan_;
    sof::ipc4::kernel::HdaCaptureSetupSequence captureSetupSequence_;
    sof::ipc4::kernel::HdaDmicCapturePlan capturePlan_;
    sof::ipc4::kernel::Cavs25CodeLoaderStreamReservationPlan codeLoaderPlan_;
    sof::ipc4::kernel::CodeLoaderStreamPlanResult codeLoaderPlanResult_;
    sof::ipc4::kernel::Cavs25CodeLoaderReservationSequence
        codeLoaderReservationSequence_;
    sof::ipc4::kernel::Cavs25CodeLoaderResetSequence codeLoaderResetSequence_;
    sof::ipc4::kernel::Cavs25CodeLoaderSetupPlan codeLoaderSetupPlan_;
    sof::ipc4::kernel::Cavs25CodeLoaderSetupSequence codeLoaderSetupSequence_;
    sof::ipc4::kernel::Cavs25CodeLoaderDmaStopSequence
        codeLoaderDmaStopSequence_;
    sof::ipc4::kernel::Cavs25CodeLoaderPostDmaResetSequence
        codeLoaderPostDmaResetSequence_;
    sof::ipc4::kernel::Cavs25RomCoreStartSequence romCoreStartSequence_;
    sof::ipc4::kernel::Cavs25RomPurgePlan romPurgePlan_;
    sof::ipc4::kernel::Cavs25RomStatusPlan romStatusPlan_;
    sof::ipc4::kernel::FirmwareManifestView firmwareManifest_;
    sof::ipc4::kernel::Cavs25MappedDma dmaResources_;
    AppleHDAStreamLease loaderStreamLease_;
    sof::ipc4::kernel::Cavs25DmaPlan dmaPlan_;
    sof::ipc4::kernel::Cavs25BootLifecycle bootLifecycle_;
    sof::ipc4::kernel::IoOps ipcIoOps_;
    sof::ipc4::kernel::Ipc4MmioAdapter ipcAdapter_;
    sof::ipc4::kernel::Ipc4Transport ipcTransport_;
    sof::ipc4::kernel::Message traceMessage_;
    sof::ipc4::kernel::Reply traceReply_;
    sof::ipc4::dmic::kernel::DmicSharedCaptureRing captureRing_;
    sof::ipc4::kernel::HdaCaptureMappedDma captureDma_;
    AppleHDAStreamLease captureStreamLease_;
    sof::ipc4::kernel::HdaCaptureMmioExecutor captureMmio_;
    uint32_t captureStreamControlMask_;
    uint32_t captureStreamControlValue_;
    sof::ipc4::kernel::HdaCapturePositionTracker capturePositionTracker_;
    sof::ipc4::kernel::HdaCaptureIocCounter captureIocCounter_;
    sof::ipc4::dmic::kernel::DmicCapturePeriodPublisher capturePublisher_;
    sof::ipc4::dmic::kernel::RuntimeIdAllocator captureIdAllocator_;
    sof::ipc4::dmic::kernel::Session captureSession_;
    sof::ipc4::dmic::kernel::CaptureRuntime captureRuntime_;
    sof::ipc4::dmic::kernel::CapturePeriodPump capturePeriodPump_;
    sof::ipc4::dmic::kernel::DmicCaptureInterruptService
        captureInterruptService_;
    sof::ipc4::dmic::kernel::DmicCaptureInterruptBinding *
        captureInterruptBinding_;
    IOLock *lifecycleLock_;
    bool dmaBuffersPrepared_;
    bool captureDmaPrepared_;
    bool captureTrackingPrepared_;
    bool postResetReservationReady_;
    bool captureSetupPlanReady_;
    bool capturePlanReady_;
    bool codeLoaderSetupPrepared_;
    bool romInitDonePrepared_;
    bool firmwareLoaderStoppedPrepared_;
    bool ipcTransportConfigured_;
    bool traceAttempted_;
    bool linkStreamPrepared_;
    bool linkStreamRunning_;
    uint32_t linkStreamOffset_;
    uint32_t captureRingBytes_;
    bool captureRoutingOwned_;
    bool captureFirstFramesPublished_;
    bool fwReadyNotificationSeen_;
    bool providerOpen_;
    uint16_t originalPciCommand_;
    bool pciCommandChanged_;
    bool quarantined_;
    bool userClientOpen_;
};

class HPIPC4AudioUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(HPIPC4AudioUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;

private:
    static IOReturn startCaptureAction(OSObject *target, void *reference,
                                       IOExternalMethodArguments *arguments);
    static IOReturn stopCaptureAction(OSObject *target, void *reference,
                                      IOExternalMethodArguments *arguments);
    static IOReturn getInfoAction(OSObject *target, void *reference,
                                  IOExternalMethodArguments *arguments);
    static const IOExternalMethodDispatch methods_[
        kDmicAudioBridgeSelectorCount];

    AppleHDADmicBridge *controller_;
    bool leaseHeld_;
};

OSDefineMetaClassAndStructors(AppleHDADmicBridge, IOService)
OSDefineMetaClassAndStructors(HPIPC4AudioUserClient, IOUserClient)

bool AppleHDADmicBridge::init(OSDictionary *dictionary) {
    if (!IOService::init(dictionary)) return false;
    pci_ = nullptr;
    appleHdaController_ = nullptr;
    hdaMap_ = nullptr;
    dspMap_ = nullptr;
    dspWritableMap_ = nullptr;
    hdaWritableMap_ = nullptr;
    hdaSnapshot_ = {};
    postResetSnapshot_ = {};
    allocationPlan_ = {};
    postResetInputPlan_ = {};
    captureSetupPlan_ = {};
    capturePlan_ = {};
    codeLoaderPlan_ = {};
    codeLoaderPlanResult_ =
        sof::ipc4::kernel::CodeLoaderStreamPlanResult::NoOutputDescriptor;
    codeLoaderReservationSequence_ = {};
    codeLoaderResetSequence_ = {};
    codeLoaderSetupPlan_ = {};
    codeLoaderSetupSequence_ = {};
    codeLoaderDmaStopSequence_ = {};
    codeLoaderPostDmaResetSequence_ = {};
    romCoreStartSequence_ = {};
    romPurgePlan_ = {};
    romStatusPlan_ = {};
    firmwareManifest_ = {};
    dmaPlan_ = {};
    captureInterruptBinding_ = nullptr;
    lifecycleLock_ = IOLockAlloc();
    if (!lifecycleLock_) return false;
    dmaBuffersPrepared_ = false;
    captureDmaPrepared_ = false;
    captureStreamControlMask_ = 0;
    captureStreamControlValue_ = UINT32_MAX;
    captureTrackingPrepared_ = false;
    postResetReservationReady_ = false;
    captureSetupPlanReady_ = false;
    capturePlanReady_ = false;
    codeLoaderSetupPrepared_ = false;
    romInitDonePrepared_ = false;
    firmwareLoaderStoppedPrepared_ = false;
    ipcIoOps_ = {};
    ipcTransportConfigured_ = false;
    traceAttempted_ = false;
    linkStreamPrepared_ = false;
    linkStreamRunning_ = false;
    linkStreamOffset_ = 0;
    captureRingBytes_ = 0;
    captureRoutingOwned_ = false;
    captureFirstFramesPublished_ = false;
    fwReadyNotificationSeen_ = false;
    providerOpen_ = false;
    originalPciCommand_ = 0;
    pciCommandChanged_ = false;
    __atomic_store_n(&quarantined_, false, __ATOMIC_RELAXED);
    __atomic_store_n(&userClientOpen_, false, __ATOMIC_RELAXED);
    return true;
}

void AppleHDADmicBridge::free() {
    if (lifecycleLock_) {
        IOLockFree(lifecycleLock_);
        lifecycleLock_ = nullptr;
    }
    IOService::free();
}

bool AppleHDADmicBridge::preparePostResetReservation() noexcept {
    if (postResetReservationReady_ || !pci_ || !hdaMap_ ||
        !controllerProviderReady() ||
        ownerServicePresent(pci_) ||
        !inputResetSequence_.complete())
        return false;

    sof::ipc4::kernel::HdaControllerStatusReader reader(
        pci_, this, hdaMap_,
        kAppleHdaOnlyMode && controllerProviderReady());
    if (!reader.readSnapshot(&postResetSnapshot_)) return false;
    if (sof::ipc4::kernel::planHdaInputStreamAfterReset(
            postResetSnapshot_.globalCapabilities,
            postResetSnapshot_.inputStreams,
            postResetSnapshot_.inputStreamCount,
            postResetSnapshot_.allocatedStreamTagMask,
            inputResetSequence_, hdaMap_->getLength(), true,
            &postResetInputPlan_) !=
        sof::ipc4::kernel::HdaPostResetPlanResult::Ok)
        return false;

    // Rebase IOC and LPIB accounting only from the post-reset snapshot. This
    // starts a new capture epoch after recovery without weakening the normal
    // monotonic rebase() path used while a stream remains active.
    const uint8_t descriptorIndex =
        postResetInputPlan_.allocation.descriptorIndex;
    const uint32_t streamControlOffset =
        postResetInputPlan_.allocation.descriptorControlOffset;
    if (!captureTrackingPrepared_ ||
        descriptorIndex >= postResetSnapshot_.inputStreamCount ||
        postResetSnapshot_.inputStreams[descriptorIndex].status != 0 ||
        static_cast<uint64_t>(streamControlOffset) + 8U >
            hdaMap_->getLength())
        return false;
    volatile uint8_t *hdaBytes = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t initialLinkPosition =
        *reinterpret_cast<volatile uint32_t *>(
            hdaBytes + streamControlOffset + 4U);
    OSSynchronizeIO();
    if (initialLinkPosition == UINT32_MAX ||
        !captureIocCounter_.resetForStart(0, true, true) ||
        !capturePositionTracker_.resetForStart(
            initialLinkPosition, 0, true, true))
        return false;

    if (inputReservationSequence_.initialize(
            postResetInputPlan_.reservation, hdaMap_->getLength()) !=
        sof::ipc4::kernel::HdaInputStreamReservationSequenceResult::Ready)
        return false;

    allocationPlan_ = postResetInputPlan_.allocation;
    postResetReservationReady_ = true;
    setProperty("HDA.PostResetReservationPlanReady", true);
    setProperty("HDA.InputStreamReserved", false);
    return true;
}

bool AppleHDADmicBridge::prepareCaptureRegisterSetupPlan() noexcept {
    if (captureSetupPlanReady_ || !postResetReservationReady_ ||
        !inputReservationSequence_.complete() || !captureDmaPrepared_ ||
        !pci_ || !controllerProviderReady() ||
        ownerServicePresent(pci_))
        return false;
    const sof::ipc4::kernel::HdaCaptureBdlPlan *bdl =
        captureDma_.bdlPlan();
    if (!bdl || allocationPlan_.descriptorIndex >=
            postResetSnapshot_.inputStreamCount ||
        allocationPlan_.descriptorControlOffset !=
            postResetInputPlan_.allocation.descriptorControlOffset)
        return false;

    const sof::ipc4::kernel::HdaStreamDescriptorSnapshot &snapshot =
        postResetSnapshot_.inputStreams[allocationPlan_.descriptorIndex];
    const uint32_t previousStreamRegisters[6] = {
        snapshot.bdlLow,
        snapshot.bdlHigh,
        snapshot.cyclicBufferLength,
        snapshot.lastValidIndex,
        snapshot.format,
        snapshot.control,
    };
    sof::ipc4::kernel::HdaCaptureRegisterPlan streamPlan{};
    if (sof::ipc4::kernel::planHdaCaptureRegisterProgramming(
            allocationPlan_, *bdl, previousStreamRegisters,
            sof::ipc4::dmic::kernel::kDmicCaptureCandidateHdaFormatWord,
            inputReservationSequence_, hdaMap_->getLength(),
            controllerProviderReady(), &streamPlan) !=
        sof::ipc4::kernel::HdaCaptureRegisterPlanResult::Ok)
        return false;

    sof::ipc4::kernel::HdaCaptureRoutingPlan routingPlan{};
    if (sof::ipc4::kernel::planHdaCaptureRouting(
            allocationPlan_, postResetSnapshot_.ppCapabilityOffset,
            postResetSnapshot_.ppCapabilityHeader,
            postResetSnapshot_.ppctl, postResetSnapshot_.intctl,
            hdaMap_->getLength(), controllerProviderReady(),
            &routingPlan) !=
        sof::ipc4::kernel::HdaCaptureRegisterPlanResult::Ok ||
        sof::ipc4::kernel::combineHdaCaptureSetupPlans(
            streamPlan, routingPlan, hdaMap_->getLength(),
            &captureSetupPlan_) !=
        sof::ipc4::kernel::HdaCaptureRegisterPlanResult::Ok ||
        captureSetupSequence_.initialize(
            captureSetupPlan_, hdaMap_->getLength()) !=
        sof::ipc4::kernel::HdaCaptureSetupSequenceResult::WriteRequired)
        return false;

    const sof::ipc4::kernel::HdaInputStreamReservationPlan *reservation =
        inputReservationSequence_.plan();
    if (!reservation) return false;
    const uint32_t confirmedControl =
        inputReservationSequence_.confirmedControlReadback();
    if (static_cast<uint64_t>(reservation->descriptorControlOffset) +
            sizeof(uint32_t) > hdaMap_->getLength())
        return false;
    volatile uint8_t *hdaBytes = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t currentControl = *reinterpret_cast<volatile uint32_t *>(
        hdaBytes + reservation->descriptorControlOffset);
    OSSynchronizeIO();
    const bool controlStillReserved = currentControl != UINT32_MAX &&
        confirmedControl != UINT32_MAX &&
        (currentControl & reservation->expectedReadbackMask) ==
            reservation->expectedReadbackValue &&
        (confirmedControl & reservation->expectedReadbackMask) ==
            reservation->expectedReadbackValue;
    const sof::ipc4::kernel::HdaCaptureStreamEvidence streamEvidence = {
        postResetInputPlan_.allocation.inputStreamCount,
        postResetInputPlan_.allocation.descriptorIndex,
        postResetInputPlan_.allocation.streamTag,
        controllerProviderReady(),
        inputReservationSequence_.complete(),
        controlStillReserved,
    };
    if (sof::ipc4::kernel::planHdaDmicCapture(
            streamEvidence, &capturePlan_) !=
        sof::ipc4::kernel::HdaCapturePlanResult::Ok)
        return false;

    captureSetupPlanReady_ = true;
    capturePlanReady_ = true;
    setProperty("HDA.CaptureSetupPlanReady", true);
    setProperty("IPC4.CaptureIdentityPlanReady", true);
    setProperty("IPC4.CaptureStreamTag", capturePlan_.streamId, 32);
    return true;
}

bool AppleHDADmicBridge::prepareInputCaptureHda() noexcept {
    if (!bootLifecycle_.canIssueIpc4() || !pci_ || !hdaMap_ ||
        !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        return false;
    if (kAppleHdaOnlyMode) {
        setProperty("Capture.InputStage", "AppleHDA input stream lease");
        // The last activation argument selects descending tag allocation.
        // SOF's host capture channel is tag - 1 and has only GCAP.ISS entries.
        if (!captureStreamLease_.allocate(appleHdaController_) ||
            captureStreamLease_.configurePcm(
                0, 0, 48000, false, 32, 2) != kIOReturnSuccess ||
            captureStreamLease_.activate(1, 2, false) != kIOReturnSuccess)
            return false;
        uint8_t descriptorIndex = UINT8_MAX;
        uint8_t streamTag = 0;
        if (!captureStreamLease_.activeDescriptor(
                hdaSnapshot_.inputStreamCount,
                hdaSnapshot_.outputStreamCount,
                hdaSnapshot_.totalStreamCount,
                &descriptorIndex, &streamTag) ||
            descriptorIndex >= hdaSnapshot_.inputStreamCount)
            return false;
        setProperty("Capture.AppleHDAStreamTag", streamTag, 8);
        if (streamTag > hdaSnapshot_.inputStreamCount ||
            (hdaSnapshot_.allocatedStreamTagMask & (1U << streamTag)) != 0) {
            setProperty("Capture.InputStage", "capture tag outside available SOF channels");
            return false;
        }
        const auto &snapshot =
            hdaSnapshot_.inputStreams[descriptorIndex];
        if ((snapshot.control &
                (sof::ipc4::kernel::kHdaStreamRunMask |
                 sof::ipc4::kernel::kHdaStreamResetMask |
                 sof::ipc4::kernel::kHdaStreamTagMask)) != 0 ||
            snapshot.status != 0 || snapshot.cyclicBufferLength != 0 ||
            snapshot.bdlLow != 0 || snapshot.bdlHigh != 0 ||
            snapshot.lastValidIndex != 0 || snapshot.format != 0)
            return false;

        auto *cyclicBuffer = OSDynamicCast(
            IOBufferMemoryDescriptor, captureStreamLease_.cyclicBuffer());
        const uint64_t cyclicBytes = cyclicBuffer
            ? cyclicBuffer->getLength() : 0;
        const uint32_t periodBytes =
            sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodBytes;
        if (!cyclicBuffer || cyclicBytes > UINT32_MAX ||
            cyclicBytes < periodBytes * 2U ||
            (cyclicBytes % periodBytes) != 0 ||
            !captureDma_.prepareWithAppleHdaBuffer(
                pci_, this, cyclicBuffer,
                static_cast<uint32_t>(cyclicBytes), periodBytes,
                kAppleHdaOnlyMode && controllerProviderReady()))
            return false;
        captureDmaPrepared_ = true;
        captureRingBytes_ = static_cast<uint32_t>(cyclicBytes);
        allocationPlan_.inputStreamCount = hdaSnapshot_.inputStreamCount;
        allocationPlan_.descriptorIndex = descriptorIndex;
        allocationPlan_.streamTag = streamTag;
        allocationPlan_.reserved = 0;
        allocationPlan_.descriptorControlOffset =
            0x80U + static_cast<uint32_t>(descriptorIndex) * 0x20U;
        allocationPlan_.allocatedStreamTagMask =
            static_cast<uint16_t>(hdaSnapshot_.allocatedStreamTagMask |
                                  (1U << streamTag));
        allocationPlan_.reserved2 = 0;
        uint32_t initialPosition = UINT32_MAX;
        if (!captureStreamLease_.linkPositionBytes(&initialPosition) ||
            initialPosition > captureRingBytes_ ||
            !captureIocCounter_.initialize(
                allocationPlan_.descriptorControlOffset, 0) ||
            !capturePositionTracker_.initialize(
                captureRingBytes_, periodBytes, initialPosition, 0,
                captureRingBytes_ - 2U * periodBytes))
            return false;
        captureTrackingPrepared_ = true;

        sof::ipc4::kernel::HdaCaptureStreamEvidence streamEvidence = {
            hdaSnapshot_.inputStreamCount, descriptorIndex, streamTag,
            true, true, true};
        if (sof::ipc4::kernel::planHdaDmicCapture(
                streamEvidence, &capturePlan_) !=
            sof::ipc4::kernel::HdaCapturePlanResult::Ok)
            return false;
        captureSetupPlan_ = {};
        captureSetupPlan_.streamControlOffset =
            allocationPlan_.descriptorControlOffset;
        captureSetupPlanReady_ = true;
        capturePlanReady_ = true;
        postResetReservationReady_ = true;
        setProperty("Capture.AppleHDADescriptor", descriptorIndex, 8);
        setProperty("Capture.AppleHDACyclicBytes", captureRingBytes_, 32);
        setProperty("Capture.AppleHDAPeriodBytes", periodBytes, 32);
        setProperty("HDA.CaptureSetupPlanReady", true);
        setProperty("IPC4.CaptureIdentityPlanReady", true);
        setProperty("IPC4.CaptureStreamTag", capturePlan_.streamId, 32);
        return true;
    }
    if (!captureDmaPrepared_) return false;
    if (!hdaWritableMap_) {
        hdaWritableMap_ = pci_->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    }
    if (!hdaWritableMap_ || hdaWritableMap_->getVirtualAddress() == 0 ||
        hdaWritableMap_->getLength() < kHdaBarMinimum ||
        hdaWritableMap_->getLength() != hdaMap_->getLength() ||
        (hdaWritableMap_->getMapOptions() & kIOMapReadOnly) != 0)
        return false;

    sof::ipc4::kernel::HdaCaptureMmioExecutor executor(
        pci_, this, hdaWritableMap_);
    if (!inputResetSequence_.complete()) {
        bool resetComplete = false;
        constexpr uint32_t kResetPollLimit = 2000;
        for (uint32_t poll = 0; poll < kResetPollLimit; ++poll) {
            const auto result = executor.advanceReset(&inputResetSequence_);
            setProperty("Capture.InputResetResult", static_cast<uint32_t>(result), 32);
            setProperty("Capture.InputResetPollCount", poll + 1U, 32);
            if (result == sof::ipc4::kernel::HdaStreamResetResult::Complete) {
                resetComplete = true;
                break;
            }
            if (result != sof::ipc4::kernel::HdaStreamResetResult::Waiting)
                return false;
            IODelay(100);
        }
        if (!resetComplete) return false;
    }

    setProperty("Capture.InputStage", "post-reset reservation plan");
    if (!postResetReservationReady_ && !preparePostResetReservation())
        return false;
    setProperty("Capture.InputStage", "stream reservation");
    if (!inputReservationSequence_.complete()) {
        const auto result = executor.advanceReservation(
            &inputReservationSequence_);
        if (result !=
                sof::ipc4::kernel::HdaInputStreamReservationSequenceResult::Complete &&
            result !=
                sof::ipc4::kernel::HdaInputStreamReservationSequenceResult::WriteRequired)
            return false;
        if (!inputReservationSequence_.complete()) {
            const auto confirmation = executor.advanceReservation(
                &inputReservationSequence_);
            if (confirmation != sof::ipc4::kernel::
                    HdaInputStreamReservationSequenceResult::Complete)
                return false;
        }
    }

    setProperty("Capture.InputStage", "capture setup plan");
    return captureSetupPlanReady_ || prepareCaptureRegisterSetupPlan();
}

void AppleHDADmicBridge::auditMultiLink(bool afterRun) noexcept {
    if (!controllerProviderReady() || !hdaMap_ || !hdaMap_->getVirtualAddress()) return;
    const uint64_t length = hdaMap_->getLength();
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(hdaMap_->getVirtualAddress());
    if (length < 0x18U) return;
    OSSynchronizeIO(); uint32_t off = *reinterpret_cast<const volatile uint16_t *>(base + 0x14U); OSSynchronizeIO();
    for (uint32_t n=0; n<10 && off; ++n) {
        if ((off & 3U) || static_cast<uint64_t>(off) + 8U > length) return;
        OSSynchronizeIO(); const uint32_t hdr=*reinterpret_cast<const volatile uint32_t *>(base+off); OSSynchronizeIO();
        if (hdr==UINT32_MAX) return;
        if (((hdr>>16)&0xfffU)==2U) {
            OSSynchronizeIO(); const uint32_t rawCount=*reinterpret_cast<const volatile uint32_t *>(base+off+4U); OSSynchronizeIO();
            setProperty("Capture.MultiLinkCapabilityOffset", off, 32);
            setProperty("Capture.MultiLinkRawCount", rawCount, 32);
            if (rawCount==UINT32_MAX || rawCount>=16) return;
            for (uint32_t j=0;j<=rawCount;++j) {
                const uint64_t link=static_cast<uint64_t>(off)+0x40U+0x40U*j;
                if (link+0x24U>length) return;
                OSSynchronizeIO();
                const uint32_t cap=*reinterpret_cast<const volatile uint32_t *>(base+link);
                const uint32_t ctl=*reinterpret_cast<const volatile uint32_t *>(base+link+4U);
                const uint32_t ptr=*reinterpret_cast<const volatile uint32_t *>(base+link+0x20U);
                OSSynchronizeIO();
                char key[64];
                snprintf(key,sizeof(key),"Capture.Ml%uCapability",j); setProperty(key,cap,32);
                snprintf(key,sizeof(key),"Capture.Ml%uLinkPointer",j); setProperty(key,ptr,32);
                snprintf(key,sizeof(key),afterRun ? "Capture.Ml%uControlAfter" : "Capture.Ml%uControlBefore",j); setProperty(key,ctl,32);
            }
            return;
        }
        off=hdr&0xffffU;
    }
    setProperty("Capture.MultiLinkCapabilityFound", false);
}

bool AppleHDADmicBridge::prepareLinkStream() noexcept {
    if (kAppleHdaOnlyMode)
        return captureStreamLease_.active() && capturePlanReady_;
    if (linkStreamPrepared_) return true;
    if (!pci_ || !controllerProviderReady() || ownerServicePresent(pci_) ||
        !hdaWritableMap_ || !capturePlanReady_ || !inputReservationSequence_.complete()) return false;
    const uint32_t count = postResetSnapshot_.totalStreamCount;
    const uint32_t index = postResetInputPlan_.allocation.descriptorIndex;
    const uint32_t tag = capturePlan_.streamId;
    if (!count || count > sof::ipc4::kernel::kHdaMaximumTotalStreamCount || index >= count || !tag || tag > 15) return false;
    const uint64_t offset = static_cast<uint64_t>(postResetSnapshot_.ppCapabilityOffset) + 0x10U + 0x10U * count + 0x10U * index;
    if (offset + 0x10U > hdaWritableMap_->getLength() || (offset & 3U) != 0) return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(hdaWritableMap_->getVirtualAddress());
    if (!base) return false;
    volatile uint32_t *ctl = reinterpret_cast<volatile uint32_t *>(base + offset);
    volatile uint16_t *fmt = reinterpret_cast<volatile uint16_t *>(base + offset + 4U);
    OSSynchronizeIO(); uint32_t value = *ctl; OSSynchronizeIO();
    setProperty("Capture.LinkOffset", static_cast<uint32_t>(offset), 32);
    setProperty("Capture.LinkInitialControl", value, 32);
    if (value == UINT32_MAX || (value & ((0xfU << 20) | 3U)) != 0) return false;
    *ctl = value | 1U; OSSynchronizeIO();
    bool asserted = false;
    for (uint32_t i=0; i<1000; ++i) { value=*ctl; OSSynchronizeIO(); if (value==UINT32_MAX) return false; if (value&1U) { asserted=true; break; } IODelay(10); }
    if (!asserted) return false;
    *ctl = value & ~1U; OSSynchronizeIO();
    bool deasserted = false;
    for (uint32_t i=0; i<1000; ++i) { value=*ctl; OSSynchronizeIO(); if (value==UINT32_MAX) return false; if (!(value&1U)) { deasserted=true; break; } IODelay(10); }
    if (!deasserted || (value & 2U)) return false;
    *ctl = (value & ~(0xfU << 20)) | (tag << 20); OSSynchronizeIO();
    *fmt = sof::ipc4::dmic::kernel::kDmicCaptureCandidateHdaFormatWord; OSSynchronizeIO();
    value=*ctl; const uint16_t format=*fmt; OSSynchronizeIO();
    setProperty("Capture.LinkPreparedControl", value, 32);
    setProperty("Capture.LinkPreparedFormat", format, 16);
    if (value==UINT32_MAX || (value & ((0xfU << 20)|3U)) != (tag << 20) ||
        format != sof::ipc4::dmic::kernel::kDmicCaptureCandidateHdaFormatWord) return false;
    linkStreamOffset_=static_cast<uint32_t>(offset); linkStreamPrepared_=true;
    return true;
}

bool AppleHDADmicBridge::setLinkStreamRunning(bool running) noexcept {
    if (!linkStreamPrepared_ || !hdaWritableMap_ || !controllerProviderReady() ||
        linkStreamOffset_ + 4U > hdaWritableMap_->getLength()) return false;
    volatile uint32_t *ctl = reinterpret_cast<volatile uint32_t *>(hdaWritableMap_->getVirtualAddress() + linkStreamOffset_);
    OSSynchronizeIO(); uint32_t value=*ctl; OSSynchronizeIO();
    const uint32_t tag=static_cast<uint32_t>(capturePlan_.streamId)<<20;
    if (value==UINT32_MAX || (value & ((0xfU<<20)|1U)) != tag) return false;
    if (((value & 2U)!=0) != running) { *ctl = running ? (value | 2U) : (value & ~2U); OSSynchronizeIO(); }
    for (uint32_t i=0;i<1000;++i) {
        value=*ctl; OSSynchronizeIO();
        if (value==UINT32_MAX || (value & ((0xfU<<20)|1U)) != tag) return false;
        if (((value & 2U)!=0)==running) {
            linkStreamRunning_=running; setProperty("Capture.LinkControl", value, 32);
            setProperty("Capture.LinkRunning", running); return true;
        }
        IODelay(10);
    }
    setProperty("Capture.LinkControl", value, 32); return false;
}

bool AppleHDADmicBridge::programCaptureHdaSetup() noexcept {
    if (kAppleHdaOnlyMode) return captureSetupPlanReady_ &&
        captureStreamLease_.active();
    if (!captureSetupPlanReady_ || !hdaWritableMap_ || !pci_ ||
        !controllerProviderReady() || ownerServicePresent(pci_))
        return false;
    sof::ipc4::kernel::HdaCaptureMmioExecutor executor(
        pci_, this, hdaWritableMap_);
    if (!captureSetupSequence_.complete()) {
        bool setupComplete = false;
        for (uint8_t step = 0; step < captureSetupPlan_.writeCount; ++step) {
            const auto result = executor.advanceSetup(&captureSetupSequence_);
            if (result ==
                sof::ipc4::kernel::HdaCaptureSetupSequenceResult::Complete) {
                setupComplete = true;
                break;
            }
            if (result !=
                sof::ipc4::kernel::HdaCaptureSetupSequenceResult::Matched)
                return false;
        }
        if (!setupComplete || !captureSetupSequence_.complete()) return false;
    }
    setProperty("HDA.CaptureSetupComplete", true);
    return true;
}

bool AppleHDADmicBridge::enableFirmwareTrace() noexcept {
    if (traceAttempted_) return true;
    traceAttempted_ = true;
    if (!ipcTransportConfigured_ || !bootLifecycle_.canIssueIpc4()) return false;
    uint32_t config[19] = {};
    config[0] = 0x100U; config[1] = 0x1000U; config[2] = 1U;
    config[3] = 0x1efU;
    if (sof::ipc4::kernel::largeConfigSet(0, 0, 6,
            reinterpret_cast<const uint8_t *>(config), sizeof(config), &traceMessage_) !=
        sof::ipc4::kernel::Result::Ok) return false;
    const auto result = ipcTransport_.send(&traceMessage_, 0, nullptr, &traceReply_,
                                           kIpcTimeoutMilliseconds, kIpcPollIntervalMicroseconds);
    setProperty("Trace.EnableTransportResult", static_cast<uint32_t>(result), 32);
    setProperty("Trace.EnableFirmwareStatus", traceReply_.firmwareStatus, 32);
    setProperty("Trace.Enabled", result == sof::ipc4::kernel::TransportResult::Ok);
    return result == sof::ipc4::kernel::TransportResult::Ok ||
           result == sof::ipc4::kernel::TransportResult::FirmwareRejected;
}

void AppleHDADmicBridge::snapshotFirmwareTrace() noexcept {
    constexpr uint32_t window = 0xc0000U, page = 0x1000U;
    if (!dspMap_ || !dspMap_->getVirtualAddress() || dspMap_->getLength() < window + 16U*page) return;
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(dspMap_->getVirtualAddress());
    uint8_t *copy = static_cast<uint8_t *>(IOMalloc(page));
    if (!copy) return;
    uint32_t found=0;
    for (uint32_t i=0; i<16 && found<4; ++i) {
        OSSynchronizeIO();
        const uint32_t type=*reinterpret_cast<const volatile uint32_t *>(base+window+i*12U+4U);
        OSSynchronizeIO();
        const uint32_t kind=type&0xffffff00U;
        if (kind!=0x474f4c00U && kind!=0x54524300U) continue;
        const uint32_t size = i==15 ? page-1024U : page;
        const uint32_t start = window + (i==15 ? 1024U : (i+1U)*page);
        if (static_cast<uint64_t>(start)+size>dspMap_->getLength()) continue;
        OSSynchronizeIO(); for (uint32_t j=0;j<size;++j) copy[j]=base[start+j]; OSSynchronizeIO();
        char key[48]; snprintf(key,sizeof(key),"Trace.Slot%uType",found); setProperty(key,type,32);
        snprintf(key,sizeof(key),"Trace.Slot%uRaw",found); setProperty(key,copy,size); ++found;
    }
    setProperty("Trace.SlotsCaptured",found,32);
    IOFree(copy,page);
}

void AppleHDADmicBridge::drainFirmwareTrace() noexcept {
    // IPC4 cAVS 2 debug slots start with host_read_ptr and dsp_write_ptr,
    // followed by a 4088-byte circular log buffer. Linux advances the first
    // pointer after copying available bytes. Discard the pre-pipeline boot and
    // topology logs here so the next snapshot contains the pipeline result.
    constexpr uint32_t debugBox = 0xc0000U;
    constexpr uint32_t slotBytes = 0x1000U;
    constexpr uint32_t slotData = debugBox + slotBytes;
    constexpr uint32_t logBytes = slotBytes - 2U * sizeof(uint32_t);
    if (!dspMap_ || !dspWritableMap_ ||
        !dspMap_->getVirtualAddress() || !dspWritableMap_->getVirtualAddress() ||
        dspMap_->getLength() < slotData + slotBytes ||
        dspWritableMap_->getLength() < slotData + slotBytes) {
        setProperty("Trace.DrainResult", "mapping unavailable");
        return;
    }

    auto *readOnly = reinterpret_cast<const volatile uint8_t *>(
        dspMap_->getVirtualAddress());
    auto *writable = reinterpret_cast<volatile uint8_t *>(
        dspWritableMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t type = *reinterpret_cast<const volatile uint32_t *>(
        readOnly + debugBox + sizeof(uint32_t));
    const uint32_t writePointerRaw = *reinterpret_cast<const volatile uint32_t *>(
        readOnly + slotData + sizeof(uint32_t));
    const uint32_t readPointer = *reinterpret_cast<const volatile uint32_t *>(
        readOnly + slotData);
    OSSynchronizeIO();
    setProperty("Trace.DrainSlotType", type, 32);
    setProperty("Trace.DrainReadBefore", readPointer, 32);
    setProperty("Trace.DrainWritePointerRaw", writePointerRaw, 32);

    if ((type & 0xffffff00U) != 0x474f4c00U ||
        readPointer >= logBytes || writePointerRaw >= logBytes) {
        setProperty("Trace.DrainResult", "slot or pointer invalid");
        return;
    }

    // Linux rounds the DSP pointer down to a 32-bit boundary before consuming
    // bytes. Keep that alignment and follow any deferred trace flushes until
    // the writer has been quiet long enough for the RUN request to be next.
    uint32_t target = writePointerRaw & ~3U;
    uint32_t stable = 0;
    uint32_t updates = 0;
    uint32_t rounds = 0;
    for (; rounds < 250U && stable < 10U; ++rounds) {
        *reinterpret_cast<volatile uint32_t *>(writable + slotData) = target;
        OSSynchronizeIO();
        const uint32_t readback = *reinterpret_cast<const volatile uint32_t *>(
            readOnly + slotData);
        const uint32_t nextRaw = *reinterpret_cast<const volatile uint32_t *>(
            readOnly + slotData + sizeof(uint32_t));
        OSSynchronizeIO();
        if (readback != target || nextRaw >= logBytes) break;
        const uint32_t next = nextRaw & ~3U;
        if (next != target) {
            target = next;
            ++updates;
            stable = 0;
        } else {
            ++stable;
        }
        IODelay(1000);
    }
    setProperty("Trace.DrainWritePointerAligned", target, 32);
    setProperty("Trace.DrainReadback", *reinterpret_cast<const volatile uint32_t *>(
                    readOnly + slotData), 32);
    setProperty("Trace.DrainUpdates", updates, 32);
    setProperty("Trace.DrainRounds", rounds, 32);
    setProperty("Trace.DrainResult",
                stable >= 10U ? "pre-pipeline trace backlog drained"
                              : "pre-pipeline trace did not settle");
}

void AppleHDADmicBridge::settleFirmwareTrace() noexcept {
    constexpr uint32_t slotData = 0xc1000U;
    constexpr uint32_t logBytes = 0x1000U - 2U * sizeof(uint32_t);
    if (!dspMap_ || !dspMap_->getVirtualAddress() ||
        dspMap_->getLength() < slotData + 0x1000U) return;
    auto *readOnly = reinterpret_cast<const volatile uint8_t *>(
        dspMap_->getVirtualAddress());
    IODelay(20000);
    uint32_t last = *reinterpret_cast<const volatile uint32_t *>(
        readOnly + slotData + sizeof(uint32_t)) & ~3U;
    uint32_t stable = 0;
    uint32_t rounds = 0;
    for (; rounds < 200U && stable < 10U; ++rounds) {
        IODelay(1000);
        OSSynchronizeIO();
        const uint32_t raw = *reinterpret_cast<const volatile uint32_t *>(
            readOnly + slotData + sizeof(uint32_t));
        if (raw >= logBytes) break;
        const uint32_t current = raw & ~3U;
        if (current == last) ++stable;
        else { last = current; stable = 0; }
    }
    setProperty("Trace.PostRunWritePointer", last, 32);
    setProperty("Trace.PostRunStableRounds", stable, 32);
    setProperty("Trace.PostRunWaitRounds", rounds, 32);
}

bool AppleHDADmicBridge::prepareFirmwareReadyTransport() noexcept {
    if (!bootLifecycle_.canIssueIpc4() || !pci_ || !dspMap_ ||
        !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        return false;
    if (!dspWritableMap_) {
        dspWritableMap_ = pci_->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress4, kIOMapInhibitCache);
    }
    if (!dspWritableMap_ || dspWritableMap_->getVirtualAddress() == 0 ||
        dspWritableMap_->getLength() < kDspBarMinimum ||
        dspWritableMap_->getLength() != dspMap_->getLength() ||
        (dspWritableMap_->getMapOptions() & kIOMapReadOnly) != 0)
        return false;

    if (!ipcAdapter_.configure(
            pci_, this, dspWritableMap_, &bootLifecycle_, true,
            kAppleHdaOnlyMode ? appleHdaController_ : nullptr,
            &AppleHDADmicBridge::handleIpc4Notification, this))
        return false;
    ipcIoOps_ = ipcAdapter_.ioOps();
    if (!ipcTransport_.configure(
            &ipcIoOps_, sof::ipc4::kernel::kMaximumPayloadBytes,
            sof::ipc4::kernel::kMaximumPayloadBytes) ||
        !ipcTransport_.confirmFirmwareReady(&bootLifecycle_))
        return false;
    ipcTransportConfigured_ = true;
    return true;
}

bool AppleHDADmicBridge::prepareCodeLoaderStream() noexcept {
    using sof::ipc4::kernel::BootPhase;
    using sof::ipc4::kernel::CodeLoaderResetResult;
    using sof::ipc4::kernel::CodeLoaderSetupSequenceResult;
    using sof::ipc4::kernel::LoaderPrepareEvidence;
    using sof::ipc4::kernel::Cavs25CodeLoaderRegisterSnapshot;
    using sof::ipc4::kernel::Cavs25CodeLoaderMmioExecutor;
    const char *failureGate = "code-loader initial preflight";
    const auto markFailureGate = [this, &failureGate](const char *gate) noexcept {
        failureGate = gate;
        setProperty("Boot.StartFailureGate", gate);
    };
    markFailureGate(failureGate);
    if (codeLoaderSetupPrepared_) {
        const bool prepared = bootLifecycle_.phase() == BootPhase::LoaderPrepared;
        if (!prepared)
            markFailureGate("loader already prepared but phase mismatch");
        return prepared;
    }
    const bool phaseReady =
        bootLifecycle_.phase() == BootPhase::DmaResourcesReady;
    const bool planReady = codeLoaderPlanResult_ ==
        sof::ipc4::kernel::CodeLoaderStreamPlanResult::Ok;
    const bool providerReady = pci_ && hdaMap_ && controllerProviderReady();
    const bool audioOwnerAbsent = pci_ && !ownerServicePresent(pci_);
    setProperty("Boot.CodeLoaderPreflightPhaseReady", phaseReady);
    setProperty("Boot.CodeLoaderPreflightPlanReady", planReady);
    setProperty("Boot.CodeLoaderPreflightProviderReady", providerReady);
    setProperty("Boot.CodeLoaderPreflightAudioOwnerAbsent", audioOwnerAbsent);
    setProperty("Boot.CodeLoaderPreflightDmaBuffersPrepared",
                dmaBuffersPrepared_);
    if (!phaseReady || !planReady || !providerReady ||
        (!kAppleHdaOnlyMode && !audioOwnerAbsent) ||
        !dmaBuffersPrepared_)
        return false;

    if (kAppleHdaOnlyMode) {
        markFailureGate("AppleHDA-managed output stream validation");
        if (!loaderStreamLease_.active() ||
            !dmaResources_.usesAppleHdaBuffer() ||
            !dmaResources_.appleHdaBufferReady())
            return false;
        uint8_t descriptorIndex = UINT8_MAX;
        uint8_t streamTag = 0;
        if (!loaderStreamLease_.activeDescriptor(
                hdaSnapshot_.inputStreamCount,
                hdaSnapshot_.outputStreamCount,
                hdaSnapshot_.totalStreamCount,
                &descriptorIndex, &streamTag))
            return false;
        IOMemoryDescriptor *memory = loaderStreamLease_.cyclicBuffer();
        const uint64_t cyclicBytes = memory ? memory->getLength() : 0;
        if (!memory || cyclicBytes > UINT32_MAX ||
            cyclicBytes < dmaResources_.firmwarePayloadBytes())
            return false;
        if (descriptorIndex >= hdaSnapshot_.totalStreamCount ||
            descriptorIndex >= sof::ipc4::kernel::kHdaMaximumTotalStreamCount)
            return false;
        const auto &initialStream =
            hdaSnapshot_.allStreamControls[descriptorIndex];
        if ((hdaSnapshot_.allocatedStreamTagMask & (1U << streamTag)) != 0 ||
            (initialStream.control &
                (sof::ipc4::kernel::kHdaStreamRunMask |
                 sof::ipc4::kernel::kHdaStreamResetMask |
                 sof::ipc4::kernel::kHdaStreamTagMask)) != 0 ||
            initialStream.status != 0 ||
            initialStream.cyclicBufferLength != 0 ||
            initialStream.bdlLow != 0 || initialStream.bdlHigh != 0 ||
            initialStream.lastValidIndex != 0 || initialStream.format != 0)
            return false;
        codeLoaderPlan_.descriptorIndex = descriptorIndex;
        codeLoaderPlan_.streamTag = streamTag;
        codeLoaderPlan_.descriptorControlOffset =
            0x80U + static_cast<uint32_t>(descriptorIndex) * 0x20U;
        const auto &snapshot =
            hdaSnapshot_.allStreamControls[descriptorIndex];
        codeLoaderPlan_.previousControl = snapshot.control;
        codeLoaderPlan_.reservationBaseControl = snapshot.control;
        codeLoaderPlan_.controlWriteValue = snapshot.control;
        codeLoaderPlan_.expectedReadbackMask = 0;
        codeLoaderPlan_.expectedReadbackValue = 0;
        codeLoaderPlan_.reserved = 0;
        const auto accepted = bootLifecycle_.acceptAppleHdaLoaderPrepared(
            true, true, descriptorIndex, streamTag,
            static_cast<uint32_t>(cyclicBytes),
            dmaResources_.firmwarePayloadBytes());
        if (accepted != sof::ipc4::kernel::BootResult::Ok) return false;
        codeLoaderSetupPrepared_ = true;
        setProperty("Boot.AppleHDALoaderPrepared", true);
        setProperty("Boot.LifecyclePhase",
                    static_cast<uint32_t>(bootLifecycle_.phase()), 32);
        return true;
    }

    if (!hdaWritableMap_) {
        markFailureGate("writable HDA BAR mapping");
        hdaWritableMap_ = pci_->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    }
    setProperty("Boot.CodeLoaderWritableHdaMapPresent",
                hdaWritableMap_ != nullptr);
    if (hdaWritableMap_)
        setProperty("Boot.CodeLoaderWritableHdaMapLength",
                    hdaWritableMap_->getLength(), 64);
    if (!hdaWritableMap_ || hdaWritableMap_->getVirtualAddress() == 0 ||
        hdaWritableMap_->getLength() < kHdaBarMinimum ||
        hdaWritableMap_->getLength() != hdaMap_->getLength() ||
        (hdaWritableMap_->getMapOptions() & kIOMapReadOnly) != 0) {
        setProperty("Boot.CodeLoaderWritableHdaVirtualAddressPresent",
                    hdaWritableMap_ &&
                        hdaWritableMap_->getVirtualAddress() != 0);
        setProperty("Boot.CodeLoaderWritableHdaMapLengthValid",
                    hdaWritableMap_ &&
                        hdaWritableMap_->getLength() >= kHdaBarMinimum &&
                        hdaMap_ && hdaWritableMap_->getLength() ==
                            hdaMap_->getLength());
        setProperty("Boot.CodeLoaderWritableHdaMapIsReadOnly",
                    hdaWritableMap_ &&
                        (hdaWritableMap_->getMapOptions() &
                            kIOMapReadOnly) != 0);
        return false;
    }

    markFailureGate("initial code-loader descriptor audit");
    Cavs25CodeLoaderMmioExecutor executor(
        pci_, this, hdaWritableMap_, &bootLifecycle_,
        kAppleHdaOnlyMode && controllerProviderReady());
    const auto retainAfterPossibleWrite = [this]() noexcept {
        sof::ipc4::kernel::AbortEvidence uncertain{};
        (void)bootLifecycle_.quarantineFailure(uncertain);
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        setProperty("Boot.LifecyclePhase",
                    static_cast<uint32_t>(bootLifecycle_.phase()), 32);
        setProperty("Status", "code-loader HDA preparation uncertain; resources retained");
        setProperty("Controller.Quarantined", true);
        return false;
    };

    const uint8_t descriptorIndex = codeLoaderPlan_.descriptorIndex;
    if (descriptorIndex >= hdaSnapshot_.totalStreamCount) {
        setProperty("Boot.StartFailureGate", "code-loader descriptor index");
        return false;
    }
    const auto &initialDescriptor =
        hdaSnapshot_.allStreamControls[descriptorIndex];
    const bool initialDescriptorEmpty = initialDescriptor.status == 0 &&
        initialDescriptor.cyclicBufferLength == 0 &&
        initialDescriptor.bdlLow == 0 && initialDescriptor.bdlHigh == 0 &&
        initialDescriptor.lastValidIndex == 0 && initialDescriptor.format == 0;
    setProperty("Boot.CodeLoaderInitialDescriptorEmpty", initialDescriptorEmpty);
    const bool resetSequenceReady = initialDescriptorEmpty &&
        codeLoaderResetSequence_.initialize(
            codeLoaderPlan_, initialDescriptor.control,
            hdaWritableMap_->getLength(), 1000, true,
            initialDescriptorEmpty);
    setProperty("Boot.CodeLoaderResetSequenceReady", resetSequenceReady);
    if (!resetSequenceReady)
        return false;

    markFailureGate("code-loader SDn reset handshake");
    bool resetWriteMayHaveOccurred = false;
    for (uint32_t poll = 0; poll < 2000; ++poll) {
        resetWriteMayHaveOccurred = true;
        const CodeLoaderResetResult result =
            executor.advanceReset(&codeLoaderResetSequence_);
        setProperty("Boot.CodeLoaderResetFailureStage",
                    executor.lastResetFailureStage());
        setProperty("Boot.CodeLoaderResetGctl",
                    executor.lastResetGctl(), 32);
        setProperty("Boot.CodeLoaderResetControl",
                    executor.lastResetControl(), 32);
        setProperty("Boot.CodeLoaderResetStatus",
                    executor.lastResetStatus(), 32);
        setProperty("Boot.CodeLoaderResetReadback",
                    executor.lastResetReadback(), 32);
        setProperty("Boot.CodeLoaderResetSequenceResult",
                    static_cast<uint32_t>(result), 32);
        setProperty("Boot.CodeLoaderResetPollCount", poll + 1U, 32);
        if (result == CodeLoaderResetResult::Complete) break;
        if (result != CodeLoaderResetResult::Waiting)
            return resetWriteMayHaveOccurred
                ? retainAfterPossibleWrite() : false;
        IODelay(100);
    }
    setProperty("Boot.CodeLoaderResetComplete",
                codeLoaderResetSequence_.complete());
    if (!codeLoaderResetSequence_.complete())
        return resetWriteMayHaveOccurred
            ? retainAfterPossibleWrite() : false;

    // Re-snapshot after reset and recompute the tag claim from live register
    // state. Preserve the pre-reset CTL value for eventual rollback.
    markFailureGate("post-reset HDA snapshot");
    sof::ipc4::kernel::HdaControllerStatusReader reader(
        pci_, this, hdaWritableMap_,
        kAppleHdaOnlyMode && controllerProviderReady());
    if (!reader.readSnapshot(&postResetSnapshot_))
        return retainAfterPossibleWrite();
    markFailureGate("post-reset code-loader reservation revalidation");
    if (sof::ipc4::kernel::planCavs25CodeLoaderStreamReservation(
            postResetSnapshot_.globalCapabilities,
            postResetSnapshot_.inputStreamCount,
            postResetSnapshot_.outputStreamCount,
            postResetSnapshot_.totalStreamCount,
            postResetSnapshot_.allStreamControls,
            postResetSnapshot_.totalStreamCount,
            postResetSnapshot_.allocatedStreamTagMask,
            codeLoaderPlan_.previousControl,
            hdaWritableMap_->getLength(), true, true, &codeLoaderPlan_) !=
        sof::ipc4::kernel::CodeLoaderStreamPlanResult::Ok ||
        codeLoaderPlan_.descriptorIndex != descriptorIndex ||
        descriptorIndex >= postResetSnapshot_.totalStreamCount ||
        codeLoaderReservationSequence_.initialize(
            codeLoaderPlan_, hdaWritableMap_->getLength()) !=
            sof::ipc4::kernel::CodeLoaderReservationSequenceResult::WriteRequired)
        return retainAfterPossibleWrite();

    markFailureGate("code-loader stream-tag reservation write");
    const auto reservationResult =
        executor.advanceReservation(&codeLoaderReservationSequence_);
    if (reservationResult !=
            sof::ipc4::kernel::CodeLoaderReservationSequenceResult::Complete ||
        !codeLoaderReservationSequence_.complete())
        return retainAfterPossibleWrite();
    markFailureGate("post-reservation HDA snapshot");
    if (!reader.readSnapshot(&postResetSnapshot_))
        return retainAfterPossibleWrite();

    Cavs25CodeLoaderRegisterSnapshot setupSnapshot{};
    setupSnapshot.ppCapabilityOffset = postResetSnapshot_.ppCapabilityOffset;
    setupSnapshot.ppCapabilityHeader = postResetSnapshot_.ppCapabilityHeader;
    setupSnapshot.ppctl = postResetSnapshot_.ppctl;
    setupSnapshot.intctl = postResetSnapshot_.intctl;
    setupSnapshot.reservedStreamControlReadback =
        postResetSnapshot_.allStreamControls[descriptorIndex].control;
    setupSnapshot.streamStatus =
        postResetSnapshot_.allStreamControls[descriptorIndex].status;
    const auto &loaderDescriptor =
        postResetSnapshot_.allStreamControls[descriptorIndex];
    setupSnapshot.streamRegisters[0] = loaderDescriptor.bdlLow;
    setupSnapshot.streamRegisters[1] = loaderDescriptor.bdlHigh;
    setupSnapshot.streamRegisters[2] = loaderDescriptor.cyclicBufferLength;
    setupSnapshot.streamRegisters[3] = loaderDescriptor.lastValidIndex;
    setupSnapshot.streamRegisters[4] = loaderDescriptor.format;
    setupSnapshot.streamRegisters[5] = codeLoaderPlan_.previousControl;
    setupSnapshot.spibPresent = postResetSnapshot_.spibCapabilityPresent;
    setupSnapshot.spibCapabilityOffset =
        postResetSnapshot_.spibCapabilityOffset;
    setupSnapshot.spibCapabilityHeader =
        postResetSnapshot_.spibCapabilityHeader;
    setupSnapshot.spbfcctl = postResetSnapshot_.spbfcctl;
    setupSnapshot.spibPosition = postResetSnapshot_.spibPositions[
        descriptorIndex];
    setupSnapshot.controllerReady =
        (postResetSnapshot_.globalControl & 1U) != 0;

    markFailureGate("stopped code-loader setup plan");
    if (sof::ipc4::kernel::planCavs25CodeLoaderSetup(
            codeLoaderPlan_, dmaPlan_, setupSnapshot,
            hdaWritableMap_->getLength(), true, true,
            &codeLoaderSetupPlan_) !=
            sof::ipc4::kernel::CodeLoaderSetupPlanResult::Ok ||
        codeLoaderSetupSequence_.initialize(
            codeLoaderSetupPlan_, hdaWritableMap_->getLength()) !=
            CodeLoaderSetupSequenceResult::WriteRequired)
        return retainAfterPossibleWrite();

    bool setupComplete = false;
    markFailureGate("stopped code-loader register setup");
    for (uint8_t step = 0; step < codeLoaderSetupPlan_.writeCount; ++step) {
        const CodeLoaderSetupSequenceResult result =
            executor.advanceSetup(&codeLoaderSetupSequence_);
        setProperty("Boot.CodeLoaderSetupSequenceResult",
                    static_cast<uint32_t>(result), 32);
        setProperty("Boot.CodeLoaderSetupStep", step, 8);
        if (result == CodeLoaderSetupSequenceResult::Complete) {
            setupComplete = true;
            break;
        }
        if (result != CodeLoaderSetupSequenceResult::Matched)
            return retainAfterPossibleWrite();
    }
    markFailureGate("final code-loader setup readback");
    if (!setupComplete || !codeLoaderSetupSequence_.complete() ||
        !reader.readSnapshot(&postResetSnapshot_))
        return retainAfterPossibleWrite();

    constexpr uint32_t kPpGlobalProcessingEnable = 1U << 30;
    constexpr uint32_t kPpInterruptEnable = 1U << 31;
    constexpr uint32_t kIntGlobalEnable = 1U << 31;
    constexpr uint32_t kIntControllerEnable = 1U << 30;
    const uint32_t streamBit = 1U << descriptorIndex;
    const uint32_t ppChangedMask = streamBit |
        kPpGlobalProcessingEnable | kPpInterruptEnable;
    const uint32_t intChangedMask = streamBit |
        kIntGlobalEnable | kIntControllerEnable;
    const bool unrelatedRoutingPreserved =
        ((postResetSnapshot_.ppctl ^ setupSnapshot.ppctl) &
            ~ppChangedMask) == 0 &&
        ((postResetSnapshot_.intctl ^ setupSnapshot.intctl) &
            ~intChangedMask) == 0 &&
        (!setupSnapshot.spibPresent ||
         ((postResetSnapshot_.spbfcctl ^ setupSnapshot.spbfcctl) &
            ~streamBit) == 0);
    const uint32_t finalControl =
        postResetSnapshot_.allStreamControls[descriptorIndex].control;
    LoaderPrepareEvidence evidence{};
    evidence.savedControllerSnapshot = true;
    evidence.codeLoaderRunClear = finalControl != UINT32_MAX &&
        (finalControl & sof::ipc4::kernel::kHdaStreamRunMask) == 0;
    evidence.streamResetComplete = codeLoaderResetSequence_.complete();
    evidence.unrelatedInterruptStatePreserved = unrelatedRoutingPreserved;
    evidence.observedDmaPlan = dmaPlan_;
    markFailureGate("code-loader lifecycle acceptance");
    if (!evidence.codeLoaderRunClear || !unrelatedRoutingPreserved ||
        bootLifecycle_.acceptLoaderPrepared(evidence) !=
            sof::ipc4::kernel::BootResult::Ok)
        return retainAfterPossibleWrite();

    codeLoaderSetupPrepared_ = true;
    setProperty("HDA.CodeLoaderSetupPrepared", true);
    setProperty("Boot.LifecyclePhase",
                static_cast<uint32_t>(bootLifecycle_.phase()), 32);
    return true;
}

bool AppleHDADmicBridge::advanceRomToInitDone() noexcept {
    using sof::ipc4::kernel::BootPhase;
    using sof::ipc4::kernel::Cavs25RomCommandExecutor;
    using sof::ipc4::kernel::Cavs25RomStatusReader;
    using sof::ipc4::kernel::RomCheckpoint;
    using sof::ipc4::kernel::RomCommandExecutionResult;
    using sof::ipc4::kernel::RomCoreStartSequenceResult;
    using sof::ipc4::kernel::RomPollResult;
    using sof::ipc4::kernel::RomPurgePollResult;

    setProperty("Boot.AdvanceStage", "ROM boot preconditions");
    if (romInitDonePrepared_)
        return bootLifecycle_.phase() == BootPhase::RomInitDone;
    if (bootLifecycle_.phase() != BootPhase::LoaderPrepared ||
        !codeLoaderSetupPrepared_ || !pci_ || !dspMap_ ||
        !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        return false;

    const auto quarantineRomState = [this]() noexcept {
        sof::ipc4::kernel::AbortEvidence uncertain{};
        (void)bootLifecycle_.quarantineFailure(uncertain);
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        setProperty("Status", "ROM boot state uncertain; resources retained");
        setProperty("Controller.Quarantined", true);
        setProperty("Boot.LifecyclePhase",
                    static_cast<uint32_t>(bootLifecycle_.phase()), 32);
        if (dspWritableMap_) {
            sof::ipc4::kernel::Cavs25RomStatusReader reader(
                pci_, this, dspWritableMap_,
                kAppleHdaOnlyMode && controllerProviderReady());
            uint32_t romStatus = UINT32_MAX;
            uint32_t romError = UINT32_MAX;
            uint32_t purgeAck = UINT32_MAX;
            uint32_t purgeError = UINT32_MAX;
            uint32_t hostRequest = UINT32_MAX;
            uint32_t adspcs = UINT32_MAX;
            if (reader.readSnapshot(&romStatus, &romError)) {
                setProperty("Boot.LastRomStatus", romStatus, 32);
                setProperty("Boot.LastRomError", romError, 32);
            }
            if (reader.readPurgeAck(&purgeAck, &purgeError)) {
                setProperty("Boot.LastPurgeAck", purgeAck, 32);
                setProperty("Boot.LastPurgeError", purgeError, 32);
            }
            if (reader.readPurgeAndCoreState(&hostRequest, &adspcs)) {
                setProperty("Boot.LastHostRequest", hostRequest, 32);
                setProperty("Boot.LastAdspcs", adspcs, 32);
            }
        }
        return false;
    };

    setProperty("Boot.AdvanceStage", "ROM writable BAR setup");
    if (!dspWritableMap_) {
        dspWritableMap_ = pci_->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress4, kIOMapInhibitCache);
    }
    if (!dspWritableMap_ || dspWritableMap_->getVirtualAddress() == 0 ||
        dspWritableMap_->getLength() < kDspBarMinimum ||
        dspWritableMap_->getLength() != dspMap_->getLength() ||
        (dspWritableMap_->getMapOptions() & kIOMapReadOnly) != 0)
        return quarantineRomState();

    Cavs25RomCommandExecutor executor(
        pci_, this, dspWritableMap_, &bootLifecycle_, true,
        kAppleHdaOnlyMode && controllerProviderReady());
    constexpr uint32_t kHostManagedCoreMask = 0x01U;
    constexpr uint32_t kHostManagedCoreCpaMask =
        kHostManagedCoreMask << 24;
    constexpr uint32_t kSecondaryDspCoreMask = 0x0eU;
    setProperty("Boot.AdvanceStage", "clear stale secondary power requests");
    uint32_t adspcs = UINT32_MAX;
    if (!executor.readAdspcs(&adspcs)) return quarantineRomState();
    const uint32_t staleSecondaryMask =
        (adspcs >> 16) & kSecondaryDspCoreMask;
    uint32_t corePowerReadback = UINT32_MAX;
    if (staleSecondaryMask != 0) {
        if (executor.requestCorePower(staleSecondaryMask, false,
                &corePowerReadback) != RomCommandExecutionResult::Ok)
            return quarantineRomState();
        bool staleRequestsCleared = false;
        const uint32_t staleCpaMask = staleSecondaryMask << 24;
        for (uint32_t poll = 0; poll < 5000; ++poll) {
            if (!executor.readAdspcs(&adspcs))
                return quarantineRomState();
            setProperty("Boot.CorePowerLastADSPCS", adspcs, 32);
            setProperty("Boot.CorePowerPollCount", poll + 1U, 32);
            if ((adspcs & staleCpaMask) == 0) {
                staleRequestsCleared = true;
                break;
            }
            IODelay(100);
        }
        setProperty("Boot.StaleSecondaryRequestsCleared",
                    staleRequestsCleared);
        if (!staleRequestsCleared) return quarantineRomState();
    } else {
        setProperty("Boot.StaleSecondaryRequestsCleared", true);
    }

    setProperty("Boot.AdvanceStage", "host-managed Core 0 power-up");
    setProperty("Boot.CorePowerUpMask", kHostManagedCoreMask, 8);
    if (executor.requestCorePower(kHostManagedCoreMask, true,
            &corePowerReadback) != RomCommandExecutionResult::Ok)
        return quarantineRomState();
    setProperty("Boot.CorePowerUpReadback", corePowerReadback, 32);
    bool hostManagedCorePowered = false;
    for (uint32_t poll = 0; poll < 5000; ++poll) {
        if (!executor.readAdspcs(&adspcs))
            return quarantineRomState();
        setProperty("Boot.CorePowerLastADSPCS", adspcs, 32);
        setProperty("Boot.CorePowerPollCount", poll + 1U, 32);
        if ((adspcs & kHostManagedCoreCpaMask) ==
            kHostManagedCoreCpaMask) {
            hostManagedCorePowered = true;
            break;
        }
        IODelay(100);
    }
    setProperty("Boot.HostManagedCorePowered", hostManagedCorePowered);
    if (!hostManagedCorePowered) return quarantineRomState();

    setProperty("Boot.AdvanceStage", "ROM purge acknowledgement baseline");
    Cavs25RomStatusReader reader(pci_, this, dspWritableMap_,
                kAppleHdaOnlyMode && controllerProviderReady());
    uint32_t initialPurgeAck = UINT32_MAX;
    uint32_t initialPurgeError = UINT32_MAX;
    if (!reader.readPurgeAck(&initialPurgeAck, &initialPurgeError) ||
        !romPurgePlan_.beginAckPoll(initialPurgeAck, initialPurgeError, 5000))
        return quarantineRomState();

    setProperty("Boot.AdvanceStage", "ROM purge request submission");
    uint32_t expectedDoorbell = 0;
    if (!sof::ipc4::kernel::buildCavs25RomPurgeDoorbell(
            codeLoaderPlan_.streamTag, &expectedDoorbell))
        return quarantineRomState();
    uint32_t doorbellReadback = UINT32_MAX;
    const RomCommandExecutionResult purgeSubmitResult =
        executor.submitPurgeRequest(codeLoaderPlan_.streamTag,
                                    &doorbellReadback);
    if (purgeSubmitResult != RomCommandExecutionResult::Ok ||
        bootLifecycle_.acceptRomPurgeSubmitted(
            expectedDoorbell, doorbellReadback) !=
            sof::ipc4::kernel::BootResult::Ok)
        return quarantineRomState();

    setProperty("Boot.AdvanceStage", "ROM primary core start");
    uint32_t hostRequest = UINT32_MAX;
    uint32_t adspcsReadback = UINT32_MAX;
    if (!reader.readPurgeAndCoreState(&hostRequest, &adspcsReadback) ||
        hostRequest == UINT32_MAX ||
        romCoreStartSequence_.initialize(
            adspcsReadback, true, expectedDoorbell,
            doorbellReadback) !=
            sof::ipc4::kernel::RomCoreStartPlanResult::Ok)
        return quarantineRomState();

    bool coreStartComplete = false;
    constexpr uint32_t kCoreStartPollLimit = 2000;
    for (uint32_t poll = 0; poll < kCoreStartPollLimit; ++poll) {
        if (!reader.readPurgeAndCoreState(&hostRequest, &adspcsReadback))
            return quarantineRomState();
        sof::ipc4::kernel::Cavs25AdspcsStep step{};
        const RomCoreStartSequenceResult next =
            romCoreStartSequence_.nextUpdate(adspcsReadback, &step);
        if (next == RomCoreStartSequenceResult::Complete) {
            coreStartComplete = true;
            break;
        }
        if (next == RomCoreStartSequenceResult::WaitingForHardware) {
            IODelay(100);
            continue;
        }
        if (next != RomCoreStartSequenceResult::Ready)
            return quarantineRomState();

        const RomCommandExecutionResult stepResult =
            executor.executeCoreStartStep(&step, &adspcsReadback);
        if (stepResult != RomCommandExecutionResult::Ok)
            return quarantineRomState();
        const RomCoreStartSequenceResult confirmed =
            romCoreStartSequence_.confirmUpdate(adspcsReadback);
        if (confirmed != RomCoreStartSequenceResult::WaitingForHardware &&
            confirmed != RomCoreStartSequenceResult::Complete)
            return quarantineRomState();
        if (romCoreStartSequence_.complete()) {
            coreStartComplete = true;
            break;
        }
    }
    if (!coreStartComplete || !romCoreStartSequence_.complete() ||
        !reader.readPurgeAndCoreState(&hostRequest, &adspcsReadback) ||
        bootLifecycle_.acceptRomPrimaryCoreStarted(
            romCoreStartSequence_, adspcsReadback) !=
            sof::ipc4::kernel::BootResult::Ok)
        return quarantineRomState();

    setProperty("Boot.AdvanceStage", "ROM purge acknowledgement");
    RomPurgePollResult purgeResult = RomPurgePollResult::Waiting;
    for (uint32_t poll = 0; poll < 5000; ++poll) {
        if (!reader.observePurgeAck(&romPurgePlan_, &purgeResult))
            return quarantineRomState();
        if (purgeResult == RomPurgePollResult::Accepted) break;
        if (purgeResult != RomPurgePollResult::Waiting)
            return quarantineRomState();
        IODelay(100);
    }
    if (purgeResult != RomPurgePollResult::Accepted)
        return quarantineRomState();
    uint32_t purgeAckReadback = UINT32_MAX;
    if (executor.acknowledgePurge(purgeResult, &purgeAckReadback) !=
            RomCommandExecutionResult::Ok ||
        bootLifecycle_.acceptRomPurgeAck(purgeResult) !=
            sof::ipc4::kernel::BootResult::Ok)
        return quarantineRomState();

    setProperty("Boot.AdvanceStage", "ROM INIT_DONE poll");
    uint32_t initialStatus = UINT32_MAX;
    uint32_t initialRomError = UINT32_MAX;
    if (!reader.readSnapshot(&initialStatus, &initialRomError) ||
        !romStatusPlan_.begin(RomCheckpoint::InitDone, 5000,
                              initialStatus, initialRomError))
        return quarantineRomState();
    RomPollResult romResult = RomPollResult::Waiting;
    for (uint32_t poll = 0; poll < 5000; ++poll) {
        if (!reader.observe(&romStatusPlan_, &romResult))
            return quarantineRomState();
        if (romResult == RomPollResult::InitDoneReached) break;
        if (romResult != RomPollResult::Waiting)
            return quarantineRomState();
        IODelay(100);
    }
    if (romResult != RomPollResult::InitDoneReached ||
        bootLifecycle_.acceptRomInitDone(romResult) !=
            sof::ipc4::kernel::BootResult::Ok)
        return quarantineRomState();

    romInitDonePrepared_ = true;
    setProperty("Boot.AdvanceStage", "ROM INIT_DONE accepted");
    setProperty("Boot.LifecyclePhase",
                static_cast<uint32_t>(bootLifecycle_.phase()), 32);
    setProperty("Boot.RomInitDoneObserved", true);
    return true;
}

bool AppleHDADmicBridge::advanceFirmwareToLoaderStopped() noexcept {
    using sof::ipc4::kernel::BootPhase;
    using sof::ipc4::kernel::Cavs25CodeLoaderMmioExecutor;
    using sof::ipc4::kernel::Cavs25RomStatusReader;
    using sof::ipc4::kernel::HdaControllerStatusReader;
    using sof::ipc4::kernel::RomCheckpoint;
    using sof::ipc4::kernel::RomPollResult;

    setProperty("Boot.AdvanceStage", "firmware start preconditions");
    if (firmwareLoaderStoppedPrepared_)
        return bootLifecycle_.phase() == BootPhase::LoaderStopped;
    const bool loaderPhaseReady = kAppleHdaOnlyMode
        ? bootLifecycle_.phase() == BootPhase::LoaderPrepared
        : romInitDonePrepared_ && bootLifecycle_.phase() == BootPhase::RomInitDone;
    if (!loaderPhaseReady ||
        !codeLoaderSetupPrepared_ || !pci_ || !dspMap_ ||
        (!kAppleHdaOnlyMode && !hdaWritableMap_) ||
        !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        return false;

    const auto quarantineBootState = [this]() noexcept {
        sof::ipc4::kernel::AbortEvidence uncertain{};
        (void)bootLifecycle_.quarantineFailure(uncertain);
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        setProperty("Status", "firmware loader handoff uncertain; resources retained");
        setProperty("Controller.Quarantined", true);
        setProperty("Boot.LifecyclePhase",
                    static_cast<uint32_t>(bootLifecycle_.phase()), 32);
        if (dspMap_) {
            Cavs25RomStatusReader reader(
            pci_, this, dspMap_,
            kAppleHdaOnlyMode && controllerProviderReady());
            uint32_t romStatus = UINT32_MAX;
            uint32_t firmwareStatus = UINT32_MAX;
            if (reader.readSnapshot(&romStatus, &firmwareStatus)) {
                setProperty("Boot.LastRomStatus", romStatus, 32);
                setProperty("Boot.LastFirmwareStatus", firmwareStatus, 32);
            }
        }
        if (hdaWritableMap_) {
            HdaControllerStatusReader reader(pci_, this, hdaWritableMap_,
        kAppleHdaOnlyMode && controllerProviderReady());
            sof::ipc4::kernel::HdaControllerStatusSnapshot snapshot{};
            if (reader.readSnapshot(&snapshot)) {
                setProperty("Boot.LastHdaGlobalControl",
                            snapshot.globalControl, 32);
                setProperty("Boot.LastHdaStreamCount",
                            snapshot.totalStreamCount, 8);
                if (codeLoaderPlan_.descriptorIndex <
                        snapshot.totalStreamCount &&
                    codeLoaderPlan_.descriptorIndex <
                        sof::ipc4::kernel::kHdaMaximumTotalStreamCount) {
                    const auto &stream = snapshot.allStreamControls[
                        codeLoaderPlan_.descriptorIndex];
                    setProperty("Boot.LastLoaderStreamControl",
                                stream.control, 32);
                    setProperty("Boot.LastLoaderStreamStatus",
                                stream.status, 8);
                    setProperty("Boot.LastLoaderStreamCbl",
                                stream.cyclicBufferLength, 32);
                    setProperty("Boot.LastLoaderStreamLvi",
                                stream.lastValidIndex, 8);
                }
            }
        }
        return false;
    };

    if (kAppleHdaOnlyMode) {
        setProperty("Boot.AdvanceStage", "AppleHDA-managed firmware DMA");
        if (!loaderStreamLease_.active()) return quarantineBootState();
        uint8_t descriptorIndex = UINT8_MAX;
        uint8_t streamTag = 0;
        uint32_t initialPosition = UINT32_MAX;
        if (!loaderStreamLease_.activeDescriptor(
                hdaSnapshot_.inputStreamCount,
                hdaSnapshot_.outputStreamCount,
                hdaSnapshot_.totalStreamCount,
                &descriptorIndex, &streamTag) ||
            descriptorIndex != codeLoaderPlan_.descriptorIndex ||
            streamTag != codeLoaderPlan_.streamTag ||
            !loaderStreamLease_.linkPositionBytes(&initialPosition))
            return quarantineBootState();

        setProperty("Boot.AppleHDALoaderInitialPosition", initialPosition, 32);
        // Prepare the loader route and SPIB before issuing the ROM command,
        // matching SOF's order. AppleHDA still owns the host descriptor.
        setProperty("Boot.AdvanceStage", "AppleHDA firmware DMA routing");
        if (!hdaWritableMap_)
            hdaWritableMap_ = pci_->mapDeviceMemoryWithRegister(
                kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
        const uint32_t ppOffset = hdaSnapshot_.ppCapabilityOffset;
        const uint32_t spibOffset = hdaSnapshot_.spibCapabilityOffset;
        const uint64_t spibPositionOffset =
            static_cast<uint64_t>(spibOffset) + 8U + 8U * descriptorIndex;
        const uint32_t streamOffset = codeLoaderPlan_.descriptorControlOffset;
        auto *workLoop = appleHdaController_->getWorkLoop();
        if (!hdaWritableMap_ || !workLoop ||
            (hdaWritableMap_->getMapOptions() & kIOMapReadOnly) != 0 ||
            hdaWritableMap_->getLength() != hdaMap_->getLength() ||
            hdaWritableMap_->getVirtualAddress() == 0 ||
            ppOffset < 0x40 || (ppOffset & 3U) != 0 ||
            (hdaSnapshot_.ppCapabilityHeader & 0x0fff0000U) != 0x00030000U ||
            !hdaSnapshot_.spibCapabilityPresent || spibOffset < 0x40 || (spibOffset & 3U) != 0 ||
            (hdaSnapshot_.spibCapabilityHeader & 0x0fff0000U) != 0x00040000U ||
            static_cast<uint64_t>(ppOffset) + 8 > hdaWritableMap_->getLength() ||
            spibPositionOffset + 4 > hdaWritableMap_->getLength() ||
            static_cast<uint64_t>(streamOffset) + 0x20 > hdaWritableMap_->getLength())
            return quarantineBootState();
        auto *hdaBase = reinterpret_cast<volatile uint32_t *>(
            hdaWritableMap_->getVirtualAddress());
        LoaderRoutingChange routing{
            hdaBase + (ppOffset + 4) / 4, hdaBase + (spibOffset + 4) / 4,
            hdaBase + spibPositionOffset / 4, hdaBase + streamOffset / 4,
            hdaBase + (streamOffset + 8) / 4,
            reinterpret_cast<volatile uint16_t *>(hdaBase + (streamOffset + 0x0c) / 4),
            hdaBase + (streamOffset + 0x18) / 4,
            1U << descriptorIndex, dmaResources_.firmwarePayloadBytes(), true,
            UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0};
        const auto restoreRouting = [&]() noexcept {
            routing.enable = false;
            const IOReturn result = workLoop->runAction(changeLoaderRouting, this, &routing);
            setProperty("Boot.AppleHDALoaderRoutingRestoreResult", static_cast<uint32_t>(result), 32);
            return result == kIOReturnSuccess;
        };
        const IOReturn routingResult = workLoop->runAction(changeLoaderRouting, this, &routing);
        setProperty("Boot.AppleHDALoaderRoutingResult", static_cast<uint32_t>(routingResult), 32);
        setProperty("Boot.AppleHDALoaderPpctlBefore", routing.ppctlBefore, 32);
        setProperty("Boot.AppleHDALoaderPpctlAfter", routing.ppctlAfter, 32);
        setProperty("Boot.AppleHDALoaderSpib", *routing.spibPosition, 32);
        setProperty("Boot.AppleHDALoaderProgrammedCbl", *routing.streamCbl, 32);
        setProperty("Boot.AppleHDALoaderProgrammedLvi", *routing.streamLvi, 16);
        if (routingResult != kIOReturnSuccess) {
            if (routingResult == kIOReturnIOError) (void)restoreRouting();
            return quarantineBootState();
        }
        if (!advanceRomToInitDone()) {
            (void)restoreRouting();
            return quarantineBootState();
        }
        const auto triggerLoader = [&](bool start) noexcept {
            routing.enable = start;
            return workLoop->runAction(triggerFirmwareLoader, this, &routing);
        };
        const auto stopAndQuarantine = [&]() noexcept {
            if (triggerLoader(false) == kIOReturnSuccess)
                (void)restoreRouting();
            return quarantineBootState();
        };

        setProperty("Boot.AdvanceStage", "AppleHDA firmware DMA baseline");
        Cavs25RomStatusReader romReader(
            pci_, this, dspMap_, controllerProviderReady());
        uint32_t baselineStatus = UINT32_MAX;
        uint32_t baselineFirmwareStatus = UINT32_MAX;
        // SOF's base-firmware timeout is three seconds at 100 us per poll.
        constexpr uint32_t kFirmwareEntryPollLimit = 30000;
        if (!romReader.readSnapshot(&baselineStatus, &baselineFirmwareStatus) ||
            !romStatusPlan_.begin(RomCheckpoint::FirmwareEntered, kFirmwareEntryPollLimit,
                                  baselineStatus, baselineFirmwareStatus)) {
            (void)restoreRouting();
            return quarantineBootState();
        }
        setProperty("Boot.FirmwareDmaBaselineRomStatus", baselineStatus, 32);
        setProperty("Boot.FirmwareDmaBaselineFirmwareStatus", baselineFirmwareStatus, 32);

        setProperty("Boot.AdvanceStage", "AppleHDA firmware DMA start");
        const auto *lpib = routing.streamControl + 1;
        OSSynchronizeIO();
        const uint32_t hostInitialPosition = *lpib;
        // The PCM start path calls resetStream/programStream again, undoing
        // the firmware-specific CBL/LVI after ROM has latched its route.
        const IOReturn startResult = triggerLoader(true);
        setProperty("Boot.AppleHDALoaderDmaStartResult",
                    static_cast<uint32_t>(startResult), 32);
        if (startResult != kIOReturnSuccess)
            return stopAndQuarantine();
        OSSynchronizeIO();
        const uint32_t runningControl = *routing.streamControl;
        setProperty("Boot.AppleHDALoaderRunningControl", runningControl, 32);
        setProperty("Boot.AppleHDALoaderRunningCbl", *routing.streamCbl, 32);
        setProperty("Boot.AppleHDALoaderRunningLvi", *routing.streamLvi, 16);
        if (runningControl == UINT32_MAX) return stopAndQuarantine();
        sof::ipc4::kernel::FirmwareDmaEvidence dmaEvidence{};
        dmaEvidence.runObserved = (runningControl & 3U) == 2U;
        dmaEvidence.expectedStreamTag = ((runningControl >> 20) & 0xfU) == streamTag;
        dmaEvidence.appleHdaStreamLeaseValidated = true;
        if (bootLifecycle_.acceptFirmwareDmaStarted(dmaEvidence) !=
            sof::ipc4::kernel::BootResult::Ok)
            return stopAndQuarantine();

        RomPollResult firmwareResult = RomPollResult::Waiting;
        setProperty("Boot.AdvanceStage", "AppleHDA firmware entry wait");
        for (uint32_t poll = 0; poll < kFirmwareEntryPollLimit; ++poll) {
            if (!romReader.observe(&romStatusPlan_, &firmwareResult))
                return stopAndQuarantine();
            if (firmwareResult != RomPollResult::Waiting) break;
            IODelay(100);
        }
        OSSynchronizeIO();
        const uint32_t hostPosition = *lpib;
        uint32_t linkPosition = UINT32_MAX;
        (void)loaderStreamLease_.linkPositionBytes(&linkPosition);
        setProperty("Boot.AppleHDALoaderHostInitialPosition", hostInitialPosition, 32);
        setProperty("Boot.AppleHDALoaderHostPosition", hostPosition, 32);
        setProperty("Boot.AppleHDALoaderLinkPosition", linkPosition, 32);
        setProperty("Boot.AppleHDALoaderControlAtRomResult", *routing.streamControl, 32);
        setProperty("Boot.FirmwareEntryPollResult", static_cast<uint32_t>(firmwareResult), 32);
        if (firmwareResult != RomPollResult::FirmwareEnteredReached ||
            bootLifecycle_.acceptFirmwareEntered(firmwareResult) !=
                sof::ipc4::kernel::BootResult::Ok)
            return stopAndQuarantine();

        setProperty("Boot.AdvanceStage", "AppleHDA firmware DMA stop");
        const IOReturn stopResult = triggerLoader(false);
        setProperty("Boot.AppleHDALoaderDmaStopResult",
                    static_cast<uint32_t>(stopResult), 32);
        bool positionStable = false;
        uint32_t stoppedPosition = UINT32_MAX;
        if (stopResult == kIOReturnSuccess &&
            loaderStreamLease_.linkPositionBytes(&stoppedPosition)) {
            for (uint32_t poll = 0; poll < 20; ++poll) {
                IODelay(1000);
                uint32_t currentPosition = UINT32_MAX;
                if (!loaderStreamLease_.linkPositionBytes(&currentPosition))
                    break;
                if (currentPosition == stoppedPosition) {
                    positionStable = true;
                    break;
                }
                stoppedPosition = currentPosition;
            }
        }
        setProperty("Boot.AppleHDALoaderPositionStable", positionStable);
        if (stopResult != kIOReturnSuccess || !positionStable ||
            !restoreRouting() ||
            loaderStreamLease_.deactivate(1000) != kIOReturnSuccess ||
            bootLifecycle_.acceptAppleHdaLoaderStopped(true, true) !=
                sof::ipc4::kernel::BootResult::Ok ||
            !dmaResources_.releaseAfterQuiescence(bootLifecycle_))
            return quarantineBootState();
        loaderStreamLease_.release();
        firmwareLoaderStoppedPrepared_ = true;
        setProperty("Boot.AdvanceStage", "AppleHDA loader stream stopped");
        setProperty("Boot.LifecyclePhase",
                    static_cast<uint32_t>(bootLifecycle_.phase()), 32);
        setProperty("Boot.LoaderStoppedObserved", true);
        return true;
    }

    setProperty("Boot.AdvanceStage", "firmware status baseline");
    Cavs25RomStatusReader romReader(pci_, this, dspMap_);
    uint32_t baselineStatus = UINT32_MAX;
    uint32_t baselineFirmwareStatus = UINT32_MAX;
    if (!romReader.readSnapshot(&baselineStatus, &baselineFirmwareStatus) ||
        !romStatusPlan_.begin(RomCheckpoint::FirmwareEntered, 5000,
                              baselineStatus, baselineFirmwareStatus))
        return quarantineBootState();

    Cavs25CodeLoaderMmioExecutor executor(
        pci_, this, hdaWritableMap_, &bootLifecycle_,
        kAppleHdaOnlyMode && controllerProviderReady());
    setProperty("Boot.AdvanceStage", "firmware DMA start");
    uint32_t runningControl = UINT32_MAX;
    const auto dmaStartResult = executor.startFirmwareDma(&codeLoaderSetupSequence_, &runningControl);
    setProperty("Boot.FirmwareDmaStartResult", static_cast<uint32_t>(dmaStartResult), 32);
    setProperty("Boot.FirmwareDmaStartControl", runningControl, 32);
    if (dmaStartResult != sof::ipc4::kernel::CodeLoaderDmaStartResult::Started ||
        runningControl == UINT32_MAX ||
        (runningControl & (1U << 1)) == 0)
        return quarantineBootState();
    sof::ipc4::kernel::FirmwareDmaEvidence dmaEvidence{};
    dmaEvidence.runObserved = true;
    dmaEvidence.expectedStreamTag =
        ((runningControl >> 20) & 0x0fU) == codeLoaderPlan_.streamTag;
    dmaEvidence.expectedDescriptorBase =
        codeLoaderSetupSequence_.plannedReadbackValue(1) ==
            dmaPlan_.descriptorBaseLow &&
        codeLoaderSetupSequence_.plannedReadbackValue(2) ==
            dmaPlan_.descriptorBaseHigh;
    if (bootLifecycle_.acceptFirmwareDmaStarted(dmaEvidence) !=
        sof::ipc4::kernel::BootResult::Ok)
        return quarantineBootState();

    setProperty("Boot.AdvanceStage", "firmware entered wait");
    RomPollResult firmwareResult = RomPollResult::Waiting;
    for (uint32_t poll = 0; poll < 5000; ++poll) {
        if (!romReader.observe(&romStatusPlan_, &firmwareResult))
            return quarantineBootState();
        if (firmwareResult == RomPollResult::FirmwareEnteredReached) break;
        if (firmwareResult != RomPollResult::Waiting)
            return quarantineBootState();
        IODelay(100);
    }
    if (firmwareResult != RomPollResult::FirmwareEnteredReached ||
        bootLifecycle_.acceptFirmwareEntered(firmwareResult) !=
            sof::ipc4::kernel::BootResult::Ok)
        return quarantineBootState();

    setProperty("Boot.AdvanceStage", "firmware loader stream stop");
    HdaControllerStatusReader hdaReader(pci_, this, hdaWritableMap_,
        kAppleHdaOnlyMode && controllerProviderReady());
    if (!hdaReader.readSnapshot(&postResetSnapshot_) ||
        postResetSnapshot_.outputStreamCount == 0 ||
        codeLoaderPlan_.descriptorIndex >=
            postResetSnapshot_.totalStreamCount ||
        codeLoaderPlan_.descriptorIndex >=
            sof::ipc4::kernel::kHdaMaximumTotalStreamCount)
        return quarantineBootState();
    uint32_t currentControl =
        postResetSnapshot_.allStreamControls[
            codeLoaderPlan_.descriptorIndex].control;
    setProperty("Boot.LoaderStopInitialControl", currentControl, 32);
    const bool stopInitialized = codeLoaderDmaStopSequence_.initialize(
            codeLoaderSetupSequence_, currentControl, 5000, true, true);
    setProperty("Boot.LoaderStopInitialized", stopInitialized);
    if (!stopInitialized) return quarantineBootState();

    bool streamStopped = false;
    for (uint32_t poll = 0; poll < 5000; ++poll) {
        const auto result = executor.advanceDmaStop(
            &codeLoaderDmaStopSequence_);
        setProperty("Boot.LoaderStopResult", static_cast<uint32_t>(result), 32);
        setProperty("Boot.LoaderStopPollCount", poll + 1U, 32);
        if (result ==
            sof::ipc4::kernel::CodeLoaderDmaStopMmioResult::Stopped) {
            streamStopped = true;
            break;
        }
        if (result != sof::ipc4::kernel::CodeLoaderDmaStopMmioResult::Waiting &&
            result != sof::ipc4::kernel::CodeLoaderDmaStopMmioResult::WriteIssued)
            return quarantineBootState();
        IODelay(100);
    }
    setProperty("Boot.AdvanceStage", "firmware loader stream reset");
    if (!streamStopped || !codeLoaderDmaStopSequence_.complete() ||
        !hdaReader.readSnapshot(&postResetSnapshot_))
        return quarantineBootState();
    currentControl = postResetSnapshot_.allStreamControls[
        codeLoaderPlan_.descriptorIndex].control;
    if (!codeLoaderPostDmaResetSequence_.initialize(
            codeLoaderSetupSequence_, codeLoaderDmaStopSequence_,
            currentControl, 5000, true))
        return quarantineBootState();

    bool streamReset = false;
    for (uint32_t poll = 0; poll < 10000; ++poll) {
        const auto result = executor.advancePostDmaReset(
            &codeLoaderPostDmaResetSequence_);
        setProperty("Boot.LoaderResetResult", static_cast<uint32_t>(result), 32);
        setProperty("Boot.LoaderResetPollCount", poll + 1U, 32);
        if (result == sof::ipc4::kernel::CodeLoaderResetResult::Complete) {
            streamReset = true;
            break;
        }
        if (result != sof::ipc4::kernel::CodeLoaderResetResult::Waiting &&
            result != sof::ipc4::kernel::CodeLoaderResetResult::WriteRequired)
            return quarantineBootState();
        IODelay(100);
    }
    setProperty("Boot.LoaderResetObserved", streamReset);
    if (!streamReset || !codeLoaderPostDmaResetSequence_.complete() ||
        !hdaReader.readSnapshot(&postResetSnapshot_) ||
        codeLoaderPlan_.descriptorIndex >=
            postResetSnapshot_.totalStreamCount ||
        codeLoaderPlan_.descriptorIndex >=
            sof::ipc4::kernel::kHdaMaximumTotalStreamCount ||
        bootLifecycle_.acceptLoaderStopped(
            codeLoaderDmaStopSequence_, codeLoaderPostDmaResetSequence_,
            postResetSnapshot_.allStreamControls[
                codeLoaderPlan_.descriptorIndex].control) !=
            sof::ipc4::kernel::BootResult::Ok)
        return quarantineBootState();

    firmwareLoaderStoppedPrepared_ = true;
    setProperty("Boot.AdvanceStage", "loader stopped accepted");
    setProperty("Boot.LifecyclePhase",
                static_cast<uint32_t>(bootLifecycle_.phase()), 32);
    setProperty("Boot.LoaderStoppedObserved", true);
    return true;
}

bool AppleHDADmicBridge::serviceFirmwareReadyNotification() noexcept {
    using sof::ipc4::kernel::BootPhase;
    setProperty("Boot.AdvanceStage", "FW_READY preconditions");
    if (bootLifecycle_.phase() != BootPhase::LoaderStopped || !pci_ ||
        !dspMap_ || !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        return false;

    // Map BAR4 writable only for the final FW_READY notification ACK. The
    // MMIO policy permits only incoming doorbell ACKs before lifecycle READY;
    // mailbox traffic and host IPC remain closed until acceptFwReady succeeds.
    if (!dspWritableMap_) {
        dspWritableMap_ = pci_->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress4, kIOMapInhibitCache);
    }
    if (!dspWritableMap_ || dspWritableMap_->getVirtualAddress() == 0 ||
        dspWritableMap_->getLength() < kDspBarMinimum ||
        dspWritableMap_->getLength() != dspMap_->getLength() ||
        (dspWritableMap_->getMapOptions() & kIOMapReadOnly) != 0 ||
        !ipcAdapter_.configure(
            pci_, this, dspWritableMap_, &bootLifecycle_, true,
            kAppleHdaOnlyMode ? appleHdaController_ : nullptr,
            &AppleHDADmicBridge::handleIpc4Notification, this))
        return false;

    ipcIoOps_ = ipcAdapter_.ioOps();
    if (!ipcTransport_.configure(
            &ipcIoOps_, sof::ipc4::kernel::kMaximumPayloadBytes,
            sof::ipc4::kernel::kMaximumPayloadBytes))
        return false;
    // Keep servicing the bounded mailbox pump until the specific FW_READY
    // notification is both observed and ACKed. Unknown notifications remain
    // unacknowledged, and the outer deadline prevents a hung Start request.
    setProperty("Boot.AdvanceStage", "FW_READY notification wait");
    bool fwReadyAcked = false;
    for (uint32_t poll = 0; poll < 5000; ++poll) {
        const auto serviceResult = ipcTransport_.servicePendingMessages(8);
        setProperty("Boot.FwReadyServiceResult", static_cast<uint32_t>(serviceResult), 32);
        setProperty("Boot.FwReadyPollCount", poll + 1U, 32);
        setProperty("Boot.FwReadySeen", __atomic_load_n(&fwReadyNotificationSeen_, __ATOMIC_ACQUIRE));
        fwReadyAcked =
            __atomic_load_n(&fwReadyNotificationSeen_, __ATOMIC_ACQUIRE) &&
            ipcTransport_.firmwareReadyNotificationAcknowledged();
        if (fwReadyAcked) break;
        if (serviceResult != sof::ipc4::kernel::TransportResult::Ok &&
            serviceResult != sof::ipc4::kernel::TransportResult::DeviceBusy)
            return false;
        IODelay(100);
    }
    if (!fwReadyAcked ||
        bootLifecycle_.acceptFwReady(true, true) !=
            sof::ipc4::kernel::BootResult::Ok)
        return false;
    setProperty("Boot.AdvanceStage", "FW_READY accepted");
    return ipcTransport_.confirmFirmwareReady(&bootLifecycle_);
}

bool AppleHDADmicBridge::advanceBootToFirmwareReady() noexcept {
    using sof::ipc4::kernel::BootPhase;
    if (bootLifecycle_.canIssueIpc4()) return true;

    if (bootLifecycle_.phase() == BootPhase::DmaResourcesReady &&
        (setProperty("Boot.AdvanceStage", "code-loader preparation"),
         !prepareCodeLoaderStream()))
        return false;
    if (!kAppleHdaOnlyMode && bootLifecycle_.phase() == BootPhase::LoaderPrepared &&
        (setProperty("Boot.AdvanceStage", "ROM startup"),
         !advanceRomToInitDone()))
        return false;
    const auto loaderPhase = kAppleHdaOnlyMode ? BootPhase::LoaderPrepared : BootPhase::RomInitDone;
    if (bootLifecycle_.phase() == loaderPhase &&
        (setProperty("Boot.AdvanceStage", "firmware startup"),
         !advanceFirmwareToLoaderStopped()))
        return false;
    if (bootLifecycle_.phase() == BootPhase::LoaderStopped &&
        (setProperty("Boot.AdvanceStage", "FW_READY handshake"),
         !serviceFirmwareReadyNotification()))
        return false;
    if (bootLifecycle_.phase() == BootPhase::FirmwareReady)
        setProperty("Boot.AdvanceStage", "firmware ready");
    return bootLifecycle_.phase() == BootPhase::FirmwareReady;
}

bool AppleHDADmicBridge::initializeCaptureSession() noexcept {
    if (!bootLifecycle_.canIssueIpc4() || !capturePlanReady_ ||
        !ipcTransportConfigured_ || !pci_ ||
        !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        return false;

    if (!captureSession_.initialized()) {
        sof::ipc4::kernel::FirmwareConfigExchangeResult configResult{};
        sof::ipc4::kernel::TransportResult transportResult{};
        sof::ipc4::kernel::FirmwareConfigResult replyResult{};
        const auto result = captureSession_.initializeFromFirmwareConfig(
            &firmwareManifest_, &capturePlan_, &bootLifecycle_,
            &ipcTransport_, &captureIdAllocator_, kIpcTimeoutMilliseconds,
            kIpcPollIntervalMicroseconds, &configResult, &transportResult,
            &replyResult);
        setProperty("Capture.ConfigRunnerResult", static_cast<uint32_t>(result), 32);
        setProperty("Capture.ConfigFirmwareStatus", captureSession_.lastFirmwareStatus(), 32);
        setProperty("Capture.ConfigExchangeResult", static_cast<uint32_t>(configResult), 32);
        setProperty("Capture.ConfigTransportResult", static_cast<uint32_t>(transportResult), 32);
        setProperty("Capture.ConfigReplyResult", static_cast<uint32_t>(replyResult), 32);
        if (result != sof::ipc4::dmic::kernel::RunnerResult::Ok)
            return false;
    }

    if (captureSession_.setupComplete()) return true;
    for (uint8_t step = 0;
         step <= sof::ipc4::dmic::kernel::OperationPlan::setupOperationCount();
         ++step) {
        sof::ipc4::dmic::kernel::OperationKind kind{};
        sof::ipc4::kernel::TransportResult transportResult{};
        const auto result = captureSession_.setupNext(
            &kind, &transportResult, kIpcTimeoutMilliseconds,
            kIpcPollIntervalMicroseconds);
        setProperty("Capture.SetupStep", step, 8);
        setProperty("Capture.SetupKind", static_cast<uint32_t>(kind), 32);
        setProperty("Capture.SetupResult", static_cast<uint32_t>(result), 32);
        setProperty("Capture.SetupFirmwareStatus", captureSession_.lastFirmwareStatus(), 32);
        setProperty("Capture.SetupTransportResult", static_cast<uint32_t>(transportResult), 32);
        setProperty("Capture.SetupRequestPrimary", ipcTransport_.lastRequestPrimary(), 32);
        setProperty("Capture.SetupRequestExtension", ipcTransport_.lastRequestExtension(), 32);
        setProperty("Capture.SetupPreflightBusyPolls",
                    ipcTransport_.lastPreflightBusyPolls(), 32);
        setProperty("Capture.SetupPreflight.HIPCIDR",
                    ipcTransport_.lastPreflightHipcidr(), 32);
        setProperty("Capture.SetupPreflight.HIPCIDA",
                    ipcTransport_.lastPreflightHipcida(), 32);
        setProperty("Capture.SetupPreflight.HIPCTDR",
                    ipcTransport_.lastPreflightHipctdr(), 32);
        if (result == sof::ipc4::dmic::kernel::RunnerResult::Complete)
            return captureSession_.setupComplete();
        if (result != sof::ipc4::dmic::kernel::RunnerResult::Ok)
            return false;
    }
    return false;
}

bool AppleHDADmicBridge::resetStoppedCaptureSession() noexcept {
    using namespace sof::ipc4::dmic::kernel;
    using namespace sof::ipc4::kernel;
    TransportResult transportResult{};
    auto result = captureSession_.setPipelineState(kIpc4PipelineStateReset,
        &transportResult, kIpcTimeoutMilliseconds, kIpcPollIntervalMicroseconds);
    setProperty("Capture.RestartResetResult", static_cast<uint32_t>(result), 32);
    if (result != RunnerResult::Ok) return false;

    result = captureSession_.setPipelineState(kIpc4PipelineStatePaused,
        &transportResult, kIpcTimeoutMilliseconds, kIpcPollIntervalMicroseconds);
    setProperty("Capture.RestartPauseResult", static_cast<uint32_t>(result), 32);
    return result == RunnerResult::Ok;
}

bool AppleHDADmicBridge::initializeCaptureRuntime() noexcept {
    if (captureRuntime_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::Uninitialized)
        return captureRuntime_.state() ==
                   sof::ipc4::kernel::HdaCaptureTriggerState::Ready ||
               captureRuntime_.state() ==
                   sof::ipc4::kernel::HdaCaptureTriggerState::Stopped;
    if (!captureSetupPlanReady_ || !captureDmaPrepared_ ||
        (!kAppleHdaOnlyMode && !inputReservationSequence_.complete()) ||
        !captureSession_.setupComplete())
        return false;

    uint32_t streamControlMask = 0;
    uint32_t streamControlValue = UINT32_MAX;
    if (kAppleHdaOnlyMode) {
        streamControlMask = 0x00f00003U;
        streamControlValue =
            (static_cast<uint32_t>(allocationPlan_.streamTag) << 20) |
            (1U << 1);
    } else {
        for (uint8_t index = 0; index < captureSetupPlan_.writeCount; ++index) {
            if (captureSetupPlan_.writes[index].offset ==
                captureSetupPlan_.streamControlOffset) {
                streamControlMask = captureSetupPlan_.readbackMasks[index];
                streamControlValue = captureSetupPlan_.readbackValues[index] | (1U << 1);
                break;
            }
        }
    }
    if (streamControlMask == 0 || streamControlValue == UINT32_MAX)
        return false;
    captureStreamControlMask_ = streamControlMask;
    captureStreamControlValue_ = streamControlValue;

    sof::ipc4::kernel::TransportResult transportResult{};
    const auto runtimeResult = kAppleHdaOnlyMode
        ? captureRuntime_.initializeAppleHda(
            &captureSession_, &captureDma_,
            captureStreamLease_.active() && captureStreamLease_.controller() ==
                appleHdaController_,
            captureSetupPlan_.streamControlOffset, streamControlMask,
            streamControlValue, kIpcTimeoutMilliseconds,
            kIpcPollIntervalMicroseconds, &transportResult)
        : captureRuntime_.initialize(
            &captureSession_, &captureDma_, controllerProviderReady(),
            inputReservationSequence_,
            captureSetupPlan_.streamControlOffset, streamControlMask,
            streamControlValue, kIpcTimeoutMilliseconds,
            kIpcPollIntervalMicroseconds, &transportResult);
    setProperty("Capture.RuntimeResult", static_cast<uint32_t>(runtimeResult), 32);
    setProperty("Capture.RuntimeFirmwareStatus", captureSession_.lastFirmwareStatus(), 32);
    setProperty("Capture.RuntimeTransportResult", static_cast<uint32_t>(transportResult), 32);
    setProperty("Capture.RuntimeControlMask", streamControlMask, 32);
    setProperty("Capture.RuntimeControlValue", streamControlValue, 32);
    if (runtimeResult != sof::ipc4::dmic::kernel::CaptureRuntimeResult::Ok) return false;
    const auto iocResult = captureRuntime_.attachIocCounter(&captureIocCounter_);
    setProperty("Capture.RuntimeIocResult", static_cast<uint32_t>(iocResult), 32);
    if (iocResult != sof::ipc4::dmic::kernel::CaptureRuntimeResult::Ok) return false;
    const auto positionResult = captureRuntime_.attachPositionTracker(&capturePositionTracker_);
    setProperty("Capture.RuntimePositionResult", static_cast<uint32_t>(positionResult), 32);
    if (positionResult != sof::ipc4::dmic::kernel::CaptureRuntimeResult::Ok) return false;
    const bool pumpReady = capturePeriodPump_.initialize(&captureRuntime_, &capturePublisher_);
    setProperty("Capture.RuntimePumpReady", pumpReady);
    if (!pumpReady) return false;
    return true;
}

bool AppleHDADmicBridge::installCaptureInterruptBinding() noexcept {
    if (captureInterruptBinding_) return true;
    if (kAppleHdaOnlyMode) {
        if (!captureStreamLease_.active() ||
            !captureInterruptService_.initializeAppleHda(
                &capturePeriodPump_, captureSetupPlan_.streamControlOffset,
                captureRingBytes_,
                sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodBytes))
            return false;
    } else if (!captureMmio_.configure(pci_, this, hdaWritableMap_) ||
               !captureInterruptService_.initialize(
                   &captureMmio_, &capturePeriodPump_,
                   captureSetupPlan_.streamControlOffset)) {
        return false;
    }
    auto *binding = new sof::ipc4::dmic::kernel::DmicCaptureInterruptBinding;
    if (!binding || !binding->init() ||
        !binding->configure(
            &captureInterruptService_,
            &AppleHDADmicBridge::captureInterruptAction, this)) {
        if (binding) binding->release();
        return false;
    }
    const IOReturn installResult = kAppleHdaOnlyMode
        ? binding->installAppleHda(&captureStreamLease_, getWorkLoop())
        : binding->install(pci_, getWorkLoop(), 0);
    if (installResult != kIOReturnSuccess) {
        binding->uninstall();
        binding->release();
        return false;
    }
    captureInterruptBinding_ = binding;
    setProperty("Capture.InterruptBindingInstalled", true);
    return true;
}

bool AppleHDADmicBridge::rebaseCapturePositionBeforeInterruptEnable() noexcept {
    if (kAppleHdaOnlyMode) {
        uint32_t linkPosition = UINT32_MAX;
        if (!captureStreamLease_.active() ||
            !captureStreamLease_.linkPositionBytes(&linkPosition))
            return false;
        const uint64_t completed = capturePositionTracker_.lastCompletedPeriodCount();
        const bool rebased = capturePositionTracker_.rebase(linkPosition, completed);
        const bool pollArmed = rebased && captureInterruptService_.armAppleHdaPollBaseline(
            linkPosition, completed);
        setProperty("Capture.PreInterrupt.PositionReadSucceeded", true);
        setProperty("Capture.PreInterrupt.LinkPosition", linkPosition, 32);
        setProperty("Capture.PreInterrupt.PositionRebased", rebased);
        setProperty("Capture.PreInterrupt.AppleHDAPollBaselineArmed", pollArmed);
        return pollArmed;
    }
    uint8_t streamStatus = 0;
    if (!captureMmio_.readStreamStatus(captureSetupPlan_.streamControlOffset,
                                       &streamStatus)) {
        setProperty("Capture.PreInterrupt.StatusReadSucceeded", false);
        return false;
    }
    setProperty("Capture.PreInterrupt.StreamStatus", streamStatus, 32);
    if ((streamStatus & ~sof::ipc4::kernel::kHdaStreamStatusKnownMask) != 0 ||
        (streamStatus & (sof::ipc4::kernel::kHdaStreamStatusFifoError |
                         sof::ipc4::kernel::kHdaStreamStatusDescriptorError)) != 0)
        return false;

    if ((streamStatus & sof::ipc4::kernel::kHdaStreamStatusBufferCompletion) != 0) {
        sof::ipc4::kernel::HdaCaptureIocAck ack{};
        const auto observe = captureIocCounter_.observeStatus(streamStatus, &ack);
        setProperty("Capture.PreInterrupt.IocObserveResult",
                    static_cast<uint32_t>(observe), 32);
        if (observe != sof::ipc4::kernel::HdaCaptureIocResult::AckRequired)
            return false;
        const bool ackSucceeded = captureMmio_.writeIocAck(
            captureSetupPlan_.streamControlOffset, ack);
        setProperty("Capture.PreInterrupt.IocAckSucceeded", ackSucceeded);
        const auto ackResult = captureIocCounter_.confirmAck(ackSucceeded);
        setProperty("Capture.PreInterrupt.IocAckResult",
                    static_cast<uint32_t>(ackResult), 32);
        if (ackResult != sof::ipc4::kernel::HdaCaptureIocResult::Acknowledged)
            return false;
    }

    uint32_t linkPosition = UINT32_MAX;
    if (!captureMmio_.readLinkPosition(captureSetupPlan_.streamControlOffset,
                                       &linkPosition)) {
        setProperty("Capture.PreInterrupt.PositionReadSucceeded", false);
        return false;
    }
    setProperty("Capture.PreInterrupt.PositionReadSucceeded", true);
    setProperty("Capture.PreInterrupt.LinkPosition", linkPosition, 32);
    const bool rebased = capturePositionTracker_.rebase(
        linkPosition, captureIocCounter_.completedPeriodCount());
    setProperty("Capture.PreInterrupt.PositionRebased", rebased);
    return rebased;
}

uint32_t AppleHDADmicBridge::readAppleHdaCaptureControl() noexcept {
    const uint64_t offset = captureSetupPlan_.streamControlOffset;
    if (!hdaMap_ || !hdaMap_->getVirtualAddress() ||
        (offset & 3U) != 0 || offset + 4 > hdaMap_->getLength())
        return UINT32_MAX;
    OSSynchronizeIO();
    const uint32_t control = *reinterpret_cast<const volatile uint32_t *>(
        hdaMap_->getVirtualAddress() + offset);
    OSSynchronizeIO();
    return control;
}

bool AppleHDADmicBridge::setAppleHdaCaptureRouting(bool enable) noexcept {
    if (!enable && !captureRoutingOwned_) return true;
    const uint64_t ppOffset = hdaSnapshot_.ppCapabilityOffset;
    const uint64_t streamOffset = captureSetupPlan_.streamControlOffset;
    const uint32_t index = allocationPlan_.descriptorIndex;
    if (!controllerProviderReady() || !captureStreamLease_.active() ||
        !hdaWritableMap_ || !hdaWritableMap_->getVirtualAddress() ||
        (hdaWritableMap_->getMapOptions() & kIOMapReadOnly) != 0 ||
        ppOffset < 0x40 || (ppOffset & 3U) != 0 ||
        (hdaSnapshot_.ppCapabilityHeader & 0x0fff0000U) != 0x00030000U ||
        index >= hdaSnapshot_.inputStreamCount || index >= 32 ||
        streamOffset != 0x80U + index * 0x20U ||
        ppOffset + 8 > hdaWritableMap_->getLength() ||
        streamOffset + 4 > hdaWritableMap_->getLength())
        return false;
    auto *workLoop = appleHdaController_->getWorkLoop();
    if (!workLoop) return false;
    auto *base = reinterpret_cast<volatile uint32_t *>(hdaWritableMap_->getVirtualAddress());
    CaptureRoutingChange routing{base + (ppOffset + 4) / 4, base + streamOffset / 4,
        1U << index, allocationPlan_.streamTag, enable, captureRoutingOwned_};
    const IOReturn result = workLoop->runAction(changeCaptureRouting, this, &routing);
    captureRoutingOwned_ = routing.ownsBit;
    setProperty("Capture.AppleHDARoutingResult", static_cast<uint32_t>(result), 32);
    return result == kIOReturnSuccess;
}

bool AppleHDADmicBridge::clearPendingCaptureIocBeforeStart() noexcept {
    if (kAppleHdaOnlyMode) {
        uint32_t initialPosition = UINT32_MAX;
        const uint64_t completed = capturePositionTracker_.lastCompletedPeriodCount();
        if (!captureStreamLease_.active() ||
            !captureStreamLease_.linkPositionBytes(&initialPosition) ||
            initialPosition > captureRingBytes_) {
            setProperty("Capture.RestartPosition", initialPosition, 32);
            return false;
        }
        setProperty("Capture.RestartPosition", initialPosition, 32);
        setProperty("Capture.RestartCompletedPeriods", completed, 64);
        return capturePositionTracker_.rebase(initialPosition, completed);
    }
    uint32_t preControl = UINT32_MAX;
    if (hdaWritableMap_ && hdaWritableMap_->getVirtualAddress() != 0 &&
        static_cast<uint64_t>(captureSetupPlan_.streamControlOffset) +
                sizeof(uint32_t) <= hdaWritableMap_->getLength()) {
        OSSynchronizeIO();
        preControl = *reinterpret_cast<volatile uint32_t *>(
            hdaWritableMap_->getVirtualAddress() +
            captureSetupPlan_.streamControlOffset);
        OSSynchronizeIO();
    }
    setProperty("Capture.StartHdaPreControl", preControl, 32);
    uint8_t streamStatus = 0;
    if (!captureMmio_.readStreamStatus(captureSetupPlan_.streamControlOffset,
                                       &streamStatus)) {
        setProperty("Capture.StartHdaStatusReadSucceeded", false);
        return false;
    }
    setProperty("Capture.StartHdaPreStatus", streamStatus, 32);
    const uint8_t errorMask =
        sof::ipc4::kernel::kHdaStreamStatusFifoError |
        sof::ipc4::kernel::kHdaStreamStatusDescriptorError;
    if ((streamStatus & errorMask) != 0 ||
        (streamStatus & ~sof::ipc4::kernel::kHdaStreamStatusKnownMask) != 0)
        return false;

    if ((streamStatus &
         sof::ipc4::kernel::kHdaStreamStatusBufferCompletion) != 0) {
        sof::ipc4::kernel::HdaCaptureIocAck ack{};
        const auto observe = captureIocCounter_.observeStatus(streamStatus, &ack);
        setProperty("Capture.StartHdaIocObserveResult",
                    static_cast<uint32_t>(observe), 32);
        if (observe != sof::ipc4::kernel::HdaCaptureIocResult::AckRequired)
            return false;
        const bool ackSucceeded = captureMmio_.writeIocAck(
            captureSetupPlan_.streamControlOffset, ack);
        setProperty("Capture.StartHdaIocAckSucceeded", ackSucceeded);
        const auto ackResult = captureIocCounter_.confirmAck(ackSucceeded);
        setProperty("Capture.StartHdaIocAckResult",
                    static_cast<uint32_t>(ackResult), 32);
        if (ackResult != sof::ipc4::kernel::HdaCaptureIocResult::Acknowledged)
            return false;
        if (!captureMmio_.readStreamStatus(
                captureSetupPlan_.streamControlOffset, &streamStatus))
            return false;
    }
    setProperty("Capture.StartHdaPostAckStatus", streamStatus, 32);
    return (streamStatus &
            (sof::ipc4::kernel::kHdaStreamStatusBufferCompletion | errorMask)) == 0 &&
        (streamStatus & ~sof::ipc4::kernel::kHdaStreamStatusKnownMask) == 0;
}

void AppleHDADmicBridge::captureInterruptAction(
    void *context,
    sof::ipc4::dmic::kernel::CaptureInterruptServiceResult result,
    sof::ipc4::kernel::HdaCaptureTriggerAction action) {
    auto *controller = static_cast<AppleHDADmicBridge *>(context);
    if (controller) controller->handleCaptureInterrupt(result, action);
}

void AppleHDADmicBridge::handleCaptureInterrupt(
    sof::ipc4::dmic::kernel::CaptureInterruptServiceResult result,
    sof::ipc4::kernel::HdaCaptureTriggerAction action) noexcept {
    using sof::ipc4::dmic::kernel::CaptureInterruptServiceResult;
    const bool benign = result == CaptureInterruptServiceResult::PeriodPublished ||
        result == CaptureInterruptServiceResult::NoPeriodReady ||
        result == CaptureInterruptServiceResult::NoInterrupt;
    if (result == CaptureInterruptServiceResult::PeriodPublished &&
        !__atomic_exchange_n(&captureFirstFramesPublished_, true, __ATOMIC_RELAXED))
        setProperty("Capture.FirstPublishedFrames", captureRing_.writeFrames(), 64);
    if (benign)
        return;

    setProperty("Capture.Interrupt.ServiceResult", static_cast<uint32_t>(result), 32);
    setProperty("Capture.Interrupt.Action", static_cast<uint32_t>(action), 32);
    setProperty("Capture.Interrupt.StatusReadSucceeded",
                 captureInterruptService_.lastStatusReadSucceeded());
    setProperty("Capture.Interrupt.StreamStatus",
                 captureInterruptService_.lastStreamStatus(), 32);
    setProperty("Capture.Interrupt.PumpResult",
                 static_cast<uint32_t>(captureInterruptService_.lastPumpResult()), 32);
    setProperty("Capture.Interrupt.RuntimeReadResult",
                 capturePublisher_.lastRuntimeReadResult(), 32);
    setProperty("Capture.Interrupt.PublishResult",
                 capturePublisher_.lastPublishResult(), 32);
    setProperty("Capture.Interrupt.RingPublishSucceeded",
                 capturePublisher_.lastRingPublishSucceeded());
    setProperty("Capture.Interrupt.PositionResult",
                 captureRuntime_.lastPositionResult(), 32);
    setProperty("Capture.Interrupt.PositionWasCblEndpoint",
                 captureRuntime_.lastPositionWasCblEndpoint());
    setProperty("Capture.Interrupt.PositionProducedBytes",
                 captureRuntime_.positionProducedBytes(), 64);
    setProperty("Capture.Interrupt.PositionConsumedBytes",
                 captureRuntime_.positionConsumedBytes(), 64);
    setProperty("Capture.Interrupt.CompletedIocPeriods",
                 captureRuntime_.completedIocPeriods(), 64);
    setProperty("Capture.Interrupt.DMAReadStage", captureDma_.lastReadStage());
    setProperty("Capture.Interrupt.DMAReadPlanResult",
                 captureDma_.lastReadPlanResult(), 32);
    setProperty("Capture.Interrupt.DMAReadOffset",
                 captureDma_.lastReadOffset(), 64);
    setProperty("Capture.Interrupt.DMAReadFirstLength",
                 captureDma_.lastReadFirstLength(), 32);
    setProperty("Capture.Interrupt.DMAReadSecondLength",
                 captureDma_.lastReadSecondLength(), 32);
    setProperty("Capture.Interrupt.DMASyncResult",
                 captureDma_.lastReadSyncResult(), 32);
    setProperty("Capture.Interrupt.DMAReadFirstBytes",
                 captureDma_.lastReadFirstBytes(), 64);
    setProperty("Capture.Interrupt.DMAReadSecondBytes",
                 captureDma_.lastReadSecondBytes(), 64);
    setProperty("Capture.Interrupt.DMAReadCompletionResult",
                 captureDma_.lastReadCompletionResult(), 32);
    setProperty("Capture.Interrupt.AckSucceeded",
                 captureInterruptService_.lastAckSucceeded());
    setProperty("Capture.Interrupt.PositionReadSucceeded",
                 captureInterruptService_.lastPositionReadSucceeded());
    setProperty("Capture.Interrupt.LinkPosition",
                 captureInterruptService_.lastLinkPosition(), 32);
    if (captureInterruptBinding_) captureInterruptBinding_->disable();
    if (linkStreamPrepared_ && !kAppleHdaOnlyMode)
        setProperty("Capture.LinkInterruptRecoveryStopped", setLinkStreamRunning(false));
    if (action == sof::ipc4::kernel::HdaCaptureTriggerAction::None)
        action = captureRuntime_.beginRecovery();
    if (action == sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma) {
        uint32_t controlReadback = UINT32_MAX;
        const bool stopSucceeded = kAppleHdaOnlyMode
            ? captureStreamLease_.stopDmaEngine(1000) == kIOReturnSuccess
            : captureMmio_.executeStreamRunAction(
                sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma,
                captureSetupPlan_.streamControlOffset,
                captureStreamControlMask_, captureStreamControlValue_,
                &controlReadback);
        if (kAppleHdaOnlyMode && stopSucceeded) {
            controlReadback = readAppleHdaCaptureControl();
            (void)setAppleHdaCaptureRouting(false);
        }
        sof::ipc4::kernel::HdaCaptureTriggerAction recoveryAction{};
        (void)captureRuntime_.confirmRecoveryDmaStopped(
            controlReadback, stopSucceeded, &recoveryAction);
    }
    captureRing_.setStatus(kDmicAudioBridgeFailed);
    __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
    setProperty("Status", "capture interrupt failed; mappings retained");
    setProperty("Controller.Quarantined", true);
}

void AppleHDADmicBridge::stopCaptureAfterStartFailure() noexcept {
    if (captureInterruptBinding_) captureInterruptBinding_->disable();
    if (linkStreamPrepared_ && !kAppleHdaOnlyMode)
        setProperty("Capture.LinkRecoveryStopped", setLinkStreamRunning(false));
    sof::ipc4::kernel::HdaCaptureTriggerAction action =
        captureRuntime_.beginRecovery();
    if (action == sof::ipc4::kernel::HdaCaptureTriggerAction::None)
        (void)captureRuntime_.failCapture(&action);
    if (action == sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma) {
        uint32_t controlReadback = UINT32_MAX;
        const bool stopSucceeded = kAppleHdaOnlyMode
            ? captureStreamLease_.stopDmaEngine(1000) == kIOReturnSuccess
            : captureMmio_.executeStreamRunAction(
                sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma,
                captureSetupPlan_.streamControlOffset,
                captureStreamControlMask_, captureStreamControlValue_,
                &controlReadback);
        if (kAppleHdaOnlyMode && stopSucceeded) {
            controlReadback = readAppleHdaCaptureControl();
            (void)setAppleHdaCaptureRouting(false);
        }
        sof::ipc4::kernel::HdaCaptureTriggerAction recoveryAction{};
        (void)captureRuntime_.confirmRecoveryDmaStopped(
            controlReadback, stopSucceeded, &recoveryAction);
    }
}

bool AppleHDADmicBridge::handleIpc4Notification(
    void *context, const sof::ipc4::HeaderFields *header) noexcept {
    auto *controller = static_cast<AppleHDADmicBridge *>(context);
    if (!controller || !header || !header->notification) return false;
    // IPC4 notification type 6 signals a debug log position update; logs are
    // snapshotted from the debug window following a failed capture start.
    if (header->notificationType == 6 && controller->bootLifecycle_.canIssueIpc4()) return true;
    if (!header->fwReady) return false;
    if (controller->bootLifecycle_.phase() !=
        sof::ipc4::kernel::BootPhase::LoaderStopped)
        return false;
    __atomic_store_n(&controller->fwReadyNotificationSeen_, true,
                     __ATOMIC_RELEASE);
    return true;
}

bool AppleHDADmicBridge::controllerProviderReady() noexcept {
    return kAppleHdaOnlyMode && appleHdaController_ && pci_ &&
        appleHdaController_->getProvider() == pci_ &&
        !appleHdaController_->isInactive() && !pci_->isInactive();
}

bool AppleHDADmicBridge::ownerServicePresent(IOPCIDevice *device) noexcept {
    if (!device) return true;

    // Do not rely only on the current AppleHDA class name: any other active
    // client that still has the PCI provider open can own or touch the BARs.
    // getOpenClientIterator() locks each returned client for arbitration, so
    // ignore only this controller's own exclusive lease.
    OSIterator *openClients = device->getOpenClientIterator();
    if (!openClients) return true;
    bool foundOpenOwner = false;
    while (OSObject *object = openClients->getNextObject()) {
        IOService *client = OSDynamicCast(IOService, object);
        if (!client || client != this) {
            foundOpenOwner = true;
            break;
        }
    }
    openClients->release();
    if (foundOpenOwner) return true;

    // Keep the registry-tree check as a second guard for an active AppleHDA
    // service that may remain attached even when it no longer reports an
    // open-client relationship after kIOServiceSeize.
    OSDictionary *matching = IOService::serviceMatching("AppleHDAController");
    if (!matching) return true;
    OSIterator *iterator = IOService::getMatchingServices(matching);
    matching->release();
    // Tahoe may have no AppleHDAController class at all. The exclusive PCI
    // lease and open-client check above already guard against another owner.
    if (!iterator) return false;

    bool found = false;
    while (OSObject *object = iterator->getNextObject()) {
        IOService *service = OSDynamicCast(IOService, object);
        if (service && service->isInactive()) continue;
        for (IOService *node = service; node; node = node->getProvider()) {
            if (node == device) {
                found = true;
                break;
            }
        }
        if (found) break;
    }
    iterator->release();
    return found;
}

bool AppleHDADmicBridge::pciFunctionInD0(IOPCIDevice *device) noexcept {
    if (!device) return false;
    const uint16_t status = device->configRead16(kIOPCIConfigStatus);
    if (status == UINT16_MAX || (status & kIOPCIStatusCapabilities) == 0)
        return false;
    uint8_t offset = device->configRead8(0x34);
    for (uint8_t visited = 0; offset != 0 && visited < 48; ++visited) {
        if (offset < 0x40 || (offset & 3U) != 0) return false;
        const uint8_t id = device->configRead8(offset);
        const uint8_t next = device->configRead8(
            static_cast<uint8_t>(offset + 1U));
        if (id == 0xff || next == 0xff) return false;
        if (id == kIOPCIPowerManagementCapability) {
            const uint16_t pmcsr = device->configRead16(
                static_cast<uint8_t>(offset + 4U));
            return pmcsr != UINT16_MAX && (pmcsr & 0x3U) == 0;
        }
        if (next == offset) return false;
        offset = next;
    }
    return false;
}

bool AppleHDADmicBridge::releaseResources() noexcept {
    // Do not remove the only recovery/IOC worker while the IPC graph is still
    // initialized or the capture stream may still access its mapped buffers.
    // The caller must first complete the runtime teardown and independently
    // confirm HDA DMA quiescence; otherwise retain every live resource.
    const sof::ipc4::kernel::BootPhase bootPhase = bootLifecycle_.phase();
    const bool bootResourcesReleasable =
        bootPhase == sof::ipc4::kernel::BootPhase::Dormant ||
        bootPhase == sof::ipc4::kernel::BootPhase::PreflightAccepted ||
        bootPhase == sof::ipc4::kernel::BootPhase::DmaResourcesReady ||
        bootPhase == sof::ipc4::kernel::BootPhase::Failed;
    if (!bootResourcesReleasable || captureRuntime_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::Uninitialized ||
        !captureDma_.quiescentForRelease())
        return false;

    if (ipcTransportConfigured_) {
        if (!ipcTransport_.clearConfiguration()) return false;
        ipcTransportConfigured_ = false;
    }
    ipcAdapter_.reset();
    ipcIoOps_ = {};
    __atomic_store_n(&fwReadyNotificationSeen_, false, __ATOMIC_RELEASE);

    if (captureInterruptBinding_) {
        if (!captureInterruptBinding_->uninstall()) {
            setProperty("Status", "AppleHDA interrupt unregistration failed; binding retained");
            return false;
        }
        captureInterruptBinding_->release();
        captureInterruptBinding_ = nullptr;
    }
    captureMmio_.reset();
    if (dmaBuffersPrepared_) {
        if (!dmaResources_.releaseBeforeHardwareSubmission()) return false;
        dmaBuffersPrepared_ = false;
    }
    // Attempt cleanup even if prepare() returned false: it may have failed
    // partway through mapping and retained an IODMACommand or descriptor.
    if (!captureDma_.releaseAfterQuiescence()) return false;
    captureDmaPrepared_ = false;
    captureStreamLease_.release();
    loaderStreamLease_.release();
    if (captureStreamLease_.stream() || loaderStreamLease_.stream())
        return false;
    OSSafeReleaseNULL(appleHdaController_);
    OSSafeReleaseNULL(dspMap_);
    OSSafeReleaseNULL(dspWritableMap_);
    OSSafeReleaseNULL(hdaWritableMap_);
    OSSafeReleaseNULL(hdaMap_);
    providerOpen_ = false;
    pciCommandChanged_ = false;
    pci_ = nullptr;
    return true;
}

IOService *AppleHDADmicBridge::probe(IOService *provider, SInt32 *score) {
    auto *matched = IOService::probe(provider, score);
    if (!matched || !provider ||
        strcmp(provider->getMetaClass()->getClassName(), "AppleHDAController") != 0)
        return nullptr;
    auto *pci = OSDynamicCast(IOPCIDevice, provider->getProvider());
    if (!pci || pci->configRead32(kIOPCIConfigVendorID) != kTargetPciId ||
        pci->configRead16(kIOPCIConfigSubSystemVendorID) != kTargetSubsystemVendor ||
        pci->configRead16(kIOPCIConfigSubSystemID) != kTargetSubsystem)
        return nullptr;
    if (score) *score = 1000;
    return matched;
}

bool AppleHDADmicBridge::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    uint16_t command = 0;
    IODeviceMemory *hdaResource = nullptr;
    IODeviceMemory *dspResource = nullptr;
    IOService *appleHdaProvider = nullptr;
    IOPCIDevice *pciProvider = nullptr;
    sof::ipc4::kernel::BootPreflightEvidence preflight{};
    sof::ipc4::kernel::DmaResourceEvidence dmaEvidence{};
    const uint16_t requiredPciCommand = kIOPCICommandBusLead |
                                       kIOPCICommandMemorySpace;
    uint32_t firmwarePayloadBytes = 0;
    IOBufferMemoryDescriptor *loaderCyclicBuffer = nullptr;
    bool loaderStreamIdle = false;
    bool loaderDescriptorEmpty = false;
    uint32_t initialRomStatus = UINT32_MAX;
    uint32_t initialRomErrorOrFirmwareStatus = UINT32_MAX;
    uint8_t loaderIndex = 0;
    const sof::ipc4::kernel::DmaMappingEvidence *mapping = nullptr;
    const auto firmwareHashResult =
        sof::ipc4::kernel::verifyKnownAdlIpc4V212Image(
            sof::ipc4::kernel::sofFirmwareImage(),
            sof::ipc4::kernel::sofFirmwareImageBytes());
    if (firmwareHashResult !=
        sof::ipc4::kernel::FirmwareImageHashResult::Ok)
        return false;
    if (sof::ipc4::kernel::parseFirmwareManifest(
            sof::ipc4::kernel::sofFirmwareImage(),
            sof::ipc4::kernel::sofFirmwareImageBytes(),
            &firmwareManifest_) !=
        sof::ipc4::kernel::FirmwareManifestResult::Ok)
        return false;
    // This service is a child of AppleHDAController. AppleHDA remains the
    // sole IOPCIDevice client that owns the HDA controller and all HDA streams.
    appleHdaProvider = OSDynamicCast(IOService, provider);
    if (!appleHdaProvider ||
        strcmp(provider->getMetaClass()->getClassName(), "AppleHDAController") != 0)
        return false;
    pciProvider = OSDynamicCast(IOPCIDevice, appleHdaProvider->getProvider());
    if (!pciProvider || pciProvider->configRead32(kIOPCIConfigVendorID) != kTargetPciId ||
        pciProvider->configRead16(kIOPCIConfigSubSystemVendorID) != kTargetSubsystemVendor ||
        pciProvider->configRead16(kIOPCIConfigSubSystemID) != kTargetSubsystem ||
        appleHdaProvider->isInactive())
        return false;
    appleHdaController_ = appleHdaProvider;
    appleHdaController_->retain();
    pci_ = pciProvider;
    providerOpen_ = false;
    if (!AppleHDAStreamLease::apiAvailable()) goto fail;
    setProperty("Startup.FailureStage", "AppleHDA child provider validation");
    setProperty("Handoff.LeaseConfirmed", controllerProviderReady());
    setProperty("Handoff.AppleHDADmicChildMode", true);
    setProperty("Handoff.PciControllerSeized", false);
    setProperty("Handoff.OwnerStillPresent", ownerServicePresent(pci_));
    if (!controllerProviderReady()) goto fail;

    setProperty("Startup.FailureStage", "read-only BAR preflight");
    originalPciCommand_ = pci_->configRead16(kIOPCIConfigCommand);
    setProperty("PCI.CommandBeforeDecode", originalPciCommand_, 16);
    if (originalPciCommand_ == 0xffff) goto fail;
    if (kAppleHdaOnlyMode) {
        // Do not change PCI decode or bus-master state under AppleHDA.
        if ((originalPciCommand_ & requiredPciCommand) != requiredPciCommand)
            goto fail;
    } else {
        pciCommandChanged_ =
            (originalPciCommand_ & requiredPciCommand) != requiredPciCommand;
        if (pciCommandChanged_) {
            pci_->setMemoryEnable(true);
            pci_->setBusLeadEnable(true);
        }
    }
    command = pci_->configRead16(kIOPCIConfigCommand);
    setProperty("BAR.PCICommand", command, 16);
    setProperty("BAR.MemoryDecodeEnabled",
                command != 0xffff && (command & kIOPCICommandMemorySpace) != 0);
    if (command == 0xffff || (command & kIOPCICommandMemorySpace) == 0)
        goto fail;
    hdaResource = pci_->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0);
    dspResource = pci_->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress4);
    setProperty("BAR.HdaResourcePresent", hdaResource != nullptr);
    setProperty("BAR.DspResourcePresent", dspResource != nullptr);
    if (hdaResource) setProperty("BAR.HdaResourceBytes", hdaResource->getLength(), 64);
    if (dspResource) setProperty("BAR.DspResourceBytes", dspResource->getLength(), 64);
    if (!hdaResource || !dspResource ||
        hdaResource->getLength() < kHdaBarMinimum ||
        dspResource->getLength() < kDspBarMinimum)
        goto fail;
    hdaMap_ = pci_->mapDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0, kIOMapInhibitCache | kIOMapReadOnly);
    dspMap_ = pci_->mapDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress4, kIOMapInhibitCache | kIOMapReadOnly);
    setProperty("BAR.HdaReadOnlyMapPresent", hdaMap_ != nullptr);
    setProperty("BAR.DspReadOnlyMapPresent", dspMap_ != nullptr);
    if (hdaMap_) setProperty("BAR.HdaMappedBytes", hdaMap_->getLength(), 64);
    if (dspMap_) setProperty("BAR.DspMappedBytes", dspMap_->getLength(), 64);
    setProperty("BAR.HdaVirtualAddressPresent",
                hdaMap_ && hdaMap_->getVirtualAddress() != 0);
    setProperty("BAR.DspVirtualAddressPresent",
                dspMap_ && dspMap_->getVirtualAddress() != 0);
    if (!hdaMap_ || !dspMap_ || hdaMap_->getLength() < kHdaBarMinimum ||
        dspMap_->getLength() < kDspBarMinimum ||
        hdaMap_->getLength() != hdaResource->getLength() ||
        dspMap_->getLength() != dspResource->getLength())
        goto fail;

    // Keep the transport and its callback storage driver-owned. The current
    // service-start mapping is deliberately read-only, so this only prepares
    // the object graph; it cannot submit IPC or acknowledge a notification.
    setProperty("Startup.Substage", "IPC MMIO adapter configure");
    if (!ipcAdapter_.configure(pci_, this, dspMap_, &bootLifecycle_, false,
                               kAppleHdaOnlyMode ? appleHdaController_ : nullptr,
                               &AppleHDADmicBridge::handleIpc4Notification,
                               this))
        goto fail;
    ipcIoOps_ = ipcAdapter_.ioOps();
    setProperty("Startup.Substage", "IPC transport configure");
    if (!ipcTransport_.configure(
            &ipcIoOps_, sof::ipc4::kernel::kMaximumPayloadBytes,
            sof::ipc4::kernel::kMaximumPayloadBytes))
        goto fail;
    ipcTransportConfigured_ = true;

    // Record a read-only cAVS boot snapshot for diagnosis. This value is not
    // accepted as a lifecycle transition or FW_READY evidence.
    {
        setProperty("Startup.Substage", "read-only cAVS ROM status snapshot");
        sof::ipc4::kernel::Cavs25RomStatusReader reader(
            pci_, this, dspMap_,
            kAppleHdaOnlyMode && controllerProviderReady());
        if (!reader.readSnapshot(&initialRomStatus,
                                 &initialRomErrorOrFirmwareStatus))
            goto fail;
    }

    // This first live path is observation-only. The BAR maps above were
    // created read-only; no ROM command, firmware start, or stream DMA occurs.
    {
        setProperty("Startup.Substage", "read-only HDA controller snapshot");
        sof::ipc4::kernel::HdaControllerStatusReader reader(
            pci_, this, hdaMap_,
            kAppleHdaOnlyMode && controllerProviderReady());
        if (!reader.readSnapshot(&hdaSnapshot_)) goto fail;
    }
    setProperty("Startup.Substage", "HDA input stream allocation plan");
    if (sof::ipc4::kernel::planHdaInputStreamAllocation(
            hdaSnapshot_.globalCapabilities, hdaSnapshot_.inputStreams,
            hdaSnapshot_.inputStreamCount,
            hdaSnapshot_.allocatedStreamTagMask, true,
            &allocationPlan_) !=
        sof::ipc4::kernel::HdaInputStreamPlanResult::Ok)
        goto fail;
    setProperty("Startup.Substage", "HDA input reset plan");
    if (!inputResetSequence_.initialize(
            allocationPlan_,
            hdaSnapshot_.inputStreams[allocationPlan_.descriptorIndex].control,
            hdaMap_->getLength(), 1000, true))
        goto fail;
    setProperty("Startup.Substage", "HDA stream counts");
    if (hdaSnapshot_.outputStreamCount == 0 ||
        hdaSnapshot_.inputStreamCount >= hdaSnapshot_.totalStreamCount)
        goto fail;
    if (kAppleHdaOnlyMode) {
        // AppleHDA, not this client, selects the actual output descriptor and
        // tag. Require at least one free output stream in the read-only
        // snapshot, then validate the lease's actual assignment after allocate.
        codeLoaderPlan_ = {};
        codeLoaderPlanResult_ =
            sof::ipc4::kernel::CodeLoaderStreamPlanResult::Ok;
    } else {
        codeLoaderPlanResult_ =
            sof::ipc4::kernel::planCavs25CodeLoaderStreamReservation(
                hdaSnapshot_.globalCapabilities,
                hdaSnapshot_.inputStreamCount,
                hdaSnapshot_.outputStreamCount,
                hdaSnapshot_.totalStreamCount,
                hdaSnapshot_.allStreamControls,
                hdaSnapshot_.totalStreamCount,
                hdaSnapshot_.allocatedStreamTagMask,
                hdaSnapshot_.allStreamControls[
                    hdaSnapshot_.inputStreamCount].control,
                hdaMap_->getLength(), true, true, &codeLoaderPlan_);
    }
    setProperty("Startup.Substage", "code-loader stream plan");
    if (codeLoaderPlanResult_ !=
        sof::ipc4::kernel::CodeLoaderStreamPlanResult::Ok)
        goto fail;

    if (kAppleHdaOnlyMode) {
        for (uint8_t ordinal = 0;
             ordinal < hdaSnapshot_.outputStreamCount; ++ordinal) {
            const uint8_t index = static_cast<uint8_t>(
                hdaSnapshot_.inputStreamCount + ordinal);
            const auto &stream = hdaSnapshot_.allStreamControls[index];
            const bool idle = (stream.control &
                (sof::ipc4::kernel::kHdaStreamRunMask |
                 sof::ipc4::kernel::kHdaStreamResetMask |
                 sof::ipc4::kernel::kHdaStreamTagMask)) == 0;
            const bool empty = stream.status == 0 &&
                stream.cyclicBufferLength == 0 && stream.bdlLow == 0 &&
                stream.bdlHigh == 0 && stream.lastValidIndex == 0 &&
                stream.format == 0;
            if (idle && empty) {
                loaderStreamIdle = true;
                loaderDescriptorEmpty = true;
                break;
            }
        }
    } else {
        loaderStreamIdle =
            (hdaSnapshot_.allStreamControls[codeLoaderPlan_.descriptorIndex].control &
                (sof::ipc4::kernel::kHdaStreamRunMask |
                sof::ipc4::kernel::kHdaStreamResetMask |
                sof::ipc4::kernel::kHdaStreamTagMask)) == 0;
        loaderIndex = codeLoaderPlan_.descriptorIndex;
        loaderDescriptorEmpty =
            hdaSnapshot_.allStreamControls[loaderIndex].status == 0 &&
            hdaSnapshot_.allStreamControls[loaderIndex].cyclicBufferLength == 0 &&
            hdaSnapshot_.allStreamControls[loaderIndex].bdlLow == 0 &&
            hdaSnapshot_.allStreamControls[loaderIndex].bdlHigh == 0 &&
            hdaSnapshot_.allStreamControls[loaderIndex].lastValidIndex == 0 &&
            hdaSnapshot_.allStreamControls[loaderIndex].format == 0;
    }
    setProperty("Startup.Substage", "boot preflight evidence");
    preflight.firmwareImage =
        sof::ipc4::kernel::sofFirmwareImage();
    setProperty("Startup.FailureStage", "read-only controller snapshot");
    preflight.firmwareImageBytes =
        sof::ipc4::kernel::sofFirmwareImageBytes();
    preflight.firmwarePlatformMatched =
        firmwareManifest_.version[0] == 2 &&
        firmwareManifest_.version[1] == 12;
    preflight.exclusiveControllerLease =
        !kAppleHdaOnlyMode && pci_->isOpen(this);
    preflight.audioOwnerQuiesced = !ownerServicePresent(pci_);
    preflight.appleHdaStreamManagerAvailable = kAppleHdaOnlyMode;
    preflight.pciFunctionInD0 = pciFunctionInD0(pci_);
    preflight.memoryDecodeEnabled =
        (command & kIOPCICommandMemorySpace) != 0;
    preflight.busMasterEnabled = (command & kIOPCICommandBusLead) != 0;
    preflight.hdaControllerReady =
        (hdaSnapshot_.globalControl & 1U) != 0;
    preflight.dspRegisterWindowValid = dspMap_ &&
        dspMap_->getVirtualAddress() != 0 &&
        dspMap_->getLength() >= kDspBarMinimum;
    preflight.codeLoaderStreamIdle = loaderStreamIdle;
    preflight.codeLoaderDescriptorEmpty = loaderDescriptorEmpty;
    if (bootLifecycle_.acceptPreflight(preflight) !=
        sof::ipc4::kernel::BootResult::Ok)
        goto fail;

    dmaBuffersPrepared_ = true;
    firmwarePayloadBytes = static_cast<uint32_t>(
        sof::ipc4::kernel::sofFirmwareImageBytes() -
        firmwareManifest_.extensionLength);
    setProperty("Startup.FailureStage", "DMA allocation preflight");
    if (kAppleHdaOnlyMode) {
        setProperty("Startup.Substage", "AppleHDA output stream lease");
        const bool streamAllocated =
            loaderStreamLease_.allocate(appleHdaController_);
        setProperty("AppleHDA.LoaderStreamAllocated", streamAllocated);
        if (!streamAllocated)
            goto fail;
        setProperty("Startup.Substage", "AppleHDA output stream PCM configuration");
        // Use the firmware loader's SDnFMT 0x0040: 48 kHz, 32-bit mono.
        const IOReturn pcmResult = loaderStreamLease_.configurePcm(
            1, 0, 48000, false, 32, 1);
        setProperty("AppleHDA.LoaderStreamPcmResult",
                    static_cast<uint32_t>(pcmResult), 32);
        if (pcmResult != kIOReturnSuccess) goto fail;
        // AppleHDA 600.2 scales this by 12 pages per unit, halved for the
        // zero direction field of a newly allocated StreamInfo. 32 units
        // yield 192 pages, enough for the firmware and within its 256-entry BDL.
        constexpr uint32_t kLoaderAppleHdaAllocationUnits = 32;
        setProperty("Startup.Substage",
                    "AppleHDA preallocate firmware cyclic buffer");
        uint64_t preallocatedLoaderBytes = 0;
        const IOReturn preallocationResult = loaderStreamLease_.preallocateCyclicBuffer(
            kLoaderAppleHdaAllocationUnits, firmwarePayloadBytes,
            preallocatedLoaderBytes);
        const bool loaderBufferPreallocated = preallocationResult == kIOReturnSuccess;
        setProperty("AppleHDA.LoaderBufferPreallocated",
                    loaderBufferPreallocated);
        setProperty("AppleHDA.LoaderBufferAllocationUnits",
                    kLoaderAppleHdaAllocationUnits, 32);
        setProperty("AppleHDA.LoaderBufferAllocationResult",
                    static_cast<uint32_t>(preallocationResult), 32);
        setProperty("AppleHDA.LoaderPreallocatedBufferBytes",
                    preallocatedLoaderBytes, 64);
        if (!loaderBufferPreallocated) goto fail;
        setProperty("Startup.Substage", "AppleHDA output stream activation");
        const IOReturn activationResult =
            loaderStreamLease_.activate(1, 0, false);
        setProperty("AppleHDA.LoaderStreamActivationResult",
                    static_cast<uint32_t>(activationResult), 32);
        if (activationResult != kIOReturnSuccess) goto fail;
        setProperty("Startup.Substage", "AppleHDA output stream descriptor validation");
        uint8_t streamDescriptorIndex = UINT8_MAX;
        uint8_t streamTag = 0;
        if (!loaderStreamLease_.activeDescriptor(
                hdaSnapshot_.inputStreamCount,
                hdaSnapshot_.outputStreamCount,
                hdaSnapshot_.totalStreamCount,
                &streamDescriptorIndex, &streamTag))
            goto fail;
        setProperty("AppleHDA.LoaderStreamDescriptorIndex",
                    streamDescriptorIndex, 8);
        setProperty("AppleHDA.LoaderStreamTag", streamTag, 8);
        setProperty("Startup.Substage", "AppleHDA output cyclic buffer acquisition");
        loaderCyclicBuffer = OSDynamicCast(
            IOBufferMemoryDescriptor, loaderStreamLease_.cyclicBuffer());
        setProperty("AppleHDA.LoaderCyclicBufferPresent",
                    loaderCyclicBuffer != nullptr);
        if (!loaderCyclicBuffer) goto fail;
        setProperty("AppleHDA.LoaderCyclicBufferBytes",
                    loaderCyclicBuffer->getLength(), 64);
        setProperty("AppleHDA.LoaderFirmwarePayloadBytes",
                    firmwarePayloadBytes, 32);
        if (loaderCyclicBuffer->getLength() < firmwarePayloadBytes)
            goto fail;
        setProperty("Startup.Substage", "AppleHDA cyclic buffer DMA mapping");
        auto *loaderDmaCommand = loaderStreamLease_.cyclicBufferCommand(loaderCyclicBuffer);
        setProperty("AppleHDA.LoaderDmaCommandPresent", loaderDmaCommand != nullptr);
        if (!dmaResources_.prepareWithAppleHdaBuffer(
                pci_, this, loaderCyclicBuffer, loaderDmaCommand, firmwarePayloadBytes,
                kAppleHdaOnlyMode && controllerProviderReady())) {
            setProperty("Startup.DmaBuffer", dmaResources_.failureBuffer());
            setProperty("Startup.DmaOperation", dmaResources_.failureStage());
            goto fail;
        }
        setProperty("Boot.AppleHDALoaderDescriptor", streamDescriptorIndex, 8);
        setProperty("Boot.AppleHDALoaderStreamTag", streamTag, 8);
        setProperty("Boot.AppleHDALoaderCyclicBufferBytes",
                    loaderCyclicBuffer->getLength(), 64);
    } else {
        setProperty("Startup.Substage", "firmware and BDL IOVM mapping");
        if (!dmaResources_.prepare(pci_, this, firmwarePayloadBytes)) {
        setProperty("Startup.DeviceMapperPresent",
                    dmaResources_.deviceMapperFound());
        setProperty("Startup.DmaBuffer", dmaResources_.failureBuffer());
        setProperty("Startup.DmaOperation", dmaResources_.failureStage());
        setProperty("Startup.DmaSegmentOffset",
                    dmaResources_.lastSegmentOffset(), 64);
        setProperty("Startup.DmaSegmentBytes",
                    dmaResources_.lastSegmentBytes(), 64);
        setProperty("Startup.DmaRequiredBytes",
                    dmaResources_.lastRequiredBytes(), 64);
        setProperty("Startup.DmaSegmentCount",
                    dmaResources_.lastSegmentCount(), 32);
        goto fail;
        }
    }
    setProperty("Startup.DeviceMapperPresent",
                dmaResources_.deviceMapperFound());
    setProperty("Startup.Substage", "SOF firmware payload staging");
    if (!dmaResources_.stageFirmwarePayload(
            sof::ipc4::kernel::sofFirmwareImage() + firmwareManifest_.extensionLength,
            firmwarePayloadBytes)) {
        setProperty("Startup.DmaBuffer", dmaResources_.failureBuffer());
        setProperty("Startup.DmaOperation", dmaResources_.failureStage());
        goto fail;
    }
    setProperty("Startup.Substage", "code-loader BDL construction");
    if (!kAppleHdaOnlyMode && !dmaResources_.buildDescriptorList(&dmaPlan_)) {
        setProperty("Startup.DmaBuffer", dmaResources_.failureBuffer());
        setProperty("Startup.DmaOperation", dmaResources_.failureStage());
        goto fail;
    }
    setProperty("Startup.Substage", "DMA mapping evidence validation");
    mapping = dmaResources_.mappingEvidence();
    if (!mapping) {
        setProperty("Startup.DmaBuffer", "unknown");
        setProperty("Startup.DmaOperation", "mapping evidence unavailable");
        goto fail;
    }
    dmaEvidence.mapping = *mapping;
    dmaEvidence.firmwarePayloadBytes =
        (sof::ipc4::kernel::sofFirmwareImageBytes() - firmwareManifest_.extensionLength);
    dmaEvidence.appleHdaBufferReady =
        kAppleHdaOnlyMode && dmaResources_.appleHdaBufferReady();
    setProperty("Startup.Substage", "boot lifecycle DMA acceptance");
    if (bootLifecycle_.acceptDmaResources(dmaEvidence) !=
        sof::ipc4::kernel::BootResult::Ok) {
        setProperty("Startup.DmaBuffer", "boot lifecycle");
        setProperty("Startup.DmaOperation", "DMA evidence rejected");
        goto fail;
    }
    setProperty("Startup.Substage", "capture shared-ring initialization");
    if (!captureRing_.initialize()) goto fail;
    if (!kAppleHdaOnlyMode) {
        setProperty("Startup.Substage", "HDA capture DMA mapping");
        if (!captureDma_.prepare(
                pci_, this,
                sof::ipc4::dmic::kernel::kDmicCaptureCandidateRingBytes,
                sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodBytes,
                true)) {
        setProperty("Startup.CaptureDeviceMapperPresent",
                    captureDma_.deviceMapperFound());
        setProperty("Startup.DmaBuffer", captureDma_.failureBuffer());
        setProperty("Startup.DmaOperation", captureDma_.failureStage());
        setProperty("Startup.DmaSegmentOffset",
                    captureDma_.lastSegmentOffset(), 64);
        setProperty("Startup.DmaSegmentBytes",
                    captureDma_.lastSegmentBytes(), 64);
        setProperty("Startup.DmaRequiredBytes",
                    captureDma_.lastRequiredBytes(), 64);
        setProperty("Startup.DmaSegmentCount",
                    captureDma_.lastSegmentCount(), 32);
        goto fail;
        }
        captureDmaPrepared_ = true;
        captureRingBytes_ =
            sof::ipc4::dmic::kernel::kDmicCaptureCandidateRingBytes;
        setProperty("Startup.Substage", "capture IOC and position tracking");
        if (!captureIocCounter_.initialize(
                allocationPlan_.descriptorControlOffset, 0) ||
            !capturePositionTracker_.initialize(
                captureRingBytes_,
                sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodBytes,
                0, 0,
                sof::ipc4::dmic::kernel::kDmicCaptureCandidateMaximumQueuedBytes))
            goto fail;
        captureTrackingPrepared_ = true;
    }
    setProperty("Startup.Substage", "capture period publisher initialization");
    if (!capturePublisher_.initialize(
            &captureRing_,
            sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodBytes))
        goto fail;

    setProperty("Startup.FailureStage", "none");
    setProperty("Startup.Substage", "none");
    setProperty("ControllerLease", kAppleHdaOnlyMode
        ? "shared open; AppleHDA owns HDA stream registers and DMA"
        : "exclusive open with kIOServiceSeize");
    setProperty("HDA.GCAP", hdaSnapshot_.globalCapabilities, 16);
    setProperty("HDA.GCTL", hdaSnapshot_.globalControl, 32);
    setProperty("HDA.InputStreamCount", hdaSnapshot_.inputStreamCount, 8);
    setProperty("HDA.OutputStreamCount", hdaSnapshot_.outputStreamCount, 8);
    setProperty("HDA.BidirectionalStreamCount",
                hdaSnapshot_.bidirectionalStreamCount, 8);
    setProperty("HDA.TotalStreamCount", hdaSnapshot_.totalStreamCount, 8);
    setProperty("HDA.AllocatedStreamTagMask",
                hdaSnapshot_.allocatedStreamTagMask, 16);
    setProperty("HDA.PPCapabilityOffset", hdaSnapshot_.ppCapabilityOffset, 32);
    setProperty("HDA.PPCTL", hdaSnapshot_.ppctl, 32);
    setProperty("HDA.INTCTL", hdaSnapshot_.intctl, 32);
    setProperty("HDA.SPIBCapabilityPresent",
                hdaSnapshot_.spibCapabilityPresent);
    if (hdaSnapshot_.spibCapabilityPresent) {
        setProperty("HDA.SPIBCapabilityOffset",
                    hdaSnapshot_.spibCapabilityOffset, 32);
        setProperty("HDA.SPBFCCTL", hdaSnapshot_.spbfcctl, 32);
    }
    setProperty("HDA.CandidateInputDescriptor",
                allocationPlan_.descriptorIndex, 8);
    setProperty("HDA.CandidateStreamTag", allocationPlan_.streamTag, 8);
    setProperty("HDA.CandidateStreamControlOffset",
                allocationPlan_.descriptorControlOffset, 32);
    setProperty("HDA.InputAllocationPlanReady", true);
    setProperty("HDA.InputResetPlanReady", true);
    setProperty("HDA.InputResetComplete", inputResetSequence_.complete());
    setProperty("HDA.PostResetReservationPlanReady",
                postResetReservationReady_);
    setProperty("HDA.InputStreamReserved", false);
    if (hdaSnapshot_.outputStreamCount > 0) {
        setProperty("HDA.CandidateCodeLoaderStreamIndex",
                    hdaSnapshot_.inputStreamCount, 8);
        setProperty("HDA.CandidateCodeLoaderStreamControlOffset",
                    0x80U + static_cast<uint32_t>(
                        hdaSnapshot_.inputStreamCount) * 0x20U, 32);
        if (hdaSnapshot_.spibCapabilityPresent)
            setProperty("HDA.CodeLoaderSPIBPositionReadback",
                        hdaSnapshot_.spibPositions[
                            hdaSnapshot_.inputStreamCount], 32);
        if (hdaSnapshot_.spibCapabilityPresent) {
            const uint32_t streamBit = 1U << hdaSnapshot_.inputStreamCount;
            setProperty("HDA.CodeLoaderSPIBEnabled",
                        (hdaSnapshot_.spbfcctl & streamBit) != 0);
        }
        setProperty("HDA.CodeLoaderReservationPlanValid",
                    codeLoaderPlanResult_ ==
                        sof::ipc4::kernel::CodeLoaderStreamPlanResult::Ok);
        if (codeLoaderPlanResult_ ==
                sof::ipc4::kernel::CodeLoaderStreamPlanResult::Ok) {
            setProperty("HDA.CodeLoaderReservationTag",
                        codeLoaderPlan_.streamTag, 8);
            setProperty("HDA.CodeLoaderReservationCTLValue",
                        codeLoaderPlan_.controlWriteValue, 32);
            setProperty("HDA.CodeLoaderReservationReadbackMask",
                        codeLoaderPlan_.expectedReadbackMask, 32);
            setProperty("HDA.CodeLoaderReservationReadbackValue",
                        codeLoaderPlan_.expectedReadbackValue, 32);
        } else {
            setProperty("HDA.CodeLoaderReservationPlanResult",
                        static_cast<uint32_t>(codeLoaderPlanResult_), 32);
        }
    }
    setProperty("Status", "read-only controller snapshot; DSP and streams idle");
    setProperty("Capture.UserClientDisabledForDiagnostic", false);
    setProperty("Capture.UserClientEnabled", true);
    setProperty("Boot.InitialRomFSR", initialRomStatus, 32);
    setProperty("Boot.InitialRomErrorOrFirmwareStatus",
                initialRomErrorOrFirmwareStatus, 32);
    setProperty("SOF.FirmwareSHA256Verified", true);
    setProperty("SOF.FirmwareBytes",
                static_cast<uint32_t>(
                    sof::ipc4::kernel::sofFirmwareImageBytes()), 32);
    setProperty("SOF.ManifestModuleCount",
                firmwareManifest_.moduleCount, 32);
    setProperty("SOF.ManifestVersionMajor", firmwareManifest_.version[0], 16);
    setProperty("SOF.ManifestVersionMinor", firmwareManifest_.version[1], 16);
    setProperty("SOF.ManifestVersionMicro", firmwareManifest_.version[2], 16);
    setProperty("SOF.ManifestVersionBuild", firmwareManifest_.version[3], 16);
    setProperty("SOF.DmaBuffersPrepared", dmaBuffersPrepared_);
    setProperty("SOF.DmaPayloadBytes",
                static_cast<uint32_t>(
                    sof::ipc4::kernel::sofFirmwareImageBytes() - firmwareManifest_.extensionLength), 32);
    setProperty("SOF.DmaDescriptorCount", 2, 8);
    setProperty("SOF.DmaCyclicBufferLength",
                dmaPlan_.cyclicBufferLength, 32);
    setProperty("Boot.LifecyclePhase",
                static_cast<uint32_t>(bootLifecycle_.phase()), 32);
    setProperty("Boot.DmaResourcesReady",
                bootLifecycle_.phase() ==
                    sof::ipc4::kernel::BootPhase::DmaResourcesReady);
    setProperty("Boot.FirmwareMappingLive",
                mapping->firmwareMappingLive);
    setProperty("Boot.DescriptorMappingLive",
                mapping->descriptorMappingLive);
    setProperty("Boot.FirmwareContentsSynchronized",
                mapping->firmwareContentsSynchronized);
    setProperty("Boot.DescriptorContentsSynchronized",
                mapping->descriptorContentsSynchronized);
    setProperty("Capture.CandidatePeriodBytes",
                sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodBytes, 32);
    setProperty("Capture.CandidateHdaFormatWord",
                sof::ipc4::dmic::kernel::kDmicCaptureCandidateHdaFormatWord, 16);
    setProperty("Capture.CandidatePeriodCount",
                sof::ipc4::dmic::kernel::kDmicCaptureCandidatePeriodCount, 32);
    setProperty("Capture.CandidateRingBytes",
                sof::ipc4::dmic::kernel::kDmicCaptureCandidateRingBytes, 32);
    setProperty("Capture.HdaDmaPrepared", captureDma_.prepared());
    setProperty("Capture.IocCounterPrepared", captureTrackingPrepared_);
    setProperty("Capture.CandidateInputStreamOffset",
                allocationPlan_.descriptorControlOffset, 32);
    setProperty("Capture.PeriodPublisherPrepared",
                capturePublisher_.ready());
    setProperty("Capture.RuntimeInitialized",
                captureRuntime_.state() !=
                    sof::ipc4::kernel::HdaCaptureTriggerState::Uninitialized);
    setProperty("Capture.RuntimeState",
                static_cast<uint32_t>(captureRuntime_.state()), 32);
    setProperty("Capture.InterruptBindingInstalled", false);
    registerService();
    return true;

fail:
    {
        OSString *failureStageObject = OSDynamicCast(
            OSString, getProperty("Startup.FailureStage"));
        const char *failureStage = failureStageObject
            ? failureStageObject->getCStringNoCopy() : "<unset>";
        const uint16_t pciCommand = pci_
            ? pci_->configRead16(kIOPCIConfigCommand) : UINT16_MAX;
        IOLog("AppleHDADmicBridge: startup preflight failed stage=%s "
              "shared=%u providerReady=%u pciCommand=0x%04x "
              "hdaStreams=%u/%u dspMapped=%u\n",
              failureStage, kAppleHdaOnlyMode ? 1U : 0U,
              controllerProviderReady() ? 1U : 0U, pciCommand,
              hdaSnapshot_.inputStreamCount,
              hdaSnapshot_.outputStreamCount, dspMap_ ? 1U : 0U);
    }
    if (releaseResources()) {
        setProperty("ControllerLease", "released after failed preflight");
        setProperty("Status", "startup preflight failed; resources released");
        setProperty("Capture.UserClientDisabledForDiagnostic", false);
        setProperty("Capture.UserClientEnabled", true);
        registerService();
        return true;
    }
    // Keep the IOService and PCI lease alive but unpublished if any DMA
    // mapping could not be released. Closing BARs/provider ownership here
    // would make a retained device mapping unsafe.
    __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
    setProperty("Status", "resource release incomplete; controller quarantined");
    setProperty("Controller.Quarantined", true);
    return true;
}

void AppleHDADmicBridge::stop(IOService *provider) {
    if (!lifecycleLock_) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        setProperty("Controller.Quarantined", true);
        return;
    }
    IOLockLock(lifecycleLock_);
    if (!releaseResources()) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        setProperty("Status", "resource release incomplete; controller quarantined");
        setProperty("Controller.Quarantined", true);
        IOLockUnlock(lifecycleLock_);
        return;
    }
    captureRing_.setStatus(kDmicAudioBridgeStopped);
    __atomic_store_n(&quarantined_, false, __ATOMIC_RELEASE);
    IOLockUnlock(lifecycleLock_);
    IOService::stop(provider);
}

IOReturn AppleHDADmicBridge::startPublishedCapture() noexcept {
    if (!lifecycleLock_) return kIOReturnNotReady;
    IOLockLock(lifecycleLock_);
    using sof::ipc4::kernel::HdaCaptureTriggerState;
    const auto initialState = captureRuntime_.state();
    const bool reuseAppleHdaCapture =
        kAppleHdaOnlyMode && initialState == HdaCaptureTriggerState::Stopped;
    IOReturn result = kIOReturnNotReady;
    sof::ipc4::kernel::HdaCaptureTriggerAction action{};
    uint32_t controlReadback = UINT32_MAX;
    bool hdaStarted = false;
    bool positionRead = false;
    uint32_t linkPosition = UINT32_MAX;
    sof::ipc4::kernel::TransportResult transportResult{};
    bool captureStartArmed = false;
    if (__atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE))
        goto done;
    if (initialState == HdaCaptureTriggerState::Running) {
        result = kIOReturnSuccess;
        goto done;
    }
    if (!bootLifecycle_.canIssueIpc4() &&
        !advanceBootToFirmwareReady()) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        setProperty("Boot.LifecyclePhase",
                    static_cast<uint32_t>(bootLifecycle_.phase()), 32);
        setProperty("Status", "firmware boot/FW_READY transition failed");
        setProperty("Controller.Quarantined", true);
        goto done;
    }
    if (!bootLifecycle_.canIssueIpc4()) goto done;
    if (reuseAppleHdaCapture) {
        setProperty("Capture.Stage", "reuse stopped AppleHDA input stream");
        const uint32_t stoppedControl = readAppleHdaCaptureControl();
        const uint32_t expectedControl = captureStreamControlValue_ & ~(1U << 1);
        if (!controllerProviderReady() || !captureStreamLease_.active() ||
            captureRoutingOwned_ || !captureDmaPrepared_ ||
            !captureDma_.prepared() || !capturePlanReady_ ||
            !captureSetupPlanReady_ || !ipcTransportConfigured_ ||
            !captureSession_.setupComplete() || !captureInterruptBinding_ ||
            stoppedControl == UINT32_MAX ||
            (stoppedControl & captureStreamControlMask_) != expectedControl) {
            setProperty("Capture.RestartStoppedControl", stoppedControl, 32);
            setProperty("Status", "stopped capture resources unavailable");
            goto done;
        }
        // AppleHDA resets the host DMA cursor on each start. Reset the SOF
        // pipelines so its host component drops the previous transfer state too.
        setProperty("Capture.Stage", "reset stopped DSP capture session");
        if (!resetStoppedCaptureSession()) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            setProperty("Status", "DSP capture session reset failed");
            setProperty("Controller.Quarantined", true);
            goto done;
        }
    } else if ((setProperty("Capture.Stage", "input HDA preparation"), !prepareInputCaptureHda()) ||
        (setProperty("Capture.Stage", "capture plan"), !capturePlanReady_) ||
        (setProperty("Capture.Stage", "IPC transport"), !prepareFirmwareReadyTransport()) ||
        (setProperty("Capture.Stage", "firmware tracing"), !enableFirmwareTrace()) ||
        (setProperty("Capture.Stage", "IPC session"), !initializeCaptureSession()) ||
        (setProperty("Capture.Stage", "capture runtime"), !initializeCaptureRuntime()) ||
        (setProperty("Capture.Stage", "HDA setup"), !programCaptureHdaSetup()) ||
        (setProperty("Capture.Stage", "link stream setup"), !prepareLinkStream()) ||
        (setProperty("Capture.Stage", "interrupt binding"), !installCaptureInterruptBinding())) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        setProperty("Status", "IPC4 session/runtime initialization failed");
        setProperty("Controller.Quarantined", true);
        goto done;
    }
    setProperty("Capture.ReusedStoppedStream", reuseAppleHdaCapture);
    // Open the generation before starting HDA or enabling IOC delivery.
    // HDA can accumulate an IOC while the IPC pipeline is being started; the
    // deferred worker must already be able to publish that first period.
    setProperty("Capture.StartStage", "ring generation");
    __atomic_store_n(&captureFirstFramesPublished_, false, __ATOMIC_RELAXED);
    removeProperty("Capture.FirstPublishedFrames");
    if (!captureRing_.beginCapture()) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        setProperty("Status", "shared capture ring could not start a generation");
        setProperty("Controller.Quarantined", true);
        goto done;
    }
    setProperty("Capture.StartStage", "runtime begin");
    if (captureRuntime_.beginStart(&action) !=
            sof::ipc4::dmic::kernel::CaptureRuntimeResult::Ok) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        goto done;
    }
    captureStartArmed = true;
    setProperty("Capture.StartStage", "period pump reset");
    if (action != sof::ipc4::kernel::HdaCaptureTriggerAction::StartHdaDma ||
        !capturePeriodPump_.resetForStart()) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        goto done;
    }
    setProperty("Capture.StartStage", "HDA DMA start");
    if (!clearPendingCaptureIocBeforeStart()) {
        setProperty("Capture.StartHdaResult", false);
        setProperty("Status", kAppleHdaOnlyMode
            ? "AppleHDA position baseline rejected before DMA start"
            : "HDA stream has stale/error status before DMA start");
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        goto done;
    }
    if (kAppleHdaOnlyMode) {
        if (setAppleHdaCaptureRouting(true)) {
            const IOReturn dmaResult = captureStreamLease_.startDmaEngine(1000);
            setProperty("Capture.AppleHDAStartDmaReturn", static_cast<uint32_t>(dmaResult), 32);
            hdaStarted = dmaResult == kIOReturnSuccess;
        }
        // The DSP pipeline is paused here. Confirm RUN and tag now, and
        // arm the position tracker before asking the DSP to produce audio.
        controlReadback = readAppleHdaCaptureControl();
    } else {
        hdaStarted = captureMmio_.executeStreamRunAction(
            action, captureSetupPlan_.streamControlOffset,
            captureStreamControlMask_, captureStreamControlValue_,
            &controlReadback);
    }
    setProperty("Capture.StartHdaResult", hdaStarted);
    setProperty("Capture.StartHdaControl", controlReadback, 32);
    if (captureRuntime_.confirmHdaStarted(
            hdaStarted, controlReadback, &action) !=
            sof::ipc4::dmic::kernel::CaptureRuntimeResult::Ok ||
        action != sof::ipc4::kernel::HdaCaptureTriggerAction::SetPipelineRunning) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        goto done;
    }
    setProperty("Capture.StartStage", "position baseline");
    positionRead = kAppleHdaOnlyMode
        ? captureStreamLease_.linkPositionBytes(&linkPosition)
        : captureMmio_.readLinkPosition(
            captureSetupPlan_.streamControlOffset, &linkPosition);
    if (!positionRead ||
        capturePeriodPump_.armPositionBaseline(linkPosition, &action) !=
            sof::ipc4::dmic::kernel::CapturePumpResult::BaselineArmed) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        goto done;
    }
    setProperty("Capture.StartStage", "link DMA start");
    if (!kAppleHdaOnlyMode && !setLinkStreamRunning(true)) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        goto done;
    }
    setProperty("Capture.StartStage", "pipeline and interrupt enable");
    auditMultiLink(false);
    drainFirmwareTrace();
    {
        const auto pipelineResult = captureRuntime_.startPipeline(
            &action, &transportResult, kIpcTimeoutMilliseconds,
            kIpcPollIntervalMicroseconds);
        auditMultiLink(true);
        settleFirmwareTrace();
        snapshotFirmwareTrace();
        setProperty("Capture.PipelineRequestPrimary", ipcTransport_.lastRequestPrimary(), 32);
        setProperty("Capture.PipelineRequestExtension", ipcTransport_.lastRequestExtension(), 32);
        setProperty("Capture.PipelineRequestPayloadBytes", ipcTransport_.lastRequestPayloadBytes(), 32);
        setProperty("Capture.PipelineRequestPayloadWord0", ipcTransport_.lastRequestPayloadWord(0), 32);
        setProperty("Capture.PipelineRequestPayloadWord1", ipcTransport_.lastRequestPayloadWord(1), 32);
        setProperty("Capture.PipelineRequestPayloadWord2", ipcTransport_.lastRequestPayloadWord(2), 32);
        setProperty("Capture.PipelineStartResult", static_cast<uint32_t>(pipelineResult), 32);
        setProperty("Capture.PipelineTransportResult", static_cast<uint32_t>(transportResult), 32);
        setProperty("Capture.PipelineReplyPrimary", ipcTransport_.lastReplyPrimary(), 32);
        setProperty("Capture.PipelineReplyExtension", ipcTransport_.lastReplyExtension(), 32);
        setProperty("Capture.PipelineNextAction", static_cast<uint32_t>(action), 32);
        if (pipelineResult != sof::ipc4::dmic::kernel::CaptureRuntimeResult::Ok ||
            action != sof::ipc4::kernel::HdaCaptureTriggerAction::None || !captureInterruptBinding_) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            goto done;
        }
        setProperty("Capture.StartStage", "interrupt position baseline");
        if (!rebaseCapturePositionBeforeInterruptEnable()) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            setProperty("Status", "pre-interrupt IOC/LPIB baseline failed");
            setProperty("Controller.Quarantined", true);
            goto done;
        }
        setProperty("Capture.StartStage", "interrupt enable");
        const IOReturn interruptResult = captureInterruptBinding_->enable();
        setProperty("Capture.InterruptEnableResult", static_cast<uint32_t>(interruptResult), 32);
        if (interruptResult != kIOReturnSuccess) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            goto done;
        }
    }
    setProperty("Capture.StartStage", "running");
    setProperty("Status", "capture running");
    result = kIOReturnSuccess;
done:
    if (captureStartArmed && result != kIOReturnSuccess &&
        __atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE))
        stopCaptureAfterStartFailure();
    IOLockUnlock(lifecycleLock_);
    return result;
}

IOReturn AppleHDADmicBridge::stopPublishedCapture() noexcept {
    if (!lifecycleLock_) return kIOReturnNotReady;
    IOLockLock(lifecycleLock_);
    IOReturn result = kIOReturnNotReady;
    using sof::ipc4::kernel::HdaCaptureTriggerAction;
    using sof::ipc4::kernel::HdaCaptureTriggerState;
    using sof::ipc4::dmic::kernel::CaptureRuntimeResult;
    bool captureWorkerLocked = false;
    if (__atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE))
        goto done;

    if (captureRuntime_.state() == HdaCaptureTriggerState::Uninitialized ||
        captureRuntime_.state() == HdaCaptureTriggerState::Ready ||
        captureRuntime_.state() == HdaCaptureTriggerState::Stopped) {
        captureRing_.setStatus(kDmicAudioBridgeStopped);
        result = kIOReturnSuccess;
        goto done;
    }
    if (captureRuntime_.state() != HdaCaptureTriggerState::Running ||
        !bootLifecycle_.canIssueIpc4() || !captureInterruptBinding_ ||
        !captureSetupPlanReady_ || !controllerProviderReady() ||
        (!kAppleHdaOnlyMode && ownerServicePresent(pci_)))
        goto done;

    // Stop accepting IOC work before the IPC4 pause and HDA RUN drain fence.
    captureInterruptBinding_->disable();
    captureWorkerLocked = captureInterruptBinding_->lockForControl();
    if (!captureWorkerLocked) {
        __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
        captureRing_.setStatus(kDmicAudioBridgeFailed);
        setProperty("Status", "interrupt worker did not quiesce");
        setProperty("Controller.Quarantined", true);
        goto done;
    }
    {
        HdaCaptureTriggerAction action{};
        sof::ipc4::kernel::TransportResult transportResult{};
        const CaptureRuntimeResult pauseResult = captureRuntime_.beginStop(
            &action, &transportResult, kIpcTimeoutMilliseconds,
            kIpcPollIntervalMicroseconds);
        if (pauseResult != CaptureRuntimeResult::Ok ||
            action != HdaCaptureTriggerAction::StopHdaDma) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            setProperty("Status", "pipeline pause failed; resources retained");
            setProperty("Controller.Quarantined", true);
            stopCaptureAfterStartFailure();
            goto done;
        }

        if (!kAppleHdaOnlyMode && !setLinkStreamRunning(false)) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            setProperty("Status", "link DMA stop failed; resources retained");
            setProperty("Controller.Quarantined", true);
            goto done;
        }
        uint32_t controlReadback = UINT32_MAX;
        bool stopped = kAppleHdaOnlyMode
            ? captureStreamLease_.stopDmaEngine(1000) == kIOReturnSuccess
            : captureMmio_.executeStreamRunAction(
                action, captureSetupPlan_.streamControlOffset,
                captureStreamControlMask_, captureStreamControlValue_,
                &controlReadback);
        if (kAppleHdaOnlyMode && stopped) {
            uint32_t previousPosition = UINT32_MAX;
            bool positionStable = false;
            if (captureStreamLease_.linkPositionBytes(&previousPosition)) {
                for (uint32_t poll = 0; poll < 20; ++poll) {
                    IODelay(1000);
                    uint32_t currentPosition = UINT32_MAX;
                    if (!captureStreamLease_.linkPositionBytes(&currentPosition))
                        break;
                    if (currentPosition == previousPosition) {
                        positionStable = true;
                        break;
                    }
                    previousPosition = currentPosition;
                }
            }
            stopped = positionStable;
        }
        if (kAppleHdaOnlyMode) {
            controlReadback = readAppleHdaCaptureControl();
            stopped = stopped && setAppleHdaCaptureRouting(false);
        }
        const CaptureRuntimeResult stopResult =
            captureRuntime_.confirmHdaStopped(
                stopped, controlReadback, stopped, &action);
        if (stopResult != CaptureRuntimeResult::Complete ||
            action != HdaCaptureTriggerAction::None) {
            __atomic_store_n(&quarantined_, true, __ATOMIC_RELEASE);
            captureRing_.setStatus(kDmicAudioBridgeFailed);
            setProperty("Status", "HDA stop fence failed; resources retained");
            setProperty("Controller.Quarantined", true);
            goto done;
        }
    }
    captureRing_.setStatus(kDmicAudioBridgeStopped);
    setProperty("Status", "capture stopped; input stream available for restart");
    result = kIOReturnSuccess;
done:
    if (captureWorkerLocked)
        captureInterruptBinding_->unlockAfterControl();
    IOLockUnlock(lifecycleLock_);
    return result;
}

void AppleHDADmicBridge::quarantineCaptureClientLoss() noexcept {
    if (lifecycleLock_) IOLockLock(lifecycleLock_);
    const bool wasAlreadyQuarantined = __atomic_exchange_n(
        &quarantined_, true, __ATOMIC_ACQ_REL);
    captureRing_.setStatus(kDmicAudioBridgeFailed);
    if (wasAlreadyQuarantined)
        setProperty("Capture.ClientClosedAfterQuarantine", true);
    else
        setProperty("Status", "capture client closed before stop was confirmed");
    setProperty("Controller.Quarantined", true);
    if (lifecycleLock_) IOLockUnlock(lifecycleLock_);
}

IOReturn AppleHDADmicBridge::newUserClient(
    task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (__atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    const bool isLocalUser = IOUserClient::clientHasPrivilege(
        securityID, kIOClientPrivilegeLocalUser) == kIOReturnSuccess;
    OSObject *audioHostEntitlement = IOUserClient::copyClientEntitlement(
        owningTask, "com.apple.private.audio.driver-host");
    const bool isAppleAudioDriverHost =
        OSDynamicCast(OSBoolean, audioHostEntitlement) == kOSBooleanTrue;
    if (audioHostEntitlement) audioHostEntitlement->release();

    // AudioServerPlugIns run inside Apple's Core-Audio-Driver-Service XPC
    // host, whose dedicated system UID is not the logged-in console user.
    // Accept that host only through Apple's signing entitlement; ordinary
    // callers still need the local-user privilege.
    if (type != 0 || !sharedCaptureRing() ||
        (!isLocalUser && !isAppleAudioDriverHost))
        return kIOReturnNotPermitted;
    if (!lifecycleLock_) return kIOReturnNotReady;
    IOLockLock(lifecycleLock_);
    if (__atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE)) {
        IOLockUnlock(lifecycleLock_);
        return kIOReturnNotReady;
    }

    bool expectedOpen = false;
    if (!__atomic_compare_exchange_n(&userClientOpen_, &expectedOpen, true,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        IOLockUnlock(lifecycleLock_);
        return kIOReturnExclusiveAccess;
    }

    auto *client = new HPIPC4AudioUserClient;
    if (!client || !client->initWithTask(owningTask, securityID, type,
                                        properties) ||
        !client->attach(this) || !client->start(this)) {
        if (client) client->release();
        releaseUserClientLease();
        IOLockUnlock(lifecycleLock_);
        return kIOReturnNoResources;
    }
    *handler = client;
    IOLockUnlock(lifecycleLock_);
    return kIOReturnSuccess;
}

bool HPIPC4AudioUserClient::initWithTask(
    task_t owningTask, void *securityToken, UInt32 type,
    OSDictionary *properties) {
    controller_ = nullptr;
    leaseHeld_ = false;
    return IOUserClient::initWithTask(owningTask, securityToken, type,
                                      properties);
}

bool HPIPC4AudioUserClient::start(IOService *provider) {
    if (!IOUserClient::start(provider)) return false;
    controller_ = OSDynamicCast(AppleHDADmicBridge, provider);
    leaseHeld_ = controller_ != nullptr;
    return leaseHeld_;
}

void HPIPC4AudioUserClient::stop(IOService *provider) {
    if (controller_ &&
        controller_->stopPublishedCapture() != kIOReturnSuccess)
        controller_->quarantineCaptureClientLoss();
    if (controller_ && leaseHeld_) controller_->releaseUserClientLease();
    leaseHeld_ = false;
    controller_ = nullptr;
    IOUserClient::stop(provider);
}

IOReturn HPIPC4AudioUserClient::clientClose() {
    if (controller_ &&
        controller_->stopPublishedCapture() != kIOReturnSuccess)
        controller_->quarantineCaptureClientLoss();
    if (controller_ && leaseHeld_) controller_->releaseUserClientLease();
    leaseHeld_ = false;
    controller_ = nullptr;
    terminate();
    return kIOReturnSuccess;
}

IOReturn HPIPC4AudioUserClient::clientMemoryForType(
    UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (!options || !memory || type != DMIC_AUDIO_BRIDGE_MAPPING_TYPE ||
        !controller_)
        return kIOReturnBadArgument;
    *memory = nullptr;
    *options = kIOMapReadOnly;
    IOBufferMemoryDescriptor *ring = controller_->sharedCaptureRing();
    if (!ring) return kIOReturnNotReady;
    ring->retain();
    *memory = ring;
    return kIOReturnSuccess;
}

const IOExternalMethodDispatch HPIPC4AudioUserClient::methods_[
    kDmicAudioBridgeSelectorCount] = {
        {&HPIPC4AudioUserClient::startCaptureAction, 0, 0, 0, 0},
        {&HPIPC4AudioUserClient::stopCaptureAction, 0, 0, 0, 0},
        {&HPIPC4AudioUserClient::getInfoAction, 0, 0, 3, 0},
};

IOReturn HPIPC4AudioUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector >= kDmicAudioBridgeSelectorCount)
        return kIOReturnBadArgument;
    return IOUserClient::externalMethod(selector, arguments,
        const_cast<IOExternalMethodDispatch *>(&methods_[selector]), this,
        nullptr);
}

IOReturn HPIPC4AudioUserClient::startCaptureAction(
    OSObject *target, void *, IOExternalMethodArguments *) {
    auto *client = OSDynamicCast(HPIPC4AudioUserClient, target);
    return client && client->controller_
        ? client->controller_->startPublishedCapture()
        : kIOReturnNotReady;
}

IOReturn HPIPC4AudioUserClient::stopCaptureAction(
    OSObject *target, void *, IOExternalMethodArguments *) {
    auto *client = OSDynamicCast(HPIPC4AudioUserClient, target);
    return client && client->controller_
        ? client->controller_->stopPublishedCapture()
        : kIOReturnNotReady;
}

IOReturn HPIPC4AudioUserClient::getInfoAction(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *client = OSDynamicCast(HPIPC4AudioUserClient, target);
    if (!client || !client->controller_ || !arguments ||
        arguments->scalarOutputCount < 3)
        return kIOReturnBadArgument;
    arguments->scalarOutput[0] = DMIC_AUDIO_BRIDGE_ABI_VERSION;
    arguments->scalarOutput[1] = DMIC_AUDIO_BRIDGE_SAMPLE_RATE;
    arguments->scalarOutput[2] = DMIC_AUDIO_BRIDGE_RING_FRAMES;
    return kIOReturnSuccess;
}

