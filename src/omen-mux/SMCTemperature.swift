import Foundation
import IOKit

enum SMCTemperature {
    static func cpu() -> Int? {
        let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"))
        guard service != IO_OBJECT_NULL else { return nil }
        defer { IOObjectRelease(service) }
        var connection: io_connect_t = 0
        guard IOServiceOpen(service, mach_task_self_, 0, &connection) == KERN_SUCCESS else {
            return nil
        }
        defer { IOServiceClose(connection) }
        for key in ["TC0D", "TC0P"] {
            if let temperature = read(key, on: connection) { return temperature }
        }
        return nil
    }

    private static func read(_ key: String, on connection: io_connect_t) -> Int? {
        // AppleSMC key-data layout on Intel: key, key info, command, value bytes.
        let encodedKey = key.utf8.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        var request = [UInt8](repeating: 0, count: 80)
        for index in 0..<4 {
            request[index] = UInt8(truncatingIfNeeded: encodedKey >> (index * 8))
        }
        request[42] = 9 // Read key metadata
        guard let info = call(request, on: connection),
              Array(info[32..<36]) == [0x38, 0x37, 0x70, 0x73],
              Array(info[28..<32]) == [2, 0, 0, 0] else { return nil }
        request[28] = 2
        request[42] = 5 // Read key value
        guard let reply = call(request, on: connection) else { return nil }
        let raw = Int16(bitPattern: UInt16(reply[48]) << 8 | UInt16(reply[49]))
        let value = Int((Double(raw) / 256).rounded())
        return (1...125).contains(value) ? value : nil
    }

    private static func call(_ request: [UInt8], on connection: io_connect_t) -> [UInt8]? {
        var response = [UInt8](repeating: 0, count: request.count)
        var size = response.count
        let status = request.withUnsafeBytes { input in
            response.withUnsafeMutableBytes { output in
                IOConnectCallStructMethod(connection, 2, input.baseAddress, input.count,
                                          output.baseAddress, &size)
            }
        }
        return status == KERN_SUCCESS && size == request.count && response[40] == 0 ? response : nil
    }
}
