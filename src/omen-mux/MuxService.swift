import Foundation
import IOKit

func findMuxService() -> io_service_t {
    var services: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("OmenMuxProbe"),
                                       &services) == KERN_SUCCESS else {
        return IO_OBJECT_NULL
    }
    defer { IOObjectRelease(services) }
    while true {
        let service = IOIteratorNext(services)
        guard service != IO_OBJECT_NULL else { return IO_OBJECT_NULL }
        if let method = IORegistryEntryCreateCFProperty(
            service, "OmenMuxMethod" as CFString, kCFAllocatorDefault, 0
        ) {
            _ = method.takeRetainedValue()
            return service
        }
        IOObjectRelease(service)
    }
}
