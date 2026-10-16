import Foundation

#if !os(watchOS)
import CoreNFC
#endif


enum SensorType: String, CustomStringConvertible {
    case libre1        = "Libre 1"
    case libreUS14day  = "Libre US 14d"
    case libreProH     = "Libre Pro/H"
    case libre2        = "Libre 2"
    case libre2Gen2    = "Libre 2 Gen2"
    case libre3        = "Libre 3"
    case libreX        = "Libre X"
    case lingo         = "Lingo"
    case libreSelect   = "Libre Select"
    case instinct      = "Instinct"
    case dexcomG6      = "Dexcom G6"
    case dexcomONE     = "Dexcom ONE"
    case dexcomG7      = "Dexcom G7"
    case dexcomONEPlus = "Dexcom ONE+"
    case stelo         = "Stelo"
    case unknown       = "unknown"

    var description: String { rawValue }
}


enum SensorFamily: Int, Comparable, CustomStringConvertible {
    case unknown    = -1
    case libre1     = 0
    case librePro   = 1
    case libre2     = 3
    case libre3     = 4
    case libreX     = 5 // ADC_PRODUCT_TYPE_GLUCOSE_KETONE (com.adc.dcm.gksensor.GKSDCM)
    case libreSense = 7
    case lingo      = 9 // ADC_PRODUCT_TYPE_GLUCOSE_WELLNESS

    var description: String {
        switch self {
        case .unknown:    "unknown"
        case .libre1:     "Libre 1"
        case .librePro:   "Libre Pro"
        case .libre2:     "Libre 2"
        case .libre3:     "Libre 3"
        case .libreX:     "Libre X"
        case .libreSense: "Libre Sense"
        case .lingo:      "Lingo"
        }
    }
    public static func < (lhs: SensorFamily, rhs: SensorFamily) -> Bool {
        return lhs.rawValue < rhs.rawValue
    }
}


enum SensorRegion: Int, CustomStringConvertible {
    case unknown            = 0
    case european           = 1
    case usa                = 2
    case australianCanadian = 4
    case easternROW         = 8

    var description: String {
        switch self {
        case .unknown:            "unknown"
        case .european:           "European"
        case .usa:                "USA"
        case .australianCanadian: "Australian / Canadian"
        case .easternROW:         "Eastern / Rest of World"
        }
    }
}


enum SensorState: UInt8, CustomStringConvertible {
    case unknown      = 0

    case notActivated = 1
    case warmingUp    = 2
    case active       = 3
    case expired      = 4
    case shutdown     = 5
    case failure      = 6

    var description: String {
        switch self {
        case .notActivated: "Not activated"
        case .warmingUp:    "Warming up"
        case .active:       "Active"
        case .expired:      "Expired"
        case .shutdown:     "Shut down"
        case .failure:      "Failure"
        default:            "unknown"
        }
    }
}


@Observable public class Sensor: Logging {

    var type: SensorType = .unknown
    var family: SensorFamily = .unknown
    var region: SensorRegion = .unknown
    var serial: String = ""
    var readerSerial: Data = Data()
    var firmware: String = ""
    var generation: Int = 0
    var securityGeneration: Int = 0

    var transmitter: Transmitter?
    var main: MainDelegate!

    var state: SensorState = .unknown
    var lastReadingDate = Date.distantPast
    var activationTime: UInt32 = 0
    var age: Int = 0
    var maxLife: Int = 0
    var initializations: Int = 0

    var uid: SensorUid = Data() {
        willSet(uid) {
            if type != .libre3 && type != .lingo && type != .libreSelect && type != .instinct && type != .libreX {
                serial = serialNumber(uid: uid, family: self.family)
            }
        }
    }

    var trend: [Glucose] = []
    var history: [Glucose] = []

    var calibrationInfo = CalibrationInfo()

    var factoryTrend: [Glucose] { trend.map { factoryGlucose(rawGlucose: $0, calibrationInfo: calibrationInfo) }}
    var factoryHistory: [Glucose] { history.map { factoryGlucose(rawGlucose: $0, calibrationInfo: calibrationInfo) }}


    init(transmitter: Transmitter? = nil, main: MainDelegate? = nil) {
        self.transmitter = transmitter
        if transmitter != nil {
            self.main = transmitter!.main
        } else {
            self.main = main
        }
    }

    var typeAndName: String { "\(type) \(transmitter!.peripheral!.name ?? "(unnamed)")" }

#if !os(watchOS)
    func execute(nfc: NFC, taskRequest: TaskRequest) async throws {
    }
#endif

}
