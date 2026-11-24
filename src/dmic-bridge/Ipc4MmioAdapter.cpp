#include "Ipc4MmioAdapter.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kMaximumDelayMicroseconds = 10000;

bool validMapping(IOMemoryMap *map, uint32_t offset,
                  size_t length) noexcept {
    if (!map || map->getVirtualAddress() == 0) return false;
    const uint64_t mapLength = map->getLength();
    return static_cast<uint64_t>(offset) <= mapLength &&
           static_cast<uint64_t>(length) <= mapLength - offset;
}

} // namespace

Ipc4MmioAdapter::Ipc4MmioAdapter() noexcept
    : pciDevice_(nullptr), owner_(nullptr), dspMap_(nullptr),
      lifecycle_(nullptr), mappingCreatedWritable_(false),
      sharedController_(nullptr), incomingHandler_(nullptr), incomingContext_(nullptr) {}

Ipc4MmioAdapter::Ipc4MmioAdapter(
    IOPCIDevice *pciDevice, IOService *owner, IOMemoryMap *dspMap,
    const Cavs25BootLifecycle *lifecycle,
    bool mappingCreatedWritable, IOService *sharedController,
    IncomingHandler incomingHandler, void *incomingContext) noexcept
    : pciDevice_(pciDevice), owner_(owner), dspMap_(dspMap),
      lifecycle_(lifecycle),
      mappingCreatedWritable_(mappingCreatedWritable),
      sharedController_(sharedController),
      incomingHandler_(incomingHandler), incomingContext_(incomingContext) {}

bool Ipc4MmioAdapter::configure(
    IOPCIDevice *pciDevice, IOService *owner, IOMemoryMap *dspMap,
    const Cavs25BootLifecycle *lifecycle, bool mappingCreatedWritable,
    IOService *sharedController, IncomingHandler incomingHandler, void *incomingContext) noexcept {
    if (!pciDevice || !owner || !dspMap || !lifecycle || !incomingHandler ||
        dspMap->getVirtualAddress() == 0 ||
        (mappingCreatedWritable &&
         (dspMap->getMapOptions() & kIOMapReadOnly) != 0))
        return false;
    pciDevice_ = pciDevice;
    owner_ = owner;
    dspMap_ = dspMap;
    lifecycle_ = lifecycle;
    mappingCreatedWritable_ = mappingCreatedWritable;
    sharedController_ = sharedController;
    incomingHandler_ = incomingHandler;
    incomingContext_ = incomingContext;
    return true;
}

void Ipc4MmioAdapter::reset() noexcept {
    pciDevice_ = nullptr;
    owner_ = nullptr;
    dspMap_ = nullptr;
    lifecycle_ = nullptr;
    mappingCreatedWritable_ = false;
    sharedController_ = nullptr;
    incomingHandler_ = nullptr;
    incomingContext_ = nullptr;
}

bool Ipc4MmioAdapter::deviceOpen() const noexcept {
    if (!pciDevice_ || !owner_ || pciDevice_->isInactive() || owner_->isInactive())
        return false;
    return pciDevice_->isOpen(owner_) ||
        (sharedController_ && !sharedController_->isInactive() &&
         owner_->getProvider() == sharedController_ &&
         sharedController_->getProvider() == pciDevice_);
}

IoOps Ipc4MmioAdapter::ioOps() noexcept {
    return IoOps{this, &Ipc4MmioAdapter::readRegister,
                 &Ipc4MmioAdapter::writeRegister,
                 &Ipc4MmioAdapter::writeMailbox,
                 &Ipc4MmioAdapter::readMailbox,
                 &Ipc4MmioAdapter::delayMicroseconds,
                 &Ipc4MmioAdapter::incomingMessage};
}

bool Ipc4MmioAdapter::supportedRegister(uint16_t offset) noexcept {
    return mmio_policy::supportedRegister(offset);
}

bool Ipc4MmioAdapter::allowedRegisterWrite(uint16_t offset, uint32_t value,
                                           bool firmwareReady) noexcept {
    return mmio_policy::allowedRegisterWrite(offset, value, firmwareReady);
}

bool Ipc4MmioAdapter::validMailboxRange(uint64_t mapLength, uint32_t offset,
                                        size_t length,
                                        bool writeToDevice) noexcept {
    return mmio_policy::validMailboxRange(mapLength, offset, length,
                                          writeToDevice);
}

bool Ipc4MmioAdapter::readRegister(void *context, uint16_t offset,
                                   uint32_t *value) noexcept {
    auto *self = static_cast<Ipc4MmioAdapter *>(context);
    if (!self || !self->deviceOpen() || !value || !supportedRegister(offset) ||
        (offset & 3U) != 0 || !validMapping(self->dspMap_, offset, sizeof(uint32_t)))
        return false;
    OSSynchronizeIO();
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(
        self->dspMap_->getVirtualAddress());
    *value = base[offset / sizeof(uint32_t)];
    OSSynchronizeIO();
    return true;
}

bool Ipc4MmioAdapter::writeRegister(void *context, uint16_t offset,
                                    uint32_t value) noexcept {
    auto *self = static_cast<Ipc4MmioAdapter *>(context);
    if (!self || !self->deviceOpen() || !self->mappingCreatedWritable_ ||
        !self->lifecycle_ ||
        !allowedRegisterWrite(offset, value,
                              self->lifecycle_->canIssueIpc4()) ||
        (offset & 3U) != 0 || !validMapping(self->dspMap_, offset, sizeof(uint32_t)))
        return false;
    volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(
        self->dspMap_->getVirtualAddress());
    OSSynchronizeIO();
    base[offset / sizeof(uint32_t)] = value;
    OSSynchronizeIO();
    return true;
}

bool Ipc4MmioAdapter::writeMailbox(void *context, uint32_t offset,
                                   const uint8_t *bytes,
                                   size_t length) noexcept {
    auto *self = static_cast<Ipc4MmioAdapter *>(context);
    if (!self || !self->deviceOpen() || !bytes ||
        !self->mappingCreatedWritable_ || !self->lifecycle_ ||
        !self->lifecycle_->canIssueIpc4() || !self->dspMap_ ||
        !validMailboxRange(self->dspMap_->getLength(), offset, length, true) ||
        !validMapping(self->dspMap_, offset, length))
        return false;
    volatile uint8_t *destination = reinterpret_cast<volatile uint8_t *>(
        self->dspMap_->getVirtualAddress()) + offset;
    OSSynchronizeIO();
    for (size_t i = 0; i < length; ++i) destination[i] = bytes[i];
    OSSynchronizeIO();
    return true;
}

bool Ipc4MmioAdapter::readMailbox(void *context, uint32_t offset,
                                  uint8_t *bytes,
                                  size_t length) noexcept {
    auto *self = static_cast<Ipc4MmioAdapter *>(context);
    if (!self || !self->deviceOpen() || !bytes || !self->lifecycle_ ||
        !self->lifecycle_->canIssueIpc4() || !self->dspMap_ ||
        !validMailboxRange(self->dspMap_->getLength(), offset, length, false) ||
        !validMapping(self->dspMap_, offset, length))
        return false;
    const volatile uint8_t *source = reinterpret_cast<const volatile uint8_t *>(
        self->dspMap_->getVirtualAddress()) + offset;
    OSSynchronizeIO();
    for (size_t i = 0; i < length; ++i) bytes[i] = source[i];
    OSSynchronizeIO();
    return true;
}

void Ipc4MmioAdapter::delayMicroseconds(void *, uint32_t delay) noexcept {
    if (delay <= kMaximumDelayMicroseconds) IODelay(delay);
}

bool Ipc4MmioAdapter::incomingMessage(void *context,
                                      const HeaderFields *header) noexcept {
    auto *self = static_cast<Ipc4MmioAdapter *>(context);
    return self && self->incomingHandler_ && header &&
           self->incomingHandler_(self->incomingContext_, header);
}

} // namespace sof::ipc4::kernel
