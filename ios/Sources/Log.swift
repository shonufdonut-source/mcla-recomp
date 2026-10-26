import Foundation

/// In-app log: kept in memory for the viewer and mirrored to Documents/mcla.log,
/// which the Files app can see (UIFileSharingEnabled) and the app can share.
final class Log {
    static let shared = Log()
    static let didAppend = Notification.Name("MCLALogDidAppend")

    private let queue = DispatchQueue(label: "mcla.log")
    private(set) var lines: [String] = []
    let fileURL: URL
    private let formatter: DateFormatter

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        fileURL = docs.appendingPathComponent("mcla.log")
        formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        try? "".write(to: fileURL, atomically: true, encoding: .utf8)
    }

    func write(_ message: String) {
        queue.async {
            let line = "\(self.formatter.string(from: Date())) \(message)"
            self.lines.append(line)
            if let data = (line + "\n").data(using: .utf8),
               let handle = try? FileHandle(forWritingTo: self.fileURL) {
                handle.seekToEndOfFile()
                handle.write(data)
                try? handle.close()
            }
            DispatchQueue.main.async {
                NotificationCenter.default.post(name: Log.didAppend, object: line)
            }
        }
    }

    var text: String { queue.sync { lines.joined(separator: "\n") } }
}

func log(_ message: String) { Log.shared.write(message) }
