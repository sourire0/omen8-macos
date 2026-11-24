import Darwin
import Foundation
import IOKit

struct Metrics {
    private var previousCPU: [UInt32]?
    var cpu: Int?
    var memoryUsed: UInt64 = 0
    var memoryTotal = ProcessInfo.processInfo.physicalMemory
    var gpu: Int?
    var gpuTemperature: Int?
    var cpuTemperature: Int?

    mutating func sample() {
        cpuTemperature = packageTemperature() ?? SMCTemperature.cpu()
        let host = mach_host_self()
        defer { mach_port_deallocate(mach_task_self_, host) }

        var load = host_cpu_load_info()
        var count = mach_msg_type_number_t(MemoryLayout<host_cpu_load_info>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &load) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics(host, HOST_CPU_LOAD_INFO, $0, &count)
            }
        }
        if result == KERN_SUCCESS {
            let ticks = [load.cpu_ticks.0, load.cpu_ticks.1, load.cpu_ticks.2, load.cpu_ticks.3]
            if let previousCPU {
                let delta = zip(ticks, previousCPU).map { UInt64($0 &- $1) }
                let total = delta.reduce(0, +)
                if total > 0 { cpu = Int((total - delta[Int(CPU_STATE_IDLE)]) * 100 / total) }
            }
            previousCPU = ticks
        }

        var vm = vm_statistics64()
        count = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.size / MemoryLayout<integer_t>.size)
        let vmResult = withUnsafeMutablePointer(to: &vm) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics64(host, HOST_VM_INFO64, $0, &count)
            }
        }
        if vmResult == KERN_SUCCESS {
            let app = UInt64(vm.internal_page_count) - min(UInt64(vm.internal_page_count), UInt64(vm.purgeable_count))
            let pages = app + UInt64(vm.wire_count) + UInt64(vm.compressor_page_count)
            memoryUsed = min(memoryTotal, pages * UInt64(vm_kernel_page_size))
        }

        var iterator: io_iterator_t = 0
        gpu = nil
        gpuTemperature = nil
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"),
                                           &iterator) == KERN_SUCCESS else { return }
        defer { IOObjectRelease(iterator) }
        while true {
            let service = IOIteratorNext(iterator)
            guard service != IO_OBJECT_NULL else { break }
            defer { IOObjectRelease(service) }
            let data = IORegistryEntryCreateCFProperty(
                service, "PerformanceStatistics" as CFString, kCFAllocatorDefault, 0
            )?.takeRetainedValue() as? [String: NSNumber]
            if let temperature = data?["Temperature(C)"]?.intValue, (1...125).contains(temperature) {
                gpuTemperature = temperature
                gpu = data?["GPU Core Utilization %"]?.intValue
                break
            }
        }
    }

    private func packageTemperature() -> Int? {
        let service = findMuxService()
        guard service != IO_OBJECT_NULL else { return nil }
        defer { IOObjectRelease(service) }
        let reading = IORegistryEntryCreateCFProperty(
            service, "OmenCPUPackageTemperature" as CFString, kCFAllocatorDefault, 0
        )?.takeRetainedValue() as? NSNumber
        return reading?.intValue
    }

    private func fanRows() -> [(String, String)] {
        let service = findMuxService()
        guard service != IO_OBJECT_NULL else { return [] }
        defer { IOObjectRelease(service) }
        let live = IORegistryEntryCreateCFProperty(
            service, "OmenTelemetryLive" as CFString, kCFAllocatorDefault, 0
        )?.takeRetainedValue() as? Bool ?? false
        let count = IORegistryEntryCreateCFProperty(
            service, "OmenFanCount" as CFString, kCFAllocatorDefault, 0
        )?.takeRetainedValue() as? NSNumber
        let label = live ? "" : "（启动时）"
        return (0..<min(count?.intValue ?? 0, 4)).map { index in
            let value = IORegistryEntryCreateCFProperty(
                service, "OmenFan\(index)RPM" as CFString, kCFAllocatorDefault, 0
            )?.takeRetainedValue() as? NSNumber
            return ("风扇 \(index + 1)\(label)", value.map { "\($0.uint32Value) RPM" } ?? "—")
        }
    }

    var rows: [(String, String)] {
        let used = Double(memoryUsed) / 1_073_741_824
        let total = Double(memoryTotal) / 1_073_741_824
        return [
            ("CPU 使用率", cpu.map { "\($0)%" } ?? "—"),
            ("内存使用量", String(format: "%.1f / %.1f GiB", used, total)),
            ("GPU 核心使用率", gpu.map { "\($0)%" } ?? "—"),
            ("CPU 温度", cpuTemperature.map { "\($0) °C" } ?? "—"),
            ("GPU 温度", gpuTemperature.map { "\($0) °C" } ?? "—"),
        ] + fanRows()
    }
}
