CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
KERNEL_SDK ?= /Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk
KERNEL_TARGET ?= x86_64-apple-macos26.0
KERNEL_FRAMEWORK = $(KERNEL_SDK)/System/Library/Frameworks/Kernel.framework
AUDIT_DIR ?= audit
UPSTREAM_DIR ?= ../upstream-ipc4-reference
DMIC_CANDIDATE ?= $(AUDIT_DIR)/sof-hda-generic-2ch-ipc4-v2.12-dmic-candidate.tplg
DMIC_SOURCE ?= $(UPSTREAM_DIR)/sof-hda-generic-2ch-ipc4-v2.12.tplg
DMIC_FIRMWARE ?= $(UPSTREAM_DIR)/sof-adl-ipc4-intel-signed-v2.12.ri
DMIC_FIRMWARE_SHA256 ?= 351325786e53ca7cd23ffc1462d82913f3e0c08ad78f89053d85e4b6703cf12b
DMIC_NHLT ?= ../NHLT-HP-8A13-20261031.aml
SOF_FIRMWARE_HEADER = generated/SofFirmwareKernelData.hpp

all: build/ipc4-manifest-inspect build/ipc4-header-decode build/ipc4-dmic-pair-check build/ipc4-message-demo build/ipc4-dmic-plan-demo build/ipc4-kernel-codec.o build/Ipc4KernelCodec-macos-kernel.o build/Ipc4KernelTransaction-macos-kernel.o build/Ipc4Header-macos-kernel.o build/KernelIdPool-macos-kernel.o build/KernelRuntimeIdAllocator-macos-kernel.o build/FirmwareManifestKernel-macos-kernel.o build/FirmwareImageHashKernel-macos-kernel.o build/FirmwareKernelData-macos-kernel.o build/FirmwareConfigKernel-macos-kernel.o build/Cavs25KernelTransportPlan-macos-kernel.o build/Cavs25RomPurgePlan-macos-kernel.o build/Cavs25RomCoreStartPlan-macos-kernel.o build/Cavs25RomStatusPlan-macos-kernel.o build/Cavs25DmaPlan-macos-kernel.o build/Cavs25CodeLoaderStreamPlan-macos-kernel.o build/Cavs25CodeLoaderSetupPlan-macos-kernel.o build/HdaInputStreamAllocatorPlan-macos-kernel.o build/HdaDmicCapturePlan-macos-kernel.o build/HdaCaptureBdlPlan-macos-kernel.o build/HdaCapturePositionPlan-macos-kernel.o build/HdaCaptureResetPlan-macos-kernel.o build/HdaCaptureRegisterPlan-macos-kernel.o build/HdaCaptureTriggerPlan-macos-kernel.o build/Cavs25BootLifecycle-macos-kernel.o build/Cavs25RomCommandExecutor-macos-kernel.o build/Cavs25CodeLoaderMmioExecutor-macos-kernel.o build/Cavs25MappedDma-macos-kernel.o build/HdaCaptureMappedDma-macos-kernel.o build/Cavs25RomStatusReader-macos-kernel.o build/HdaControllerStatusReader-macos-kernel.o build/HdaCaptureMmioExecutor-macos-kernel.o build/DmicKernelCaptureRuntime-macos-kernel.o build/DmicSharedCaptureRing-macos-kernel.o build/DmicCapturePeriodPublisher-macos-kernel.o build/DmicCapturePeriodPump-macos-kernel.o build/DmicCaptureInterruptService-macos-kernel.o build/DmicCaptureInterruptBinding-macos-kernel.o build/Ipc4MmioAdapter-macos-kernel.o build/Ipc4KernelTransport-macos-kernel.o build/DmicKernelStaticData-macos-kernel.o build/DmicKernelOperationPlan-macos-kernel.o build/DmicKernelOperationRunner-macos-kernel.o build/DmicKernelSession-macos-kernel.o build/AppleHDAStreamLease-macos-kernel.o build/HPIPC4Controller-macos-kernel.o build/HPIPC4KernelCore-macos.o build/HPIPC4DriverCompileOnly-macos.o

.PHONY: kernel-bundle
kernel-bundle: build/HPIPC4CompileOnly.kext/Contents/MacOS/HPIPC4CompileOnly

build/HPIPC4CompileOnly.kext/Contents/MacOS/HPIPC4CompileOnly: build/HPIPC4DriverCompileOnly-macos.o HPIPC4CompileOnly-Info.plist
	mkdir -p build/HPIPC4CompileOnly.kext/Contents/MacOS
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-Xlinker -kext -nostdlib -lcc_kext -o $@ build/HPIPC4DriverCompileOnly-macos.o
	cp HPIPC4CompileOnly-Info.plist build/HPIPC4CompileOnly.kext/Contents/Info.plist

# Matchable integration candidate, staged only under build/ and never loaded
# by this Makefile. Keep the no-personality compile-only bundle for safe audits.
.PHONY: candidate-bundle
candidate-bundle: build/HPIPC4Candidate.kext/Contents/MacOS/AppleHDADmicBridge


# Build the DMIC helper as a child service of AppleHDA and embed it into the
# SimpleHDA module payload; it must never be added as a PCI-matching EFI kext.
SIMPLEHDA_PROJECT := ../SimpleHDA
.PHONY: appledha-dmic-module
appledha-dmic-module: candidate-bundle
	mkdir -p $(SIMPLEHDA_PROJECT)/Resources/AppleHDADmicBridge.kext/Contents/MacOS
	cp HPIPC4Candidate-Info.plist $(SIMPLEHDA_PROJECT)/Resources/AppleHDADmicBridge.kext/Contents/Info.plist
	cp build/HPIPC4Candidate.kext/Contents/MacOS/AppleHDADmicBridge $(SIMPLEHDA_PROJECT)/Resources/AppleHDADmicBridge.kext/Contents/MacOS/AppleHDADmicBridge
	python3 $(SIMPLEHDA_PROJECT)/Tools/prepare_payload.py


build/HPIPC4Candidate.kext/Contents/MacOS/AppleHDADmicBridge: build/HPIPC4CompileOnly.kext/Contents/MacOS/HPIPC4CompileOnly HPIPC4Candidate-Info.plist
	mkdir -p build/HPIPC4Candidate.kext/Contents/MacOS
	cp build/HPIPC4CompileOnly.kext/Contents/MacOS/HPIPC4CompileOnly $@
	cp HPIPC4Candidate-Info.plist build/HPIPC4Candidate.kext/Contents/Info.plist

# Reproducible static audit for the audited local firmware/topology inputs.
# This target writes only under build/ and never loads a driver or accesses hardware.
.PHONY: hal-factory-smoke
hal-factory-smoke: build/hal-factory-smoke build/HPIPC4Audio.driver/Contents/MacOS/HPIPC4Audio
	./build/hal-factory-smoke build/HPIPC4Audio.driver

build/hal-factory-smoke: AudioServerPlugIn/HALFactorySmoke.c
	mkdir -p build
	clang -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-std=gnu11 -O2 -Wall -Wextra -framework CoreAudio \
		-framework CoreFoundation -o $@ $<

.PHONY: offline-audit
offline-audit: all kernel-bundle candidate-bundle hal-plugin hal-factory-smoke
	python3 verify_firmware_digest.py "$(DMIC_FIRMWARE)" "$(DMIC_FIRMWARE_SHA256)"
	python3 generate_firmware_kernel_header.py "$(DMIC_FIRMWARE)" build/SofFirmwareKernelData.audit.hpp
	cmp $(SOF_FIRMWARE_HEADER) build/SofFirmwareKernelData.audit.hpp
	python3 "$(AUDIT_DIR)/validate_ipc4_dmic_candidate.py" "$(DMIC_CANDIDATE)" "$(DMIC_SOURCE)" "$(DMIC_FIRMWARE)" --parser-dir "$(AUDIT_DIR)"
	./build/ipc4-dmic-pair-check "$(DMIC_FIRMWARE)" "$(DMIC_CANDIDATE)"
	python3 build_dmic_operation_plan.py "$(DMIC_CANDIDATE)" "$(DMIC_SOURCE)" "$(DMIC_FIRMWARE)" build/dmic-operation-plan.json --audit-dir "$(AUDIT_DIR)" --nhlt "$(DMIC_NHLT)"
	python3 generate_dmic_static_header.py build/dmic-operation-plan.json build/DmicStaticData.audit.hpp --kernel-output build/DmicKernelStaticData.audit.hpp
	cmp generated/DmicStaticData.hpp build/DmicStaticData.audit.hpp
	cmp generated/DmicKernelStaticData.hpp build/DmicKernelStaticData.audit.hpp
	./build/ipc4-message-demo
	./build/ipc4-dmic-plan-demo "$(DMIC_FIRMWARE)"
	./build/ipc4-manifest-inspect "$(DMIC_FIRMWARE)" >/dev/null
	plutil -lint build/HPIPC4CompileOnly.kext/Contents/Info.plist HPIPC4Candidate-Info.plist build/HPIPC4Candidate.kext/Contents/Info.plist build/HPIPC4Audio.driver/Contents/Info.plist
	file build/HPIPC4CompileOnly.kext/Contents/MacOS/HPIPC4CompileOnly build/HPIPC4Candidate.kext/Contents/MacOS/HPIPC4 build/HPIPC4Audio.driver/Contents/MacOS/HPIPC4Audio

build/HPIPC4Controller-macos-kernel.o: HPIPC4Controller.cpp HdaControllerStatusReader.hpp HdaInputStreamAllocatorPlan.hpp Cavs25CodeLoaderStreamPlan.hpp Cavs25CodeLoaderSetupPlan.hpp Cavs25CodeLoaderMmioExecutor.hpp Cavs25RomCommandExecutor.hpp Cavs25RomStatusReader.hpp Cavs25RomStatusPlan.hpp Cavs25RomPurgePlan.hpp FirmwareImageHashKernel.hpp FirmwareManifestKernel.hpp FirmwareKernelData.hpp Cavs25MappedDma.hpp Cavs25DmaPlan.hpp Cavs25BootLifecycle.hpp DmicCaptureDmaConfig.hpp DmicCapturePeriodPublisher.hpp HdaCaptureMappedDma.hpp HdaCapturePositionPlan.hpp HdaCaptureRegisterPlan.hpp HdaCaptureResetPlan.hpp HdaDmicCapturePlan.hpp DmicSharedCaptureRing.hpp DmicKernelSession.hpp DmicKernelOperationRunner.hpp KernelRuntimeIdAllocator.hpp DmicKernelCaptureRuntime.hpp DmicCapturePeriodPump.hpp DmicCaptureInterruptService.hpp DmicCaptureInterruptBinding.hpp Ipc4MmioAdapter.hpp Ipc4KernelTransport.hpp AppleHDAStreamLease.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/AppleHDAStreamLease-macos-kernel.o: AppleHDAStreamLease.cpp AppleHDAStreamLease.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/ipc4-manifest-inspect: FirmwareManifest.cpp FirmwareManifest.hpp ManifestInspect.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) FirmwareManifest.cpp ManifestInspect.cpp -o $@

build/ipc4-header-decode: Ipc4Header.cpp Ipc4Header.hpp Ipc4HeaderCheck.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) Ipc4Header.cpp Ipc4HeaderCheck.cpp -o $@

build/ipc4-dmic-pair-check: FirmwareManifest.cpp FirmwareManifest.hpp DmicPairCheck.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) FirmwareManifest.cpp DmicPairCheck.cpp -o $@

build/ipc4-message-demo: Ipc4Header.cpp Ipc4Header.hpp Ipc4Messages.cpp Ipc4Messages.hpp Ipc4Transaction.cpp Ipc4Transaction.hpp Cavs25TransportPlan.cpp Cavs25TransportPlan.hpp Ipc4MessageDemo.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) Ipc4Header.cpp Ipc4Messages.cpp Ipc4Transaction.cpp Cavs25TransportPlan.cpp Ipc4MessageDemo.cpp -o $@

generated/DmicStaticData.hpp: generated/dmic-operation-plan.json generate_dmic_static_header.py
	python3 generate_dmic_static_header.py $< generated/DmicStaticData.hpp --kernel-output generated/DmicKernelStaticData.hpp

generated/DmicKernelStaticData.hpp: generated/DmicStaticData.hpp
	test -f $@ || python3 generate_dmic_static_header.py generated/dmic-operation-plan.json generated/DmicStaticData.hpp --kernel-output $@

$(SOF_FIRMWARE_HEADER): $(DMIC_FIRMWARE) generate_firmware_kernel_header.py
	python3 generate_firmware_kernel_header.py "$<" "$@"

build/FirmwareKernelData-macos-kernel.o: FirmwareKernelData.cpp FirmwareKernelData.hpp $(SOF_FIRMWARE_HEADER)
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicKernelStaticData-macos-kernel.o: DmicKernelStaticData.cpp generated/DmicKernelStaticData.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicFrontendReviewPlan-macos-kernel.o: DmicFrontendReviewPlan.cpp DmicFrontendReviewPlan.hpp generated/DmicFrontendReviewData.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicKernelOperationPlan-macos-kernel.o: DmicKernelOperationPlan.cpp DmicKernelOperationPlan.hpp HdaDmicCapturePlan.hpp FirmwareManifestKernel.hpp Ipc4KernelCodec.hpp generated/DmicKernelStaticData.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicKernelOperationRunner-macos-kernel.o: DmicKernelOperationRunner.cpp DmicKernelOperationRunner.hpp DmicKernelOperationPlan.hpp HdaDmicCapturePlan.hpp Ipc4KernelTransport.hpp Cavs25BootLifecycle.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicKernelSession-macos-kernel.o: DmicKernelSession.cpp DmicKernelSession.hpp DmicKernelOperationRunner.hpp KernelRuntimeIdAllocator.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/ipc4-dmic-plan-demo: FirmwareManifest.cpp FirmwareManifest.hpp FirmwareConfig.cpp FirmwareConfig.hpp Ipc4Messages.cpp Ipc4Messages.hpp DmicMessagePlan.cpp DmicMessagePlan.hpp RuntimeIdAllocator.cpp RuntimeIdAllocator.hpp DmicMessagePlanDemo.cpp generated/DmicStaticData.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) FirmwareManifest.cpp FirmwareConfig.cpp Ipc4Messages.cpp DmicMessagePlan.cpp RuntimeIdAllocator.cpp DmicMessagePlanDemo.cpp -o $@

build/ipc4-kernel-codec.o: Ipc4KernelCodec.cpp Ipc4KernelCodec.hpp
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -fno-exceptions -fno-rtti -c Ipc4KernelCodec.cpp -o $@

build/Ipc4KernelCodec-macos-kernel.o: Ipc4KernelCodec.cpp Ipc4KernelCodec.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Ipc4KernelTransaction-macos-kernel.o: Ipc4KernelTransaction.cpp Ipc4KernelTransaction.hpp Ipc4Header.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Ipc4Header-macos-kernel.o: Ipc4Header.cpp Ipc4Header.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/KernelIdPool-macos-kernel.o: KernelIdPool.cpp KernelIdPool.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/KernelRuntimeIdAllocator-macos-kernel.o: KernelRuntimeIdAllocator.cpp KernelRuntimeIdAllocator.hpp DmicKernelOperationPlan.hpp KernelIdPool.hpp FirmwareManifestKernel.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/FirmwareConfigKernel-macos-kernel.o: FirmwareConfigKernel.cpp FirmwareConfigKernel.hpp Ipc4KernelCodec.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/FirmwareManifestKernel-macos-kernel.o: FirmwareManifestKernel.cpp FirmwareManifestKernel.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/FirmwareImageHashKernel-macos-kernel.o: FirmwareImageHashKernel.cpp FirmwareImageHashKernel.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25KernelTransportPlan-macos-kernel.o: Cavs25KernelTransportPlan.cpp Cavs25KernelTransportPlan.hpp Ipc4KernelCodec.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25RomCommandExecutor-macos-kernel.o: Cavs25RomCommandExecutor.cpp Cavs25RomCommandExecutor.hpp Cavs25BootLifecycle.hpp Cavs25RomCoreStartPlan.hpp Cavs25RomPurgePlan.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25CodeLoaderMmioExecutor-macos-kernel.o: Cavs25CodeLoaderMmioExecutor.cpp Cavs25CodeLoaderMmioExecutor.hpp Cavs25CodeLoaderSetupPlan.hpp Cavs25BootLifecycle.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25DmaPlan-macos-kernel.o: Cavs25DmaPlan.cpp Cavs25DmaPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25RomStatusPlan-macos-kernel.o: Cavs25RomStatusPlan.cpp Cavs25RomStatusPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25RomPurgePlan-macos-kernel.o: Cavs25RomPurgePlan.cpp Cavs25RomPurgePlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25RomCoreStartPlan-macos-kernel.o: Cavs25RomCoreStartPlan.cpp Cavs25RomCoreStartPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaDmicCapturePlan-macos-kernel.o: HdaDmicCapturePlan.cpp HdaDmicCapturePlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaInputStreamAllocatorPlan-macos-kernel.o: HdaInputStreamAllocatorPlan.cpp HdaInputStreamAllocatorPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25CodeLoaderStreamPlan-macos-kernel.o: Cavs25CodeLoaderStreamPlan.cpp Cavs25CodeLoaderStreamPlan.hpp HdaInputStreamAllocatorPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25CodeLoaderSetupPlan-macos-kernel.o: Cavs25CodeLoaderSetupPlan.cpp Cavs25CodeLoaderSetupPlan.hpp Cavs25CodeLoaderStreamPlan.hpp Cavs25DmaPlan.hpp HdaCaptureRegisterPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCaptureTriggerPlan-macos-kernel.o: HdaCaptureTriggerPlan.cpp HdaCaptureTriggerPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCaptureBdlPlan-macos-kernel.o: HdaCaptureBdlPlan.cpp HdaCaptureBdlPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCapturePositionPlan-macos-kernel.o: HdaCapturePositionPlan.cpp HdaCapturePositionPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCaptureResetPlan-macos-kernel.o: HdaCaptureResetPlan.cpp HdaCaptureResetPlan.hpp HdaInputStreamAllocatorPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCaptureRegisterPlan-macos-kernel.o: HdaCaptureRegisterPlan.cpp HdaCaptureRegisterPlan.hpp HdaCaptureBdlPlan.hpp HdaInputStreamAllocatorPlan.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25MappedDma-macos-kernel.o: Cavs25MappedDma.cpp Cavs25MappedDma.hpp Cavs25DmaPlan.hpp Cavs25BootLifecycle.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCaptureMappedDma-macos-kernel.o: HdaCaptureMappedDma.cpp HdaCaptureMappedDma.hpp HdaCaptureBdlPlan.hpp HdaCapturePositionPlan.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicKernelCaptureRuntime-macos-kernel.o: DmicKernelCaptureRuntime.cpp DmicKernelCaptureRuntime.hpp DmicKernelSession.hpp HdaCaptureMappedDma.hpp HdaCapturePositionPlan.hpp HdaCaptureTriggerPlan.hpp HdaInputStreamAllocatorPlan.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicSharedCaptureRing-macos-kernel.o: DmicSharedCaptureRing.cpp DmicSharedCaptureRing.hpp DmicAudioBridgeABI.h
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicCapturePeriodPublisher-macos-kernel.o: DmicCapturePeriodPublisher.cpp DmicCapturePeriodPublisher.hpp DmicKernelCaptureRuntime.hpp DmicSharedCaptureRing.hpp DmicAudioBridgeABI.h
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicCapturePeriodPump-macos-kernel.o: DmicCapturePeriodPump.cpp DmicCapturePeriodPump.hpp DmicCapturePeriodPublisher.hpp DmicKernelCaptureRuntime.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicCaptureInterruptService-macos-kernel.o: DmicCaptureInterruptService.cpp DmicCaptureInterruptService.hpp DmicCapturePeriodPump.hpp HdaCaptureMmioExecutor.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/DmicCaptureInterruptBinding-macos-kernel.o: DmicCaptureInterruptBinding.cpp DmicCaptureInterruptBinding.hpp DmicCaptureInterruptService.hpp AppleHDAStreamLease.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaCaptureMmioExecutor-macos-kernel.o: HdaCaptureMmioExecutor.cpp HdaCaptureMmioExecutor.hpp HdaCaptureRegisterPlan.hpp HdaCaptureResetPlan.hpp HdaCapturePositionPlan.hpp HdaCaptureTriggerPlan.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25RomStatusReader-macos-kernel.o: Cavs25RomStatusReader.cpp Cavs25RomStatusReader.hpp Cavs25RomStatusPlan.hpp Cavs25RomPurgePlan.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HdaControllerStatusReader-macos-kernel.o: HdaControllerStatusReader.cpp HdaControllerStatusReader.hpp HdaInputStreamAllocatorPlan.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Ipc4MmioAdapter-macos-kernel.o: Ipc4MmioAdapter.cpp Ipc4MmioAdapter.hpp Ipc4MmioPolicy.hpp Ipc4KernelTransport.hpp Cavs25KernelTransportPlan.hpp Cavs25BootLifecycle.hpp
	mkdir -p build
	clang++ -Wno-deprecated-declarations -Wno-inconsistent-missing-override -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Cavs25BootLifecycle-macos-kernel.o: Cavs25BootLifecycle.cpp Cavs25BootLifecycle.hpp Cavs25CodeLoaderSetupPlan.hpp Cavs25DmaPlan.hpp Cavs25RomCoreStartPlan.hpp Cavs25RomStatusPlan.hpp FirmwareImageHashKernel.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/Ipc4KernelTransport-macos-kernel.o: Ipc4KernelTransport.cpp Ipc4KernelTransport.hpp Cavs25KernelTransportPlan.hpp FirmwareConfigKernel.hpp Ipc4KernelTransaction.hpp
	mkdir -p build
	clang++ -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) \
		-I$(KERNEL_FRAMEWORK)/Headers -I$(KERNEL_FRAMEWORK)/Headers/IOKit \
		-mkernel -nostdlib -std=c++17 -fno-exceptions -fno-rtti -fno-builtin \
		-fno-common -fno-stack-protector -DKERNEL -DKERNEL_PRIVATE \
		-DDRIVER_PRIVATE -DAPPLE -DNeXT -D__MACOSX__ \
		-Wall -Wextra -Wconversion -Wsign-conversion -c $< -o $@

build/HPIPC4KernelCore-macos.o: build/Ipc4KernelCodec-macos-kernel.o build/Ipc4Header-macos-kernel.o build/Ipc4KernelTransaction-macos-kernel.o build/KernelIdPool-macos-kernel.o build/KernelRuntimeIdAllocator-macos-kernel.o build/FirmwareManifestKernel-macos-kernel.o build/FirmwareImageHashKernel-macos-kernel.o build/FirmwareKernelData-macos-kernel.o build/FirmwareConfigKernel-macos-kernel.o build/Cavs25KernelTransportPlan-macos-kernel.o build/Cavs25RomPurgePlan-macos-kernel.o build/Cavs25RomCoreStartPlan-macos-kernel.o build/Cavs25RomStatusPlan-macos-kernel.o build/Cavs25DmaPlan-macos-kernel.o build/Cavs25CodeLoaderStreamPlan-macos-kernel.o build/Cavs25CodeLoaderSetupPlan-macos-kernel.o build/HdaInputStreamAllocatorPlan-macos-kernel.o build/HdaDmicCapturePlan-macos-kernel.o build/HdaCaptureBdlPlan-macos-kernel.o build/HdaCapturePositionPlan-macos-kernel.o build/HdaCaptureResetPlan-macos-kernel.o build/HdaCaptureRegisterPlan-macos-kernel.o build/HdaCaptureTriggerPlan-macos-kernel.o build/Cavs25BootLifecycle-macos-kernel.o build/Ipc4KernelTransport-macos-kernel.o build/DmicKernelStaticData-macos-kernel.o build/DmicKernelOperationPlan-macos-kernel.o build/DmicKernelOperationRunner-macos-kernel.o build/DmicKernelSession-macos-kernel.o build/DmicFrontendReviewPlan-macos-kernel.o
	ld -r -arch x86_64 -o $@ $^

build/HPIPC4DriverCompileOnly-macos.o: build/HPIPC4Controller-macos-kernel.o build/AppleHDAStreamLease-macos-kernel.o build/Cavs25RomCommandExecutor-macos-kernel.o build/Cavs25CodeLoaderMmioExecutor-macos-kernel.o build/Cavs25MappedDma-macos-kernel.o build/HdaCaptureMappedDma-macos-kernel.o build/Cavs25RomStatusReader-macos-kernel.o build/HdaControllerStatusReader-macos-kernel.o build/HdaCaptureMmioExecutor-macos-kernel.o build/DmicKernelCaptureRuntime-macos-kernel.o build/DmicSharedCaptureRing-macos-kernel.o build/DmicCapturePeriodPublisher-macos-kernel.o build/DmicCapturePeriodPump-macos-kernel.o build/DmicCaptureInterruptService-macos-kernel.o build/DmicCaptureInterruptBinding-macos-kernel.o build/Ipc4MmioAdapter-macos-kernel.o build/HPIPC4KernelCore-macos.o
	ld -r -arch x86_64 -o $@ $^

clean:
	rm -rf build

.PHONY: all clean

.PHONY: hal-plugin
hal-plugin: build/HPIPC4Audio.driver/Contents/MacOS/HPIPC4Audio

build/HPIPC4Audio.driver/Contents/MacOS/HPIPC4Audio: AudioServerPlugIn/HPIPC4AudioPlugIn.c AudioServerPlugIn/Info.plist DmicAudioBridgeABI.h
	mkdir -p build/HPIPC4Audio.driver/Contents/MacOS
	clang -arch x86_64 -target $(KERNEL_TARGET) -isysroot $(KERNEL_SDK) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -bundle -framework CoreAudio -framework IOKit -framework CoreFoundation -o $@ AudioServerPlugIn/HPIPC4AudioPlugIn.c
	cp AudioServerPlugIn/Info.plist build/HPIPC4Audio.driver/Contents/Info.plist
