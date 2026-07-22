import Foundation

public enum CodexHID {
    public static let vendorID = 0x303A
    public static let productID = 0x8360
    public static let versionNumber = 0x0100
    public static let productName = "kbd-1.0-codex-micro"
    public static let manufacturerName = "Work Louder"

    public static let reportID: UInt8 = 6
    public static let reportDataSize = 63
    public static let rpcChannel: UInt8 = 2
    public static let rpcChunkSize = 61

    /// Vendor-defined application collection used by the physical bridge.
    /// Report ID 6 carries 63 data bytes in both directions.
    public static let reportDescriptor = Data([
        0x06, 0x00, 0xFF,             // Usage Page (Vendor 0xFF00)
        0x09, 0x01,                   // Usage (1)
        0xA1, 0x01,                   // Collection (Application)
        0x85, reportID,               // Report ID (6)
        0x15, 0x00,                   // Logical Minimum (0)
        0x26, 0xFF, 0x00,             // Logical Maximum (255)
        0x75, 0x08,                   // Report Size (8)
        0x95, UInt8(reportDataSize),  // Report Count (63)
        0x09, 0x01,
        0x81, 0x02,                   // Input (Data, Variable, Absolute)
        0x95, UInt8(reportDataSize),
        0x09, 0x01,
        0x91, 0x02,                   // Output (Data, Variable, Absolute)
        0xC0,                         // End Collection
    ])
}

public enum CodexReportFramer {
    /// Produces complete reports including the leading report ID byte.
    public static func encode(text: String) -> [Data] {
        encode(bytes: Data(text.utf8))
    }

    /// Produces complete reports including the leading report ID byte.
    public static func encode(bytes: Data) -> [Data] {
        guard !bytes.isEmpty else { return [] }

        var reports: [Data] = []
        var offset = 0
        while offset < bytes.count {
            let count = min(CodexHID.rpcChunkSize, bytes.count - offset)
            var report = Data(repeating: 0, count: CodexHID.reportDataSize + 1)
            report[0] = CodexHID.reportID
            report[1] = CodexHID.rpcChannel
            report[2] = UInt8(count)
            report.replaceSubrange(3..<(3 + count), with: bytes[offset..<(offset + count)])
            reports.append(report)
            offset += count
        }
        return reports
    }

    /// Normalizes macOS callbacks that may expose the report ID separately or
    /// include it at byte zero, then returns the RPC byte-stream fragment.
    public static func decode(reportID: UInt32, data: Data) -> Data? {
        guard reportID == UInt32(CodexHID.reportID) || data.first == CodexHID.reportID else {
            return nil
        }

        var normalized = data
        if normalized.first == CodexHID.reportID {
            // Data.removeFirst() advances startIndex instead of rebasing it.
            // Re-materialize so the fixed HID offsets below remain zero-based.
            normalized = Data(normalized.dropFirst())
        }
        guard normalized.count >= 2, normalized[0] == CodexHID.rpcChannel else {
            return nil
        }

        let announcedLength = min(Int(normalized[1]), CodexHID.rpcChunkSize)
        let availableLength = max(0, normalized.count - 2)
        let payloadLength = min(announcedLength, availableLength)
        return normalized.subdata(in: 2..<(2 + payloadLength))
    }
}
