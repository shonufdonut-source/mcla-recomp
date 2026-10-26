import Foundation

/// In-app log: kept in memory for the viewer and mirrored to Documents/mcla.log,
/// which the Files app can see (UIFileSharingEnabled) and the app can share.
/// The previous run's log is kept as mcla-previous.log, so a crash is not lost on relaunch.
final class Log {
    static let shared = Log()
    static let didAppend = Notification.Name("MCLALogDidAppend")

    private let queue = DispatchQueue(label: "mcla.log")
    private(set) var lines: [String] = []
    let fileURL: URL
    let previousURL: URL
    private let formatter: DateFormatter

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        fileURL = docs.appendingPathComponent("mcla.log")
        previousURL = docs.appendingPathComponent("mcla-previous.log")
        formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        let fm = FileManager.default
        if fm.fileExists(atPath: fileURL.path) {
            try? fm.removeItem(at: previousURL)
            try? fm.moveItem(at: fileURL, to: previousURL)
        }
        try? "".write(to: fileURL, atomically: true, encoding: .utf8)
    }

    func write(_ message: String) {
        queue.async { self.append(message) }
    }

    /// Returns only after the line is on disk. Used by the device probe, which may crash.
    func writeSync(_ message: String) {
        queue.sync { self.append(message) }
    }

    private func append(_ message: String) {
        let line = "\(formatter.string(from: Date())) \(message)"
        lines.append(line)
        if let data = (line + "\n").data(using: .utf8),
           let handle = try? FileHandle(forWritingTo: fileURL) {
            handle.seekToEndOfFile()
            handle.write(data)
            try? handle.synchronize()
            try? handle.close()
        }
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Log.didAppend, object: line)
        }
    }

    var text: String { queue.sync { lines.joined(separator: "\n") } }
}

func log(_ message: String) { Log.shared.write(message) }
