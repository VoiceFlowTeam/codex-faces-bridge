import Foundation

public struct CodexThreadLighting: Equatable, Sendable {
    public var id: Int
    public var color: UInt32
    public var brightness: Double
    public var effect: Int
    public var speed: Double

    public init(id: Int, color: UInt32 = 0, brightness: Double = 0, effect: Int = 0, speed: Double = 0) {
        self.id = id
        self.color = color
        self.brightness = brightness
        self.effect = effect
        self.speed = speed
    }
}

public struct CodexLightingState: Equatable, Sendable {
    public var threads = (0..<6).map { CodexThreadLighting(id: $0) }
    public var ambient: [String: Double] = [:]
    public var keys: [String: Double] = [:]

    public init() {}

    public static func == (lhs: CodexLightingState, rhs: CodexLightingState) -> Bool {
        lhs.threads == rhs.threads && NSDictionary(dictionary: lhs.ambient).isEqual(to: rhs.ambient)
            && NSDictionary(dictionary: lhs.keys).isEqual(to: rhs.keys)
    }
}

public final class CodexProtocolEngine {
    public typealias ReportSink = (Data) -> Void
    public typealias LightingSink = (CodexLightingState) -> Void
    public typealias LogSink = (String) -> Void

    public static let softwareVersion = "0.3.0-macos-software"

    private let queue = DispatchQueue(label: "codex.micro.protocol")
    private let reportSink: ReportSink
    private let lightingSink: LightingSink
    private let logSink: LogSink
    private var receiveBuffer = Data()
    private var lighting = CodexLightingState()
    private var profileIndex = 0
    private var layerIndex = 0

    public init(
        reportSink: @escaping ReportSink,
        lightingSink: @escaping LightingSink = { _ in },
        logSink: @escaping LogSink = { _ in }
    ) {
        self.reportSink = reportSink
        self.lightingSink = lightingSink
        self.logSink = logSink
    }

    public func receiveOutputReport(reportID: UInt32, data: Data) {
        guard let payload = CodexReportFramer.decode(reportID: reportID, data: data) else {
            logSink("Ignoring non-Codex HID output report id=\(reportID) bytes=\(data.count)")
            return
        }
        queue.async { [weak self] in
            self?.feed(payload)
        }
    }

    public func setDeviceState(profile: Int, layer: Int) {
        queue.async { [weak self] in
            self?.profileIndex = profile
            self?.layerIndex = layer
        }
    }

    public func sendKey(_ key: String, pressed: Bool, agentIndex: Int? = nil) {
        var params: [String: Any] = ["k": key, "act": pressed ? 1 : 0]
        if let agentIndex { params["ag"] = agentIndex }
        sendNotification(method: "v.oai.hid", params: params)
    }

    public func sendHIDAction(_ key: String, action: Int, agentIndex: Int? = nil) {
        var params: [String: Any] = ["k": key, "act": action]
        if let agentIndex { params["ag"] = agentIndex }
        sendNotification(method: "v.oai.hid", params: params)
    }

    public func sendJoystick(angle: Double, distance: Double) {
        sendNotification(method: "v.oai.rad", params: ["a": angle, "d": distance])
    }

    private func feed(_ bytes: Data) {
        for byte in bytes {
            switch byte {
            case 0x0A:
                if !receiveBuffer.isEmpty {
                    processLine(receiveBuffer)
                    receiveBuffer.removeAll(keepingCapacity: true)
                }
            case 0x00, 0x0D:
                continue
            default:
                if receiveBuffer.count >= 2047 {
                    logSink("RPC receive buffer overflow; dropping message")
                    receiveBuffer.removeAll(keepingCapacity: true)
                } else {
                    receiveBuffer.append(byte)
                }
            }
        }
    }

    private func processLine(_ line: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: line),
            let request = object as? [String: Any]
        else {
            logSink("Ignoring malformed JSON-RPC message")
            return
        }

        let requestID = request["id"] ?? request["i"] ?? NSNull()
        guard let method = (request["method"] ?? request["m"]) as? String else {
            sendError(id: requestID, code: -32600, message: "Invalid request")
            return
        }
        let params = request["params"] ?? request["p"] ?? NSNull()
        logSink("RPC \(method)")

        switch method {
        case "sys.version":
            sendResult(id: requestID, result: ["version": Self.softwareVersion])
        case "device.status":
            sendResult(id: requestID, result: [
                "version": Self.softwareVersion,
                "profile_index": profileIndex,
                "layer_index": layerIndex,
                "battery": 100,
                "is_charging": true,
            ])
        case "v.oai.thstatus":
            applyThreadLighting(params)
            sendResult(id: requestID, result: true)
        case "v.oai.rgbcfg":
            applyRGBConfiguration(params)
            sendResult(id: requestID, result: true)
        default:
            sendError(id: requestID, code: -32601, message: "Method not found")
        }
    }

    private func applyThreadLighting(_ raw: Any) {
        guard let values = raw as? [[String: Any]] else { return }
        for value in values {
            guard let id = integer(value["id"]), lighting.threads.indices.contains(id) else { continue }
            var thread = lighting.threads[id]
            if let color = integer(value["c"]) { thread.color = UInt32(clamping: color) }
            if let brightness = double(value["b"]) { thread.brightness = brightness }
            if let effect = integer(value["e"]) { thread.effect = effect }
            if let speed = double(value["s"]) { thread.speed = speed }
            lighting.threads[id] = thread
        }
        lightingSink(lighting)
    }

    private func applyRGBConfiguration(_ raw: Any) {
        guard let value = raw as? [String: Any] else { return }
        lighting.ambient = numericDictionary(value["ambient"])
        lighting.keys = numericDictionary(value["keys"])
        lightingSink(lighting)
    }

    private func sendNotification(method: String, params: [String: Any]) {
        queue.async { [weak self] in
            self?.sendJSONObject(["method": method, "params": params])
        }
    }

    private func sendResult(id: Any, result: Any) {
        sendJSONObject(["id": id, "result": result])
    }

    private func sendError(id: Any, code: Int, message: String) {
        sendJSONObject(["id": id, "error": ["code": code, "message": message]])
    }

    private func sendJSONObject(_ object: [String: Any]) {
        guard JSONSerialization.isValidJSONObject(object), var encoded = try? JSONSerialization.data(withJSONObject: object) else {
            logSink("Unable to encode JSON-RPC message")
            return
        }
        encoded.append(0x0A)
        for report in CodexReportFramer.encode(bytes: encoded) {
            reportSink(report)
        }
    }

    private func integer(_ value: Any?) -> Int? {
        if let value = value as? Int { return value }
        if let value = value as? NSNumber { return value.intValue }
        return nil
    }

    private func double(_ value: Any?) -> Double? {
        if let value = value as? Double { return value }
        if let value = value as? NSNumber { return value.doubleValue }
        return nil
    }

    private func numericDictionary(_ raw: Any?) -> [String: Double] {
        guard let source = raw as? [String: Any] else { return [:] }
        return source.reduce(into: [:]) { result, item in
            if let number = double(item.value) { result[item.key] = number }
        }
    }
}
