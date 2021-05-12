//
//  OpenHaystack – Tracking personal Bluetooth devices via Apple's Find My network
//
//  Copyright © 2021 Secure Mobile Networking Lab (SEEMOO)
//  Copyright © 2021 The Open Wireless Link Project
//
//  SPDX-License-Identifier: AGPL-3.0-only
//

import Combine
import Foundation
import SwiftUI
import CryptoKit


func byteArray<T>(from value: T) -> [UInt8] where T: FixedWidthInteger {
    withUnsafeBytes(of: value.bigEndian, Array.init)
}

extension Digest {
    var bytes: [UInt8] { Array(makeIterator()) }
    var data: Data { Data(bytes) }

    var hexStr: String {
        bytes.map { String(format: "%02X", $0) }.joined()
    }
}


class FindMyController: ObservableObject {
  static let shared = FindMyController()

  @Published var error: Error?
  @Published var devices = [ModemDevice]()
  @Published var messages = [UInt32: Message]()

  @Published var modemID: UInt32 = 0
  @Published var batch_size: UInt32 = 128

  func clearMessages() {
     self.messages = [UInt32: Message]()
  }

  func fetchBitsUntilEnd(
    for modemID: UInt32, message messageID: UInt32, startBit: UInt32, bitCount: UInt32, with searchPartyToken: Data, completion: @escaping (Error?) -> Void
    ) {
    
    let fill_0: [UInt8] = [0, 0, 0, 0, 0, 0]
    let static_prefix: [UInt8] = [0xba, 0xbe]

       var m = self.messages[messageID]!
        for bit in startBit..<startBit+bitCount {
            for v in 0...1 {
                var validKeyCounter: UInt32 = 0
                var adv_key = [UInt8]()
                repeat {
                    adv_key = static_prefix + byteArray(from: UInt32(bit)) + byteArray(from: UInt32(messageID)) + byteArray(from: m.modemID)
                    adv_key += byteArray(from: validKeyCounter) + fill_0 + byteArray(from: UInt32(v)) //2 lines, otherwise the XCode type checker takes too long
                    validKeyCounter += 1
                    print("==== Testing key")
                } while (BoringSSL.isPublicKeyValid(Data(adv_key)) == 0)
                print("Found valid pub key on \(validKeyCounter). try")
                let k = DataEncodingKey(index: UInt32(bit), bitValue: UInt8(v), advertisedKey: adv_key, hashedKey: SHA256.hash(data: adv_key).data)
                m.keys.append(k)
                print(Data(adv_key).base64EncodedString())
            }
        }
       m.fetchedBits = startBit + bitCount
      self.messages[UInt32(messageID)] = m
      // Includes async fetch if finished, otherwise fetches more bits
      self.fetchReports(for: messageID, with: searchPartyToken, completion: completion)
  }

  func fetchMessage(
    for modemID: UInt32, message messageID: UInt32, with searchPartyToken: Data, completion: @escaping (Error?) -> Void
    ) {
    
    self.modemID = modemID
    let start_index: UInt32 = 0
    let message_finished = false;
    let m = Message(modemID: modemID, messageID: UInt32(messageID))
    self.messages[messageID] = m
 
    fetchBitsUntilEnd(for: modemID, message: messageID, startBit: start_index, bitCount: self.batch_size, with: searchPartyToken, completion: completion);
  }



  func fetchReports(for messageID: UInt32, with searchPartyToken: Data, completion: @escaping (Error?) -> Void) {

    DispatchQueue.global(qos: .background).async {
      let fetchReportGroup = DispatchGroup()

      let fetcher = ReportsFetcher()
    

        fetchReportGroup.enter()

        let keys = self.messages[messageID]!.keys

        let keyHashes = keys.map({ $0.hashedKey.base64EncodedString() })

        // 21 days reduced to 1 day
        let duration: Double = (24 * 60 * 60) * 1
        let startDate = Date() - duration

        fetcher.query(
          forHashes: keyHashes,
          start: startDate,
          duration: duration,
          searchPartyToken: searchPartyToken
        ) { jd in
          guard let jsonData = jd else {
            fetchReportGroup.leave()
            return
          }

          do {
            // Decode the report
            let report = try JSONDecoder().decode(FindMyReportResults.self, from: jsonData)
            self.messages[UInt32(messageID)]!.reports += report.results
          } catch {
            print("Failed with error \(error)")
            self.messages[UInt32(messageID)]!.reports = []
          }
          fetchReportGroup.leave()
        }

      // Completion Handler
      fetchReportGroup.notify(queue: .main) {
        print("Finished loading the reports. Now decode them")

        // Export the reports to the desktop
        var reports = [FindMyReport]()
        for (_, message) in self.messages {
          for report in message.reports {
            reports.append(report)
          }
        }
        DispatchQueue.main.async {
            self.decodeReports(messageID: messageID, with: searchPartyToken) { _ in completion(nil) }
          }

        }
      }
    }

  

    func decodeReports(messageID: UInt32, with searchPartyToken: Data, completion: @escaping (Error?) -> Void) {
      print("Decoding reports")

      // Iterate over all messages
      var message = messages[messageID]

      // Map the keys in a dictionary for faster access
      let reports = message!.reports
      let keyMap = message!.keys.reduce(
        into: [String: DataEncodingKey](), { $0[$1.hashedKey.base64EncodedString()] = $1 })

      var reportMap = [String: Int]()
      reports.forEach{ reportMap[$0.id, default:0] += 1 }

      //print(keyMap)
      //print(reportMap)
      var result = [UInt32: UInt8]()
      var earlyExit = false
      for (report_id, count) in reportMap {
        guard let k = keyMap[report_id] else { print("FATAL ERROR"); return; }
        result[k.index] = k.bitValue
          print("Bit \(k.index): \(k.bitValue) (\(count))")
      }
      var resultBitStr = ""
      var resultByteStr = ""
      var byte_valid = 1
      var byte_completely_invalid = 1
      if result.keys.max() == nil { print("No reports found"); completion(nil); return }
      for i in 0..<message!.fetchedBits {
          let v = result[i]
          if v == nil {
              byte_valid = 0
              resultBitStr += "?"
          } else {
              byte_completely_invalid = 0
              resultBitStr += String(v!)
          }
          let (quotient, remainder) = i.quotientAndRemainder(dividingBy: 8)
          if (remainder == 7) { // End of byte
            if byte_completely_invalid == 1 {
                earlyExit = true
                break
            }
            if byte_valid == 1 {
              print("Fetched a full byte")
              let valid_byte = UInt8(strtoul(String(resultBitStr[resultBitStr.index(resultBitStr.startIndex, offsetBy: String.IndexDistance(quotient*8))...resultBitStr.index(resultBitStr.startIndex, offsetBy: String.IndexDistance(quotient*8+7))]), nil, 2))
              print("Full byte \(valid_byte)")
              let str_byte = String(bytes: [valid_byte], encoding: .utf8)
              resultByteStr += str_byte ?? "?"
            }
            else {
              print("No full byte")
              resultByteStr += "?"
            }
            byte_valid = 1
            byte_completely_invalid = 1
          }
      }
      
      print("Result bitstring: \(resultBitStr)")
      print("Result bytestring: \(resultByteStr)")
      message?.decodedStr = resultByteStr
      self.messages[messageID] = message
      if earlyExit {
          print("Fetched a fully invalid byte. Message probably ended.")
          completion(nil)
          return
      }
      // Not finished yet -> Next round
      print("Haven't found end byte yet. Starting with bit \(result.keys.max) now")
      fetchBitsUntilEnd(for: modemID, message: messageID, startBit: UInt32(result.keys.max()!), bitCount: self.batch_size, with: searchPartyToken, completion: completion); // remove bitCount magic value   
   }
}


struct FindMyControllerKey: EnvironmentKey {
  static var defaultValue: FindMyController = .shared
}

extension EnvironmentValues {
  var findMyController: FindMyController {
    get { self[FindMyControllerKey.self] }
    set { self[FindMyControllerKey.self] = newValue }
  }
}

enum FindMyErrors: Error {
  case decodingPlistFailed(message: String)
}
