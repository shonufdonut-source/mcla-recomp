import Foundation

/// Runs the C++ device probe (ReXGlue SDK + MoltenVK, no game code) on its own thread.
enum DeviceProbe {
    private static var running = false

    static func run() {
        guard !running else { return }
        running = true
        log("device probe: starting; if the app closes, reopen it and share mcla-previous.log")
        let thread = Thread {
            let temp = NSTemporaryDirectory()
            mcla_probe_run(temp) { line in
                Log.shared.writeSync(line.map { String(cString: $0) } ?? "")
            }
            DispatchQueue.main.async { running = false }
        }
        thread.stackSize = 8 << 20
        thread.name = "mcla.probe"
        thread.start()
    }
}
