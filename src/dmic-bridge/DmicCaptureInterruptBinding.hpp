#pragma once

#include "DmicCaptureInterruptService.hpp"
#include "AppleHDAStreamLease.hpp"

#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>

namespace sof::ipc4::dmic::kernel {

using CaptureInterruptActionHandler = void (*)(
    void *context, CaptureInterruptServiceResult result,
    sof::ipc4::kernel::HdaCaptureTriggerAction action);

} // namespace sof::ipc4::dmic::kernel

// Bridges the gated PCI interrupt callback to a serialized, non-gated
// thread-call worker. install() registers the interrupt source disabled. A
// controller establishes the position baseline after SDn.RUN, starts the
// IPC4 pipeline, then enables this source. The action
// callback runs under the worker's serialization lock and must not call
// uninstall() synchronously.
class DmicCaptureInterruptBinding final : public OSObject {
    OSDeclareDefaultStructors(DmicCaptureInterruptBinding)

public:
    bool init() override;
    void free() override;

    bool configure(sof::ipc4::dmic::kernel::DmicCaptureInterruptService *service,
                   sof::ipc4::dmic::kernel::CaptureInterruptActionHandler actionHandler,
                   void *actionContext) noexcept;
    IOReturn install(IOService *interruptProvider, IOWorkLoop *workLoop,
                     int interruptIndex) noexcept;
    IOReturn installAppleHda(AppleHDAStreamLease *lease,
                             IOWorkLoop *workLoop) noexcept;
    IOReturn enable() noexcept;
    // Controller lifecycle transactions use this same lock as the worker.
    // Never acquire it from actionContext callbacks, which already run locked.
    bool lockForControl() noexcept;
    void unlockAfterControl() noexcept;
    // Disables future interrupt delivery without waiting for an in-flight
    // worker; use the control lock to serialize with that worker.
    void disable() noexcept;
    bool uninstall() noexcept;

private:
    static void interruptThunk(OSObject *owner,
                               IOInterruptEventSource *source,
                               int count);
    static void threadCallThunk(thread_call_param_t context,
                                thread_call_param_t unused);
    static void appleHdaInterruptThunk(
        void *target, void *refcon, bool interrupt, uint64_t *timestamp);
    static void appleHdaPollTimerThunk(OSObject *owner,
                                       IOTimerEventSource *source);
    void queueWorker() noexcept;
    void runWorker() noexcept;

    sof::ipc4::dmic::kernel::DmicCaptureInterruptService *service_;
    sof::ipc4::dmic::kernel::CaptureInterruptActionHandler actionHandler_;
    void *actionContext_;
    IOInterruptEventSource *interruptSource_;
    IOTimerEventSource *appleHdaPollTimer_;
    AppleHDAStreamLease *appleHdaLease_;
    IOWorkLoop *workLoop_;
    thread_call_t threadCall_;
    IOLock *workerLock_;
    bool configured_;
    bool installed_;
    bool enabled_;
};

namespace sof::ipc4::dmic::kernel {
using DmicCaptureInterruptBinding = ::DmicCaptureInterruptBinding;
} // namespace sof::ipc4::dmic::kernel
