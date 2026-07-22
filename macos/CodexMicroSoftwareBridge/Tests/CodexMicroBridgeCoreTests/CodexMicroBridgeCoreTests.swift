import XCTest
@testable import CodexMicroBridgeCore

final class CodexMicroBridgeCoreTests: XCTestCase {
    func testFramingRoundTripAcrossMultipleReports() throws {
        let text = String(repeating: "abcdef", count: 30) + "\n"
        let reports = CodexReportFramer.encode(text: text)
        XCTAssertGreaterThan(reports.count, 1)
        XCTAssertTrue(reports.allSatisfy { $0.count == 64 && $0[0] == CodexHID.reportID })

        let decoded = reports.compactMap {
            CodexReportFramer.decode(reportID: UInt32(CodexHID.reportID), data: $0)
        }.reduce(into: Data()) { $0.append($1) }
        XCTAssertEqual(String(data: decoded, encoding: .utf8), text)
    }

    func testVersionRequestProducesResponse() throws {
        let responseExpectation = expectation(description: "version response")
        var output = Data()
        let engine = CodexProtocolEngine(reportSink: { report in
            if let payload = CodexReportFramer.decode(reportID: UInt32(CodexHID.reportID), data: report) {
                output.append(payload)
                if output.last == 0x0A { responseExpectation.fulfill() }
            }
        })

        let request = #"{"id":7,"method":"sys.version"}"# + "\n"
        for report in CodexReportFramer.encode(text: request) {
            engine.receiveOutputReport(reportID: UInt32(CodexHID.reportID), data: report)
        }
        wait(for: [responseExpectation], timeout: 1)

        let object = try XCTUnwrap(try JSONSerialization.jsonObject(with: output) as? [String: Any])
        XCTAssertEqual((object["id"] as? NSNumber)?.intValue, 7)
        let result = try XCTUnwrap(object["result"] as? [String: Any])
        XCTAssertEqual(result["version"] as? String, CodexProtocolEngine.softwareVersion)
    }

    func testKeyNotificationUsesExistingSemanticProtocol() throws {
        let responseExpectation = expectation(description: "key notification")
        var output = Data()
        let engine = CodexProtocolEngine(reportSink: { report in
            if let payload = CodexReportFramer.decode(reportID: UInt32(CodexHID.reportID), data: report) {
                output.append(payload)
                if output.last == 0x0A { responseExpectation.fulfill() }
            }
        })

        engine.sendKey("ACT06", pressed: true)
        wait(for: [responseExpectation], timeout: 1)

        let object = try XCTUnwrap(try JSONSerialization.jsonObject(with: output) as? [String: Any])
        XCTAssertEqual(object["method"] as? String, "v.oai.hid")
        let params = try XCTUnwrap(object["params"] as? [String: Any])
        XCTAssertEqual(params["k"] as? String, "ACT06")
        XCTAssertEqual((params["act"] as? NSNumber)?.intValue, 1)
    }
}
