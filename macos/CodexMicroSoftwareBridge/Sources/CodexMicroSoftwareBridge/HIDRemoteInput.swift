import Foundation
import IOKit.hid
import CodexMicroBridgeCore

final class HIDRemoteInput {
    typealias KeyHandler = (_ key: String, _ pressed: Bool) -> Void
    typealias RadialHandler = (_ angle: Double, _ distance: Double) -> Void

    private let productFilter: String?
    private let keyHandler: KeyHandler
    private let radialHandler: RadialHandler
    private let log: (String) -> Void
    private let queue = DispatchQueue(label: "codex.micro.remote-hid")
    private var manager: IOHIDManager?
    private var acceptedDevices = Set<IOHIDDevice>()

    init(
        productFilter: String?,
        keyHandler: @escaping KeyHandler,
        radialHandler: @escaping RadialHandler,
        log: @escaping (String) -> Void
    ) {
        self.productFilter = productFilter?.lowercased()
        self.keyHandler = keyHandler
        self.radialHandler = radialHandler
        self.log = log
    }

    func start() throws {
        let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        self.manager = manager
        let context = Unmanaged.passUnretained(self).toOpaque()

        IOHIDManagerSetDeviceMatching(manager, nil)
        IOHIDManagerRegisterDeviceMatchingCallback(manager, { context, _, _, device in
            guard let context else { return }
            Unmanaged<HIDRemoteInput>.fromOpaque(context).takeUnretainedValue().deviceAdded(device)
        }, context)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, { context, _, _, device in
            guard let context else { return }
            Unmanaged<HIDRemoteInput>.fromOpaque(context).takeUnretainedValue().deviceRemoved(device)
        }, context)
        IOHIDManagerRegisterInputValueCallback(manager, { context, _, _, value in
            guard let context else { return }
            Unmanaged<HIDRemoteInput>.fromOpaque(context).takeUnretainedValue().input(value)
        }, context)
        IOHIDManagerSetDispatchQueue(manager, queue)
        let result = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        guard result == kIOReturnSuccess else {
            throw NSError(domain: NSMachErrorDomain, code: Int(result), userInfo: [
                NSLocalizedDescriptionKey: "Unable to open IOHIDManager. Input Monitoring permission may be required.",
            ])
        }
        IOHIDManagerActivate(manager)
    }

    func stop() {
        guard let manager else { return }
        IOHIDManagerCancel(manager)
        IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        self.manager = nil
        acceptedDevices.removeAll()
    }

    private func deviceAdded(_ device: IOHIDDevice) {
        let vendorID = numberProperty(device, key: kIOHIDVendorIDKey)?.intValue
        let productID = numberProperty(device, key: kIOHIDProductIDKey)?.intValue
        if vendorID == CodexHID.vendorID && productID == CodexHID.productID {
            return
        }

        let product = stringProperty(device, key: kIOHIDProductKey) ?? "Unknown HID"
        if let productFilter, !product.lowercased().contains(productFilter) {
            return
        }
        acceptedDevices.insert(device)
        log("HID input connected: \(product) vid=\(hex(vendorID)) pid=\(hex(productID))")
    }

    private func deviceRemoved(_ device: IOHIDDevice) {
        guard acceptedDevices.remove(device) != nil else { return }
        log("HID input disconnected: \(stringProperty(device, key: kIOHIDProductKey) ?? "Unknown HID")")
    }

    private func input(_ value: IOHIDValue) {
        let element = IOHIDValueGetElement(value)
        let device = IOHIDElementGetDevice(element)
        guard acceptedDevices.contains(device) else { return }

        let page = Int(IOHIDElementGetUsagePage(element))
        let usage = Int(IOHIDElementGetUsage(element))
        let pressed = IOHIDValueGetIntegerValue(value) != 0
        guard page != 0, usage != 0 else { return }

        if page == 0x07 {
            handleKeyboardUsage(usage, pressed: pressed)
        } else if page == 0x0C {
            handleConsumerUsage(usage, pressed: pressed)
        }
    }

    private func handleKeyboardUsage(_ usage: Int, pressed: Bool) {
        switch usage {
        case 0x52: radialHandler(-90, pressed ? 1 : 0) // Up
        case 0x51: radialHandler(90, pressed ? 1 : 0)  // Down
        case 0x50: radialHandler(180, pressed ? 1 : 0) // Left
        case 0x4F: radialHandler(0, pressed ? 1 : 0)   // Right
        case 0x28: keyHandler("ACT06", pressed)       // Return / OK
        case 0x29, 0x2A: keyHandler("ACT07", pressed) // Escape / Back
        case 0x2C: keyHandler("ACT08", pressed)       // Space / Play
        case 0x4A: keyHandler("ACT10", pressed)       // Home
        default: break
        }
    }

    private func handleConsumerUsage(_ usage: Int, pressed: Bool) {
        switch usage {
        case 0xCD: keyHandler("ACT08", pressed)       // Play/Pause
        case 0x40: keyHandler("ACT09", pressed)       // Menu
        case 0x223: keyHandler("ACT10", pressed)      // AC Home
        case 0x224: keyHandler("ACT07", pressed)      // AC Back
        case 0xE9, 0xB5: keyHandler("ACT11", pressed) // Volume+/Next
        case 0xEA, 0xB6: keyHandler("ACT12", pressed) // Volume-/Previous
        default: break
        }
    }

    private func stringProperty(_ device: IOHIDDevice, key: String) -> String? {
        IOHIDDeviceGetProperty(device, key as CFString) as? String
    }

    private func numberProperty(_ device: IOHIDDevice, key: String) -> NSNumber? {
        IOHIDDeviceGetProperty(device, key as CFString) as? NSNumber
    }

    private func hex(_ value: Int?) -> String {
        value.map { String(format: "0x%04X", $0) } ?? "unknown"
    }
}
