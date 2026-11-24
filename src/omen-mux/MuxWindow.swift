import SwiftUI
import AppKit
import IOKit

@main
struct OmenMuxApp: App {
    var body: some Scene {
        WindowGroup("OMEN MUX") { MuxWindow() }
            .defaultSize(width: 795, height: 520)
            .windowResizability(.contentMinSize)
    }
}

private struct CardOutline: ViewModifier {
    let surface: Color
    let border: Color

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: 10)
        content
            .background(surface, in: shape)
            .overlay(shape.strokeBorder(border, lineWidth: 1).allowsHitTesting(false))
    }
}

struct MuxWindow: View {
    @State private var metrics = Metrics()
    @State private var current = "正在读取…"
    @State private var feedback = ""
    @State private var available = false
    @State private var fanAvailable = false
    @State private var fanMode: UInt8?
    @State private var profile: UInt8?
    @State private var profileWritable = false
    @State private var working = false
    @State private var restartPrompt = false
    @State private var pendingMode: UInt8?
    @State private var gpuMode: UInt8?

    @Environment(\.colorScheme) private var colorScheme

    private var accent: Color {
        colorScheme == .dark ? Color(red: 0.39, green: 0.86, blue: 0.79)
                             : Color(red: 0.03, green: 0.42, blue: 0.39)
    }

    private var controlSurface: Color {
        colorScheme == .dark ? Color(red: 0.13, green: 0.19, blue: 0.20)
                             : Color(red: 0.91, green: 0.96, blue: 0.95)
    }

    private var monitorSurface: Color {
        colorScheme == .dark ? Color(red: 0.16, green: 0.18, blue: 0.22)
                             : Color(red: 0.94, green: 0.96, blue: 0.98)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack {
                    VStack(alignment: .leading, spacing: 5) {
                        Text("OMEN MUX").font(.system(size: 31, weight: .bold, design: .monospaced))
                        Text("硬件监控与控制").font(.subheadline).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(action: refresh) { Label("刷新状态", systemImage: "arrow.clockwise") }
                        .disabled(working)
                }

                LazyVGrid(columns: [GridItem(.adaptive(minimum: 220), spacing: 10)], spacing: 10) {
                    controlCard("性能模式", icon: "gauge.with.dots.needle.67percent",
                                status: profile.flatMap { $0 <= 2 ? ["性能", "平衡", "低温"][Int($0)] : nil } ?? "暂无读数") {
                        modeButton("性能", selected: profile == 0) { request("performance", action: "profile") }
                        modeButton("平衡", selected: profile == 1) { request("balanced", action: "profile") }
                        modeButton("低温", selected: profile == 2) { request("cool", action: "profile") }
                    }
                    .disabled(!profileWritable || profile.map { $0 > 2 } != false || working)

                    controlCard("风扇控制", icon: "fan",
                                status: fanMode.map { $0 == 1 ? "全速" : "自动" } ?? "暂无读数") {
                        modeButton("全速", selected: fanMode == 1) { request("max", action: "fan") }
                        modeButton("自动", selected: fanMode == 0) { request("auto", action: "fan") }
                    }
                    .disabled(!fanAvailable || working)

                    controlCard("显卡模式", icon: "display", status: current) {
                        modeButton("混合模式", selected: gpuMode == 0) { request("hybrid") }
                        modeButton("独显直连", selected: gpuMode == 1) { request("discrete") }
                    }
                    .disabled(!available || working)
                }

                Label("实时监控", systemImage: "waveform.path").font(.headline)
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 10)], spacing: 10) {
                    ForEach(metrics.rows, id: \.0) { row in
                        VStack(alignment: .leading, spacing: 8) {
                            Text(row.0).font(.caption).foregroundStyle(.secondary)
                            Text(row.1).font(.system(size: 21, weight: .semibold, design: .monospaced))
                                .foregroundStyle(accent).lineLimit(1).minimumScaleFactor(0.65)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(13)
                        .modifier(CardOutline(surface: monitorSurface, border: Color.primary.opacity(colorScheme == .dark ? 0.22 : 0.16)))
                    }
                }

                if !feedback.isEmpty {
                    Text(feedback).font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
            .padding(24)
        }
        .frame(minWidth: 320, minHeight: 260)
        .background(Color(nsColor: .windowBackgroundColor))
        .tint(accent)
        .task {
            refresh()
            while !Task.isCancelled {
                metrics.sample()
                refreshControls()
                try? await Task.sleep(for: .seconds(1))
            }
        }
        .alert("重启后应用新的显卡模式", isPresented: $restartPrompt) {
            Button("稍后重启", role: .cancel) {}
            Button("立即重启") { restart() }
        } message: {
            Text("固件已接受切换请求。请先保存工作。")
        }
    }

    private func controlCard<Actions: View>(_ title: String, icon: String, status: String,
                                            @ViewBuilder actions: () -> Actions) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(title, systemImage: icon).font(.headline)
            Text(status).font(.callout).foregroundStyle(.secondary)
                .frame(minHeight: 34, alignment: .topLeading)
            HStack(spacing: 7, content: actions)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(CardOutline(surface: controlSurface, border: Color.primary.opacity(colorScheme == .dark ? 0.22 : 0.16)))
    }

    private func modeButton(_ title: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 4) {
                if selected {
                    Image(systemName: "checkmark")
                        .accessibilityHidden(true)
                }
                Text(title)
            }
            .font(.system(size: 12, weight: selected ? .bold : .medium))
            .foregroundStyle(selected ? (colorScheme == .dark ? Color.black : Color.white) : Color.primary)
            .padding(.horizontal, 9).padding(.vertical, 7)
            .background(selected ? accent : Color.primary.opacity(0.07),
                        in: RoundedRectangle(cornerRadius: 6))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func refresh() {
        pendingMode = nil
        gpuMode = nil
        let service = findMuxService()
        guard service != IO_OBJECT_NULL else {
            current = "尚未加载 OMEN MUX 驱动"
            available = false
            return
        }
        defer { IOObjectRelease(service) }

        func value(_ name: String) -> UInt8? {
            let property = IORegistryEntryCreateCFProperty(service, name as CFString, kCFAllocatorDefault, 0)
            return (property?.takeRetainedValue() as? NSNumber)?.uint8Value
        }
        let canWrite = IORegistryEntryCreateCFProperty(
            service, "OmenMuxDirectWriteEnabled" as CFString, kCFAllocatorDefault, 0
        )?.takeRetainedValue() as? Bool
        guard let mode = value("OmenMuxModeByte") else {
            current = "固件未返回模式"
            available = false
            return
        }
        gpuMode = mode & 0x7f
        switch mode & 0x7f {
        case 0: current = "当前：混合模式"
        case 1: current = "当前：独显直连"
        default: current = String(format: "当前固件模式：0x%02x", mode)
        }
        available = canWrite == true && (mode & 0x7f) <= 1
        pendingMode = value("OmenMuxPendingMode")
        if let pending = pendingMode {
            current += pending == 0 ? "\n待重启确认：混合模式" : "\n待重启确认：独显直连"
        }
    }

    private func refreshControls() {
        let service = findMuxService()
        guard service != IO_OBJECT_NULL else {
            fanAvailable = false
            fanMode = nil
            profile = nil
            profileWritable = false
            return
        }
        defer { IOObjectRelease(service) }
        func property(_ key: String) -> CFTypeRef? {
            IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue()
        }
        fanAvailable = property("OmenFanControlEnabled") as? Bool == true
        fanMode = (property("OmenFanMaxState") as? NSNumber)?.uint8Value
        profile = (property("OmenPerformanceRaw") as? NSNumber)?.uint8Value
        profileWritable = property("OmenPerformanceControlEnabled") as? Bool == true
    }

    private func request(_ target: String, action: String = "set") {
        guard let executable = Bundle.main.url(forAuxiliaryExecutable: "omen-mux") else {
            feedback = "找不到显卡切换程序。"
            return
        }
        working = true
        Task {
            let result = await Task.detached {
                Self.run(executable, arguments: [action, target])
            }.value
            feedback = result.output
            working = false
            refresh()
            let expected: UInt8 = target == "hybrid" ? 0 : 1
            if action == "set", result.success, pendingMode == expected {
                restartPrompt = true
            }
        }
    }

    private func restart() {
        working = true
        Task {
            let result = await Task.detached {
                Self.run(URL(fileURLWithPath: "/usr/bin/osascript"),
                         arguments: ["-e", "tell application \"System Events\" to restart"])
            }.value
            working = false
            if !result.success {
                feedback = "无法请求重启。请从苹果菜单选择重新启动。\n" + result.output
            }
        }
    }

    private nonisolated static func run(_ executable: URL, arguments: [String])
        -> (success: Bool, output: String) {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        do {
            try process.run()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return (process.terminationStatus == 0,
                    String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
        } catch {
            return (false, error.localizedDescription)
        }
    }
}
