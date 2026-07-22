import CodexMicroBridgeCore
import Dispatch
import Foundation

private struct Options {
    var hidProductFilter: String?
    var bleNameFilter: String?
    var bleMapPath: String?
    var enableHIDInput = false
    var enableStdin = true
    var verbose = false
    var protocolSelfTest = false
    var probeOnly = false

    static func parse(_ arguments: [String]) throws -> Options {
        var options = Options()
        var index = 0
        while index < arguments.count {
            switch arguments[index] {
            case "--hid-product":
                index += 1
                guard index < arguments.count else { throw usageError("--hid-product requires a value") }
                options.hidProductFilter = arguments[index]
                options.enableHIDInput = true
            case "--all-hid-input":
                options.enableHIDInput = true
            case "--ble-name":
                index += 1
                guard index < arguments.count else { throw usageError("--ble-name requires a value") }
                options.bleNameFilter = arguments[index]
            case "--ble-map":
                index += 1
                guard index < arguments.count else { throw usageError("--ble-map requires a value") }
                options.bleMapPath = arguments[index]
            case "--no-stdin": options.enableStdin = false
            case "--verbose": options.verbose = true
            case "--protocol-self-test": options.protocolSelfTest = true
            case "--probe-only": options.probeOnly = true
            case "--help", "-h":
                printUsage()
                exit(EXIT_SUCCESS)
            default:
                throw usageError("Unknown option: \(arguments[index])")
            }
            index += 1
        }
        return options
    }
}

private func usageError(_ message: String) -> NSError {
    NSError(domain: "CodexMicroSoftwareBridge", code: 2, userInfo: [NSLocalizedDescriptionKey: message])
}

private func printUsage() {
    print("""
    Usage: codex-micro-software-bridge [options]

      --hid-product NAME    Read only physical HID devices whose product contains NAME
      --all-hid-input       Read all physical HID devices (use carefully)
      --ble-name NAME       Scan/connect a private-GATT BLE remote containing NAME
      --ble-map FILE        JSON mapping for exact BLE characteristic notifications
      --no-stdin            Disable interactive stdin commands
      --verbose             Print RPC and raw input diagnostics
      --protocol-self-test  Exercise report framing/RPC without creating virtual HID
      --probe-only          Probe remote input without creating virtual HID

    Interactive commands:
      tap ACT06             Send press + release
      down ACT06            Send press
      up ACT06              Send release
      agent 0 tap           Tap AG00 for agent slot 0
      rad -90 1             Send radial angle/distance
      quit                  Stop the bridge
    """)
}

private final class BridgeApplication {
    private let options: Options
    private let virtualDevice = VirtualHIDDevice()
    private var protocolEngine: CodexProtocolEngine!
    private var hidInput: HIDRemoteInput?
    private var bleInput: BLERemoteInput?
    private let stopped = DispatchSemaphore(value: 0)

    init(options: Options) {
        self.options = options
    }

    func run() throws {
        if options.protocolSelfTest {
            try runProtocolSelfTest()
            return
        }

        protocolEngine = CodexProtocolEngine(
            reportSink: { [weak self] report in
                guard let self else { return }
                if self.options.probeOnly {
                    if self.options.verbose {
                        self.log("Would send Codex report: \(report.map { String(format: "%02x", $0) }.joined())")
                    }
                } else {
                    do { try self.virtualDevice.send(report) }
                    catch { self.log("Virtual HID input failed: \(error.localizedDescription)") }
                }
            },
            lightingSink: { [weak self] state in self?.showLighting(state) },
            logSink: { [weak self] message in
                if self?.options.verbose == true { self?.log(message) }
            }
        )
        if options.probeOnly {
            log("Probe-only mode: virtual Codex Micro creation is disabled")
        } else {
            try virtualDevice.start { [weak self] reportID, data in
                self?.protocolEngine.receiveOutputReport(reportID: reportID, data: data)
            }
            log("Virtual Codex Micro is active: vid=0x303A pid=0x8360 report=6")
        }

        if options.enableHIDInput {
            let input = HIDRemoteInput(
                productFilter: options.hidProductFilter,
                keyHandler: { [weak self] key, pressed in self?.protocolEngine.sendKey(key, pressed: pressed) },
                radialHandler: { [weak self] angle, distance in self?.protocolEngine.sendJoystick(angle: angle, distance: distance) },
                log: { [weak self] in self?.log($0) }
            )
            try input.start()
            hidInput = input
        }

        if let bleNameFilter = options.bleNameFilter {
            let mapping = try BLERemoteMapping.load(path: options.bleMapPath)
            bleInput = BLERemoteInput(
                nameFilter: bleNameFilter,
                mapping: mapping,
                keyHandler: { [weak self] key, pressed in self?.protocolEngine.sendKey(key, pressed: pressed) },
                radialHandler: { [weak self] angle, distance in self?.protocolEngine.sendJoystick(angle: angle, distance: distance) },
                log: { [weak self] in self?.log($0) }
            )
        }

        if options.enableStdin { startStdin() }
        log("Type 'help' for commands. Press Control-C to stop.")

        signal(SIGINT, SIG_IGN)
        let signalSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
        signalSource.setEventHandler { [weak self] in self?.stop() }
        signalSource.resume()
        stopped.wait()
        signalSource.cancel()
    }

    private func runProtocolSelfTest() throws {
        let received = DispatchSemaphore(value: 0)
        var bytes = Data()
        let engine = CodexProtocolEngine(reportSink: { report in
            if let payload = CodexReportFramer.decode(reportID: UInt32(CodexHID.reportID), data: report) {
                bytes.append(payload)
                if bytes.last == 0x0A { received.signal() }
            }
        })
        for report in CodexReportFramer.encode(text: #"{"id":1,"method":"sys.version"}"# + "\n") {
            engine.receiveOutputReport(reportID: UInt32(CodexHID.reportID), data: report)
        }
        guard received.wait(timeout: .now() + 2) == .success,
              let object = try JSONSerialization.jsonObject(with: bytes) as? [String: Any],
              let result = object["result"] as? [String: Any],
              result["version"] as? String == CodexProtocolEngine.softwareVersion
        else {
            throw usageError("Protocol self-test failed")
        }
        print("Protocol self-test passed (\(CodexProtocolEngine.softwareVersion)).")
    }

    private func startStdin() {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            while let line = readLine() {
                guard let self else { return }
                if !self.handleCommand(line) { break }
            }
            self?.stop()
        }
    }

    @discardableResult
    private func handleCommand(_ line: String) -> Bool {
        let parts = line.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        guard let command = parts.first?.lowercased() else { return true }

        switch command {
        case "tap" where parts.count == 2,
             "down" where parts.count == 2,
             "up" where parts.count == 2:
            let key = parts[1].uppercased()
            if command == "tap" {
                protocolEngine.sendKey(key, pressed: true)
                protocolEngine.sendKey(key, pressed: false)
            } else {
                protocolEngine.sendKey(key, pressed: command == "down")
            }
        case "agent" where parts.count == 3:
            guard let index = Int(parts[1]), (0..<6).contains(index) else {
                log("Agent index must be 0...5"); return true
            }
            let mode = parts[2].lowercased()
            let key = String(format: "AG%02d", index)
            if mode == "tap" {
                protocolEngine.sendKey(key, pressed: true, agentIndex: index)
                protocolEngine.sendKey(key, pressed: false, agentIndex: index)
            } else if mode == "down" || mode == "up" {
                protocolEngine.sendKey(key, pressed: mode == "down", agentIndex: index)
            } else {
                log("Agent mode must be tap, down, or up")
            }
        case "rad" where parts.count == 3:
            guard let angle = Double(parts[1]), let distance = Double(parts[2]) else {
                log("Usage: rad ANGLE DISTANCE"); return true
            }
            protocolEngine.sendJoystick(angle: angle, distance: distance)
        case "help": printUsage()
        case "quit", "exit": stop(); return false
        default: log("Unknown command. Type 'help'.")
        }
        return true
    }

    private func showLighting(_ state: CodexLightingState) {
        let values = state.threads.map {
            String(format: "%d:#%06X@%.2f/e%d", $0.id, $0.color, $0.brightness, $0.effect)
        }.joined(separator: " ")
        log("Codex lighting: \(values)")
    }

    private func log(_ message: String) {
        print("[Codex Micro] \(message)")
        fflush(stdout)
    }

    private func stop() {
        hidInput?.stop()
        bleInput?.stop()
        virtualDevice.stop()
        stopped.signal()
    }
}

do {
    let options = try Options.parse(Array(CommandLine.arguments.dropFirst()))
    let application = BridgeApplication(options: options)
    try application.run()
} catch {
    fputs("error: \(error.localizedDescription)\n", stderr)
    fputs("Run with --help for usage.\n", stderr)
    exit(EXIT_FAILURE)
}
