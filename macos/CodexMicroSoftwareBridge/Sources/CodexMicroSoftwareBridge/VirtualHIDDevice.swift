import Dispatch
import Foundation
import IOKit.hid
import CodexMicroBridgeCore

enum VirtualHIDError: LocalizedError {
    case creationFailed
    case notStarted
    case reportFailed(IOReturn)

    var errorDescription: String? {
        switch self {
        case .creationFailed:
            return "Unable to create the virtual HID device. The signed executable needs com.apple.developer.hid.virtual.device."
        case .notStarted:
            return "The virtual HID device has not started."
        case let .reportFailed(code):
            return String(format: "Unable to submit virtual HID report (0x%08x).", code)
        }
    }
}

final class VirtualHIDDevice {
    typealias OutputReportHandler = (_ reportID: UInt32, _ data: Data) -> Void

    private let queue = DispatchQueue(label: "codex.micro.virtual-hid")
    private var device: IOHIDUserDevice?
    private var outputReportHandler: OutputReportHandler?

    func start(outputReportHandler: @escaping OutputReportHandler) throws {
        self.outputReportHandler = outputReportHandler

        let serial = "MACOS-\(Host.current().localizedName?.uppercased() ?? "SOFTWARE")"
        let properties: [String: Any] = [
            kIOHIDReportDescriptorKey: CodexHID.reportDescriptor,
            kIOHIDVendorIDKey: CodexHID.vendorID,
            kIOHIDProductIDKey: CodexHID.productID,
            kIOHIDVersionNumberKey: CodexHID.versionNumber,
            kIOHIDProductKey: CodexHID.productName,
            kIOHIDManufacturerKey: CodexHID.manufacturerName,
            kIOHIDSerialNumberKey: serial,
            kIOHIDTransportKey: "Bluetooth",
            kIOHIDPrimaryUsagePageKey: 0xFF00,
            kIOHIDPrimaryUsageKey: 1,
            kIOHIDMaxInputReportSizeKey: CodexHID.reportDataSize + 1,
            kIOHIDMaxOutputReportSizeKey: CodexHID.reportDataSize + 1,
        ]

        // IOHIDUserDeviceOptionsCreateOnActivate == 1 << 0. The Swift
        // overlay doesn't currently publish the enum case, so use its ABI value.
        guard let created = IOHIDUserDeviceCreateWithProperties(
            kCFAllocatorDefault,
            properties as CFDictionary,
            1
        ) else {
            throw VirtualHIDError.creationFailed
        }
        device = created

        IOHIDUserDeviceSetDispatchQueue(created, queue)
        IOHIDUserDeviceRegisterSetReportBlock(created) { [weak self] _, reportID, bytes, length in
            guard length >= 0 else { return kIOReturnBadArgument }
            self?.outputReportHandler?(reportID, Data(bytes: bytes, count: length))
            return kIOReturnSuccess
        }
        IOHIDUserDeviceRegisterGetReportBlock(created) { _, _, bytes, length in
            let count = min(max(0, length.pointee), CodexHID.reportDataSize)
            bytes.initialize(repeating: 0, count: count)
            length.pointee = count
            return kIOReturnSuccess
        }
        IOHIDUserDeviceActivate(created)
    }

    func send(_ report: Data) throws {
        guard let device else { throw VirtualHIDError.notStarted }
        let result: IOReturn = report.withUnsafeBytes { rawBuffer in
            guard let address = rawBuffer.baseAddress?.assumingMemoryBound(to: UInt8.self) else {
                return kIOReturnBadArgument
            }
            return IOHIDUserDeviceHandleReportWithTimeStamp(
                device,
                mach_absolute_time(),
                address,
                report.count
            )
        }
        guard result == kIOReturnSuccess else { throw VirtualHIDError.reportFailed(result) }
    }

    func stop() {
        if let device {
            IOHIDUserDeviceCancel(device)
        }
        device = nil
        outputReportHandler = nil
    }
}
