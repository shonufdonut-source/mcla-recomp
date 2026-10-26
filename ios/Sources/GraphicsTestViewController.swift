import QuartzCore
import UIKit

/// View whose backing layer is the CAMetalLayer the SDK's Vulkan presenter draws into.
final class MetalLayerView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
    var onDrawableSizeChange: ((UInt32, UInt32) -> Void)?

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.screen.nativeScale ?? UIScreen.main.nativeScale
        contentScaleFactor = scale
        let size = CGSize(width: bounds.width * scale, height: bounds.height * scale)
        if metalLayer.drawableSize != size, size.width > 0, size.height > 0 {
            metalLayer.drawableSize = size
            onDrawableSizeChange?(UInt32(size.width), UInt32(size.height))
        }
    }
}

/// Asks for the display's top rate: 120 Hz on ProMotion iPads (the iPad Pro 2018 has it), 60 elsewhere.
/// iOS may still lower it (Low Power Mode, heat); the frame-rate log shows what was granted.
func makeDisplayLink(target: Any, selector: Selector) -> CADisplayLink {
    let link = CADisplayLink(target: target, selector: selector)
    let top = Float(UIScreen.main.maximumFramesPerSecond)
    link.preferredFrameRateRange = CAFrameRateRange(minimum: min(60, top), maximum: top, preferred: top)
    link.add(to: .main, forMode: .common)
    log("display: up to \(Int(top)) Hz, requested \(Int(top))")
    return link
}

/// Full-screen graphics test: SDK presenter + immediate drawer through MoltenVK (no game code).
final class GraphicsTestViewController: UIViewController {
    private let metalView = MetalLayerView()
    private var displayLink: CADisplayLink?
    private var started = false

    override func loadView() { view = metalView }

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
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard !started else { return }
        started = true
        view.layoutIfNeeded()
        let size = metalView.metalLayer.drawableSize
        log("graphics test: layer \(Int(size.width))x\(Int(size.height)) px")
        let layerPtr = UnsafeRawPointer(Unmanaged.passUnretained(metalView.metalLayer).toOpaque())
        let result = mcla_gfx_start(layerPtr, UInt32(size.width), UInt32(size.height)) { line in
            Log.shared.writeSync(line.map { String(cString: $0) } ?? "")
        }
        guard result == 0 else {
            log("graphics test: start failed (\(result)); tap Share log")
            return
        }
        metalView.onDrawableSizeChange = { w, h in mcla_gfx_resize(w, h) }
        displayLink = makeDisplayLink(target: self, selector: #selector(tick))
    }

    @objc private func tick() { mcla_gfx_tick() }

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
