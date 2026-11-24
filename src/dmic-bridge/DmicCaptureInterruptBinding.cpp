#include "DmicCaptureInterruptBinding.hpp"

using sof::ipc4::dmic::kernel::CaptureInterruptActionHandler;
using sof::ipc4::dmic::kernel::CaptureInterruptServiceResult;
using sof::ipc4::dmic::kernel::DmicCaptureInterruptService;

OSDefineMetaClassAndStructors(DmicCaptureInterruptBinding, OSObject)

bool DmicCaptureInterruptBinding::init() {
    if (!OSObject::init()) return false;
    service_ = nullptr;
    actionHandler_ = nullptr;
    actionContext_ = nullptr;
    interruptSource_ = nullptr;
    appleHdaPollTimer_ = nullptr;
    appleHdaLease_ = nullptr;
    workLoop_ = nullptr;
    threadCall_ = nullptr;
    workerLock_ = IOLockAlloc();
    configured_ = false;
    installed_ = false;
    enabled_ = false;
    if (!workerLock_) return false;
    threadCall_ = thread_call_allocate_with_priority(
        &DmicCaptureInterruptBinding::threadCallThunk, this,
        THREAD_CALL_PRIORITY_KERNEL);
    if (!threadCall_) {
        IOLockFree(workerLock_);
        workerLock_ = nullptr;
        return false;
    }
    return true;
}

bool DmicCaptureInterruptBinding::configure(
    DmicCaptureInterruptService *service,
    CaptureInterruptActionHandler actionHandler,
    void *actionContext) noexcept {
    if (configured_ || installed_ || !service || !service->ready() ||
        !actionHandler)
        return false;
    service_ = service;
    actionHandler_ = actionHandler;
    actionContext_ = actionContext;
    configured_ = true;
    return true;
}

IOReturn DmicCaptureInterruptBinding::install(
    IOService *interruptProvider, IOWorkLoop *workLoop,
    int interruptIndex) noexcept {
    if (!configured_ || installed_ || !interruptProvider || !workLoop ||
        interruptIndex < 0 || !threadCall_ || !workerLock_)
        return kIOReturnBadArgument;

    interruptSource_ = IOInterruptEventSource::interruptEventSource(
        this, &DmicCaptureInterruptBinding::interruptThunk,
        interruptProvider, interruptIndex);
    if (!interruptSource_) return kIOReturnNoResources;

    workLoop_ = workLoop;
    workLoop_->retain();
    const IOReturn addResult = workLoop_->addEventSource(interruptSource_);
    if (addResult != kIOReturnSuccess) {
        workLoop_->release();
        workLoop_ = nullptr;
        OSSafeReleaseNULL(interruptSource_);
        return addResult;
    }

    __atomic_store_n(&installed_, true, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

IOReturn DmicCaptureInterruptBinding::installAppleHda(
    AppleHDAStreamLease *lease, IOWorkLoop *workLoop) noexcept {
    if (!configured_ || installed_ || !lease || !lease->active() || !workLoop ||
        !threadCall_ || !workerLock_)
        return kIOReturnBadArgument;
    appleHdaPollTimer_ = IOTimerEventSource::timerEventSource(
        this, &DmicCaptureInterruptBinding::appleHdaPollTimerThunk);
    if (!appleHdaPollTimer_) return kIOReturnNoResources;
    workLoop_ = workLoop;
    workLoop_->retain();
    const IOReturn addResult = workLoop_->addEventSource(appleHdaPollTimer_);
    if (addResult != kIOReturnSuccess) {
        workLoop_->release();
        workLoop_ = nullptr;
        OSSafeReleaseNULL(appleHdaPollTimer_);
        return addResult;
    }
    const IOReturn result = lease->registerInterrupt(
        this, &DmicCaptureInterruptBinding::appleHdaInterruptThunk,
        lease->stream(), 0);
    if (result != kIOReturnSuccess) {
        workLoop_->removeEventSource(appleHdaPollTimer_);
        OSSafeReleaseNULL(appleHdaPollTimer_);
        OSSafeReleaseNULL(workLoop_);
        return result;
    }
    appleHdaLease_ = lease;
    __atomic_store_n(&installed_, true, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

IOReturn DmicCaptureInterruptBinding::enable() noexcept {
    if (!configured_ || !__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) ||
        (!interruptSource_ && !appleHdaLease_) ||
        __atomic_load_n(&enabled_, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    __atomic_store_n(&enabled_, true, __ATOMIC_RELEASE);
    if (interruptSource_) interruptSource_->enable();
    if (appleHdaPollTimer_) appleHdaPollTimer_->setTimeoutMS(1);
    return kIOReturnSuccess;
}

bool DmicCaptureInterruptBinding::lockForControl() noexcept {
    if (!configured_ || !__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) ||
        !workerLock_)
        return false;
    IOLockLock(workerLock_);
    return true;
}

void DmicCaptureInterruptBinding::unlockAfterControl() noexcept {
    if (workerLock_) IOLockUnlock(workerLock_);
}

void DmicCaptureInterruptBinding::disable() noexcept {
    if (interruptSource_ && __atomic_load_n(&enabled_, __ATOMIC_ACQUIRE))
        interruptSource_->disable();
    __atomic_store_n(&enabled_, false, __ATOMIC_RELEASE);
    if (appleHdaPollTimer_) appleHdaPollTimer_->cancelTimeout();
}

bool DmicCaptureInterruptBinding::uninstall() noexcept {
    disable();
    if (appleHdaLease_) {
        if (appleHdaLease_->unregisterInterrupt() != kIOReturnSuccess)
            return false;
        appleHdaLease_ = nullptr;
    }
    if (__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) && workLoop_ &&
        appleHdaPollTimer_)
        workLoop_->removeEventSource(appleHdaPollTimer_);
    if (__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) && workLoop_ &&
        interruptSource_)
        workLoop_->removeEventSource(interruptSource_);
    __atomic_store_n(&installed_, false, __ATOMIC_RELEASE);
    if (workerLock_) IOLockLock(workerLock_);
    if (workerLock_) IOLockUnlock(workerLock_);
    if (threadCall_) thread_call_cancel_wait(threadCall_);
    OSSafeReleaseNULL(interruptSource_);
    OSSafeReleaseNULL(appleHdaPollTimer_);
    OSSafeReleaseNULL(workLoop_);
    __atomic_store_n(&installed_, false, __ATOMIC_RELEASE);
    return true;
}

void DmicCaptureInterruptBinding::interruptThunk(
    OSObject *owner, IOInterruptEventSource *, int) {
    auto *binding = OSDynamicCast(DmicCaptureInterruptBinding, owner);
    if (binding) binding->queueWorker();
}

void DmicCaptureInterruptBinding::appleHdaInterruptThunk(
    void *target, void *refcon, bool interrupt, uint64_t *) {
    auto *binding = static_cast<DmicCaptureInterruptBinding *>(target);
    if (!binding || !interrupt ||
        !__atomic_load_n(&binding->installed_, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&binding->enabled_, __ATOMIC_ACQUIRE) ||
        !binding->appleHdaLease_ ||
        refcon != binding->appleHdaLease_->stream())
        return;
    if (binding->threadCall_) thread_call_enter(binding->threadCall_);
}

void DmicCaptureInterruptBinding::appleHdaPollTimerThunk(
    OSObject *owner, IOTimerEventSource *source) {
    auto *binding = OSDynamicCast(DmicCaptureInterruptBinding, owner);
    if (!binding || !source) return;
    if (__atomic_load_n(&binding->installed_, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&binding->enabled_, __ATOMIC_ACQUIRE)) {
        if (binding->threadCall_) thread_call_enter(binding->threadCall_);
        source->setTimeoutMS(1);
    }
}

void DmicCaptureInterruptBinding::threadCallThunk(
    thread_call_param_t context, thread_call_param_t) {
    auto *binding = static_cast<DmicCaptureInterruptBinding *>(context);
    if (binding) binding->runWorker();
}

void DmicCaptureInterruptBinding::queueWorker() noexcept {
    if (__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&enabled_, __ATOMIC_ACQUIRE) && threadCall_)
        thread_call_enter(threadCall_);
}

void DmicCaptureInterruptBinding::runWorker() noexcept {
    if (!__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&enabled_, __ATOMIC_ACQUIRE) || !service_ ||
        !actionHandler_ || !workerLock_)
        return;
    IOLockLock(workerLock_);
    if (appleHdaLease_) {
        uint32_t position = UINT32_MAX;
        const bool positionRead = appleHdaLease_->linkPositionBytes(&position);
        if (__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&enabled_, __ATOMIC_ACQUIRE)) {
            sof::ipc4::kernel::HdaCaptureTriggerAction action =
                sof::ipc4::kernel::HdaCaptureTriggerAction::None;
            const CaptureInterruptServiceResult result =
                service_->serviceAppleHdaPosition(
                    positionRead ? position : UINT32_MAX, &action);
            actionHandler_(actionContext_, result, action);
        }
        IOLockUnlock(workerLock_);
        return;
    }
    constexpr uint32_t kMaximumPeriodsPerCall = 16;
    uint32_t processed = 0;
    while (__atomic_load_n(&installed_, __ATOMIC_ACQUIRE) &&
           processed < kMaximumPeriodsPerCall) {
        sof::ipc4::kernel::HdaCaptureTriggerAction action =
            sof::ipc4::kernel::HdaCaptureTriggerAction::None;
        const CaptureInterruptServiceResult result =
            service_->serviceOne(&action);
        actionHandler_(actionContext_, result, action);
        if (result != CaptureInterruptServiceResult::PeriodPublished) break;
        ++processed;
    }
    const bool reschedule = __atomic_load_n(&installed_, __ATOMIC_ACQUIRE) &&
        processed == kMaximumPeriodsPerCall;
    if (reschedule && threadCall_) thread_call_enter(threadCall_);
    IOLockUnlock(workerLock_);
}

void DmicCaptureInterruptBinding::free() {
    if (!uninstall()) return;
    if (threadCall_) {
        while (!thread_call_free(threadCall_))
            thread_call_cancel_wait(threadCall_);
        threadCall_ = nullptr;
    }
    if (workerLock_) {
        IOLockFree(workerLock_);
        workerLock_ = nullptr;
    }
    OSObject::free();
}

