#include "DmicMessagePlan.hpp"
#include "FirmwareConfig.hpp"
#include "RuntimeIdAllocator.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: ipc4-dmic-plan-demo firmware.ri\n";
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open firmware image");
        const std::vector<uint8_t> image{std::istreambuf_iterator<char>(input), {}};
        const auto firmware = sof::ipc4::FirmwareManifest::parse(image.data(), image.size());
        const auto fwConfigRequest = sof::ipc4::makeFirmwareConfigRequest(4096);
        const std::vector<uint8_t> fwConfigTuple{
            9, 0, 0, 0, 4, 0, 0, 0, 32, 0, 0, 0};
        const auto fwPipelineCeiling = sof::ipc4::parseFirmwareConfigReply(
            0x63000000U, fwConfigTuple.data(), fwConfigTuple.size());
        if (fwConfigRequest.message.primary != 0x43000000U ||
            fwConfigRequest.message.extension != 0x20701000U ||
            fwConfigRequest.replyCapacityBytes != 4096 || fwPipelineCeiling != 32)
            throw std::runtime_error("FW_CONFIG request header mismatch");
        // Placeholder ceiling for offline serialization only. A real driver
        // must use the inclusive ceiling returned by its FW_CONFIG query.
        sof::ipc4::dmic::RuntimeIdAllocator allocator(32);
        const auto ids = allocator.allocate(firmware);
        const auto plan = sof::ipc4::dmic::buildMessagePlan(firmware, ids);
        if (plan.createPipeline.payload.size() != 28 || plan.initModules.size() != 4 ||
            plan.bindRoutes.size() != 3 || plan.configureControls.size() != 2 ||
            plan.unbindRoutes.size() != 3 ||
            plan.deletePipeline.primary != sof::ipc4::deletePipeline(ids.pipeline).primary)
            throw std::runtime_error("DMIC message plan shape mismatch");
        if (plan.initModules[0].payload.size() != 3128 ||
            plan.initModules[1].payload.size() != 64 ||
            plan.initModules[2].payload.size() != 40 ||
            plan.initModules[3].payload.size() != 84 ||
            plan.configureControls[0].payload.size() != 108 ||
            plan.configureControls[1].payload.size() != 52)
            throw std::runtime_error("DMIC serialized payload size mismatch");
        std::cout << "firmware=" << firmware.imageName << " version="
                  << firmware.version[0] << '.' << firmware.version[1] << '.'
                  << firmware.version[2] << '.' << firmware.version[3] << '\n';
        std::cout << "pipeline payload=" << plan.createPipeline.payload.size()
                  << " module_init=" << plan.initModules.size()
                  << " binds=" << plan.bindRoutes.size()
                  << " controls=" << plan.configureControls.size()
                  << " unbinds=" << plan.unbindRoutes.size() << '\n';
        std::cout << "allocated illustrative IDs: pipeline=" << unsigned(ids.pipeline)
                  << " copier=" << unsigned(ids.daiCopier) << ','
                  << unsigned(ids.moduleCopier) << " gain=" << unsigned(ids.gain)
                  << " eqiir=" << unsigned(ids.eqiir) << '\n';
        std::cout << "FW_CONFIG GET template=0x" << std::hex
                  << fwConfigRequest.message.primary << "|0x"
                  << fwConfigRequest.message.extension << std::dec
                  << " reply_capacity=" << fwConfigRequest.replyCapacityBytes << '\n';
        std::cout << "runtime-ID message plan verified; no hardware transport present\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
