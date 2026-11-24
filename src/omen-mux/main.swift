import Foundation
import IOKit
import Darwin

func fail(_ message: String, code: Int32 = 1) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(code)
}

if CommandLine.arguments.dropFirst().elementsEqual(["metrics"]) {
    var metrics = Metrics()
    metrics.sample()
    Thread.sleep(forTimeInterval: 0.5)
    metrics.sample()
    for (name, value) in metrics.rows { print("\(name)  \(value)") }
    exit(0)
}

var key = "OmenMuxTarget"
let target: UInt8?
switch Array(CommandLine.arguments.dropFirst()) {
case ["status"], ["firmware"]: target = nil
case ["profile", "performance"]: key = "OmenPerformanceTarget"; target = 0
case ["profile", "balanced"]: key = "OmenPerformanceTarget"; target = 1
case ["profile", "cool"]: key = "OmenPerformanceTarget"; target = 2
case ["fan", "auto"]: key = "OmenFanMaxTarget"; target = 0
case ["fan", "max"]: key = "OmenFanMaxTarget"; target = 1
case ["set", "hybrid"]: target = 0
case ["set", "discrete"]: target = 1
default: fail("用法：omen-mux metrics | omen-mux firmware | omen-mux status | omen-mux set hybrid | omen-mux set discrete | omen-mux fan auto|max | omen-mux profile performance|balanced|cool", code: 64)
}
let service = findMuxService()
guard service != IO_OBJECT_NULL else {
    fail("未发现 OMEN MUX 探针。")
}
defer { IOObjectRelease(service) }

func readNumber(_ key: String) -> NSNumber? {
    IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?
        .takeRetainedValue() as? NSNumber
}

if CommandLine.arguments.dropFirst().elementsEqual(["firmware"]) {
    let keys = ["OmenMuxModeByte", "OmenFanCount", "OmenFanMaxState", "OmenPerformanceRaw"]
    for key in keys {
        let value = readNumber(key).map { String(format: "0x%02x", $0.uint32Value) } ?? "无读数"
        print("\(key)  \(value)")
    }
    exit(0)
}

if let target {
    let request = [key: NSNumber(value: target)] as CFDictionary
    let status = IORegistryEntrySetCFProperties(service, request)
    guard status == KERN_SUCCESS else {
        fail(String(format: "切换请求失败（IOKit 0x%08x）。", UInt32(bitPattern: status)))
    }
    if key == "OmenPerformanceTarget" {
        print("已读取并确认固件性能档。")
    } else if key == "OmenFanMaxTarget" {
        print("固件已接受风扇设置。请核对实际转速。")
    } else if readNumber("OmenMuxPendingMode") != nil {
        print("固件已接受请求。重启后请运行 omen-mux status 确认模式。")
    } else {
        print("固件已处于所选模式。")
    }
    exit(0)
}

guard let mode = readNumber("OmenMuxModeByte")?.uint8Value else {
    for key in ["OmenMuxWDGStatus", "OmenMuxReadStatus", "OmenMuxFirmwareStatus"] {
        if let status = readNumber(key) {
            print(String(format: "%@ = 0x%08x", key, status.uint32Value))
        }
    }
    fail("固件未返回显卡模式。")
}

let name: String
switch mode & 0x7f {
case 0: name = "混合模式"
case 1: name = "独显直连"
case 2: name = "Optimus"
case 3: name = "仅核显"
default: name = "未知模式"
}
print(String(format: "%@（固件模式字节 0x%02x）", name, UInt32(mode)))
if let pending = readNumber("OmenMuxPendingMode")?.uint8Value {
    print("待重启确认：" + (pending == 0 ? "混合模式" : "独显直连"))
}
