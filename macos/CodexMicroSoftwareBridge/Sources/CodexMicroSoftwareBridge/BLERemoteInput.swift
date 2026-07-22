@preconcurrency import CoreBluetooth
import Foundation

struct BLERemoteMapping: Decodable {
    struct Entry: Decodable {
        let characteristic: String
        let value: String
        let key: String?
        let pressed: Bool?
        let angle: Double?
        let distance: Double?
    }

    let entries: [Entry]

    static func load(path: String?) throws -> BLERemoteMapping {
        guard let path else { return BLERemoteMapping(entries: []) }
        return try JSONDecoder().decode(BLERemoteMapping.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
    }
}

final class BLERemoteInput: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let nameFilter: String
    private let listOnly: Bool
    private let mapping: BLERemoteMapping
    private let keyHandler: (_ key: String, _ pressed: Bool) -> Void
    private let radialHandler: (_ angle: Double, _ distance: Double) -> Void
    private let log: (String) -> Void
    private let queue = DispatchQueue(label: "codex.micro.remote-ble")
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var seenPeripherals = Set<UUID>()

    init(
        nameFilter: String,
        listOnly: Bool = false,
        mapping: BLERemoteMapping,
        keyHandler: @escaping (_ key: String, _ pressed: Bool) -> Void,
        radialHandler: @escaping (_ angle: Double, _ distance: Double) -> Void,
        log: @escaping (String) -> Void
    ) {
        self.nameFilter = nameFilter.lowercased()
        self.listOnly = listOnly
        self.mapping = mapping
        self.keyHandler = keyHandler
        self.radialHandler = radialHandler
        self.log = log
        super.init()
        central = CBCentralManager(delegate: self, queue: queue)
    }

    func stop() {
        central.stopScan()
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            log("BLE central state: \(central.state.rawValue)")
            return
        }
        if listOnly {
            log("Listing nearby BLE advertisements")
        } else {
            log("Scanning for BLE remote containing name: \(nameFilter)")
        }
        central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: listOnly])
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let name = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? ""
        if listOnly {
            guard seenPeripherals.insert(peripheral.identifier).inserted else { return }
            log("BLE advertisement name=\(name.isEmpty ? "<unnamed>" : name) id=\(peripheral.identifier.uuidString) rssi=\(RSSI)")
            return
        }
        guard name.lowercased().contains(nameFilter) else { return }
        self.peripheral = peripheral
        central.stopScan()
        peripheral.delegate = self
        log("Connecting BLE remote: \(name) rssi=\(RSSI)")
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("BLE remote connected: \(peripheral.name ?? peripheral.identifier.uuidString)")
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("BLE remote disconnected: \(error?.localizedDescription ?? "no error")")
        self.peripheral = nil
        central.scanForPeripherals(withServices: nil)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { log("Service discovery failed: \(error.localizedDescription)"); return }
        for service in peripheral.services ?? [] {
            log("BLE service \(service.uuid.uuidString)")
            peripheral.discoverCharacteristics(nil, for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error { log("Characteristic discovery failed: \(error.localizedDescription)"); return }
        for characteristic in service.characteristics ?? [] {
            let properties = characteristic.properties
            log("BLE characteristic \(characteristic.uuid.uuidString) properties=0x\(String(properties.rawValue, radix: 16))")
            if properties.contains(.notify) || properties.contains(.indicate) {
                peripheral.setNotifyValue(true, for: characteristic)
            }
            if properties.contains(.read) {
                peripheral.readValue(for: characteristic)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, let data = characteristic.value else { return }
        let value = data.map { String(format: "%02x", $0) }.joined()
        let characteristicID = characteristic.uuid.uuidString.uppercased()
        log("BLE notify \(characteristicID) \(value)")

        guard let entry = mapping.entries.first(where: {
            $0.characteristic.uppercased() == characteristicID && $0.value.lowercased() == value
        }) else { return }

        if let key = entry.key {
            keyHandler(key, entry.pressed ?? true)
        } else if let angle = entry.angle, let distance = entry.distance {
            radialHandler(angle, distance)
        }
    }
}
