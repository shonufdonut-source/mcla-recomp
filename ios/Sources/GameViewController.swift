import QuartzCore
import UIKit

/// Where default.xex is: On My iPad > MCLA > game, then On My iPad > MCLA itself, then the folder
/// picked with Import game folder. The game folder is only read; nothing in it is changed.
enum GameFolder {
    static let bookmarkKey = "gameFolderBookmark"
    private static var scopedURL: URL?

    static func locate() -> URL? {
        let fm = FileManager.default
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        // The iPad's file system is case-sensitive; accept "Game" or "GAME" for the folder too.
        let named = ((try? fm.contentsOfDirectory(atPath: docs.path)) ?? [])
            .filter { $0.lowercased() == "game" }
            .map { docs.appendingPathComponent($0, isDirectory: true) }
        for candidate in named + [docs] {
            if fm.fileExists(atPath: candidate.appendingPathComponent("default.xex").path) { return candidate }
        }
        guard let data = UserDefaults.standard.data(forKey: bookmarkKey) else { return nil }
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil,
                                 bookmarkDataIsStale: &stale) else { return nil }
        // The runtime reads the folder from its own threads for as long as the app runs.
        if scopedURL != url, url.startAccessingSecurityScopedResource() { scopedURL = url }
        return fm.fileExists(atPath: url.appendingPathComponent("default.xex").path) ? url : nil
    }

    /// Writable, not shown in the Files app: user data (saves), shader cache, settings.
    static func dataFolder() -> URL {
        let fm = FileManager.default
        let url = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("mcla", isDirectory: true)
        try? fm.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
}

/// Full-screen game host. Runs the recompiled game up to the chosen stage (see mcla_probe.h).
final class GameViewController: UIViewController {
    private let metalView = MetalLayerView()
    private let stage: Int32
    private var displayLink: CADisplayLink?
    private var started = false
    /// Pausing waits for the GPU command processor, so it runs here, in order, off the main thread.
    private let lifecycleQueue = DispatchQueue(label: "mcla.game.lifecycle")

    init(stage: Int32) {
        self.stage = stage
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func loadView() { view = metalView }

    // The game has the whole screen: no clock or battery over it, the home bar fades.
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        let share = UIButton(type: .system)
        share.setTitle("Share log", for: .normal)
        share.backgroundColor = UIColor.white.withAlphaComponent(0.8)
        share.layer.cornerRadius = 8
        share.contentEdgeInsets = UIEdgeInsets(top: 8, left: 14, bottom: 8, right: 14)
        share.addTarget(self, action: #selector(shareLog(_:)), for: .touchUpInside)
        share.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(share)
        NSLayoutConstraint.activate([
            share.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 12),
            share.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -12),
        ])
        // Faint after a few seconds so it does not sit over the game; it still takes taps.
        UIView.animate(withDuration: 1, delay: 5, options: [.allowUserInteraction]) { share.alpha = 0.2 }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard !started else { return }
        started = true
        view.layoutIfNeeded()
        guard let game = GameFolder.locate() else {
            log("game: no default.xex found (On My iPad > MCLA > game, On My iPad > MCLA, or the imported folder)")
            return
        }
        let data = GameFolder.dataFolder()
        let size = metalView.metalLayer.drawableSize
        log("game: stage \(stage), game folder \(game.path), data folder \(data.path), layer \(Int(size.width))x\(Int(size.height)) px")
        let layerPtr = UnsafeRawPointer(Unmanaged.passUnretained(metalView.metalLayer).toOpaque())
        let result = mcla_game_start(layerPtr, UInt32(size.width), UInt32(size.height),
                                     game.path, data.path, stage) { line in
            Log.shared.writeSync(line.map { String(cString: $0) } ?? "")
        }
        log("game: start returned \(result); tap Share log")
        metalView.onDrawableSizeChange = { w, h in mcla_game_resize(w, h) }
        displayLink = makeDisplayLink(target: self, selector: #selector(tick))
        NotificationCenter.default.addObserver(self, selector: #selector(thermalChanged),
                                               name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
        thermalChanged()
        NotificationCenter.default.addObserver(self, selector: #selector(memoryWarning),
                                               name: UIApplication.didReceiveMemoryWarningNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(willResignActive),
                                               name: UIApplication.willResignActiveNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(didBecomeActive),
                                               name: UIApplication.didBecomeActiveNotification, object: nil)
    }

    @objc private func tick() { mcla_game_tick() }

    /// iOS refuses GPU work from an app that is off screen and the game loses its GPU device,
    /// so graphics stop before the app goes; the background task asks iOS for time to finish.
    @objc private func willResignActive() {
        var task = UIBackgroundTaskIdentifier.invalid
        let finish = {
            guard task != .invalid else { return }
            UIApplication.shared.endBackgroundTask(task)
            task = .invalid
        }
        task = UIApplication.shared.beginBackgroundTask(withName: "Pause the game") { finish() }
        lifecycleQueue.async {
            mcla_game_set_paused(1)
            DispatchQueue.main.async { finish() }
        }
    }

    @objc private func didBecomeActive() {
        lifecycleQueue.async { mcla_game_set_paused(0) }
    }

    @objc private func memoryWarning() {
        log("iOS memory warning: the app is close to its memory limit")
    }

    /// A hot iPad slows itself down, so heat changes go in the log next to the frame rate.
    @objc private func thermalChanged() {
        let names = ["nominal", "fair", "serious", "critical"]
        let state = ProcessInfo.processInfo.thermalState.rawValue
        log("thermal state: \(state) (\(state < names.count ? names[state] : "unknown"))")
    }

    @objc private func shareLog(_ sender: UIButton) {
        var items: [Any] = [Log.shared.fileURL]
        if FileManager.default.fileExists(atPath: Log.shared.previousURL.path) {
            items.append(Log.shared.previousURL)
        }
        let vc = UIActivityViewController(activityItems: items, applicationActivities: nil)
        vc.popoverPresentationController?.sourceView = sender
        present(vc, animated: true)
    }
}
