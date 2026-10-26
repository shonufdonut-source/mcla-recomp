import UIKit
import UniformTypeIdentifiers

/// M1 screen: log viewer, game-folder import, share log.
/// No game code or data is bundled; the folder is picked by the user in the Files app.
final class MainViewController: UIViewController, UIDocumentPickerDelegate {
    private let textView = UITextView()
    private static let bookmarkKey = "gameFolderBookmark"

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "MCLA (port M1b probe)"
        view.backgroundColor = .systemBackground

        textView.isEditable = false
        textView.font = UIFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        textView.text = Log.shared.text
        textView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(textView)
        NSLayoutConstraint.activate([
            textView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            textView.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),
            textView.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            textView.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
        ])

        navigationItem.leftBarButtonItem = UIBarButtonItem(
            title: "Import game folder", style: .plain, target: self, action: #selector(importFolder))
        navigationItem.rightBarButtonItems = [
            UIBarButtonItem(title: "Share log", style: .plain, target: self, action: #selector(shareLog)),
            UIBarButtonItem(title: "Run probe", style: .plain, target: self, action: #selector(runProbe)),
        ]

        NotificationCenter.default.addObserver(
            self, selector: #selector(logAppended(_:)), name: Log.didAppend, object: nil)
        textView.text = Log.shared.text
        scanAppDocuments()
        checkSavedFolder()
    }

    /// Files placed in On My iPad > MCLA (this app's Documents folder) need no picker.
    private func scanAppDocuments() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        DispatchQueue.global(qos: .utility).async {
            let fm = FileManager.default
            let names = ((try? fm.contentsOfDirectory(atPath: docs.path)) ?? []).filter { $0 != "mcla.log" }
            var bytes: Int64 = 0
            var files = 0
            if let e = fm.enumerator(at: docs, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
                for case let f as URL in e where f.lastPathComponent != "mcla.log" {
                    let v = try? f.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                    if v?.isRegularFile == true { files += 1; bytes += Int64(v?.fileSize ?? 0) }
                }
            }
            log("app Documents folder (On My iPad > MCLA): \(files) files, \(bytes / 1_048_576) MB, default.xex present: \(names.contains("default.xex"))")
        }
    }

    @objc private func logAppended(_ note: Notification) {
        // Rebuild from the log itself so a line is never shown twice.
        textView.text = Log.shared.text
        let end = NSRange(location: textView.text.utf16.count, length: 0)
        textView.scrollRangeToVisible(end)
    }

    @objc private func runProbe() {
        DeviceProbe.run()
    }

    @objc private func shareLog() {
        var items: [Any] = [Log.shared.fileURL]
        if FileManager.default.fileExists(atPath: Log.shared.previousURL.path) {
            items.append(Log.shared.previousURL)
        }
        let vc = UIActivityViewController(activityItems: items, applicationActivities: nil)
        vc.popoverPresentationController?.barButtonItem = navigationItem.rightBarButtonItems?.first
        present(vc, animated: true)
    }

    @objc private func importFolder() {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder])
        picker.delegate = self
        picker.allowsMultipleSelection = false
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let url = urls.first else { return }
        log("picked folder: \(url.lastPathComponent)")
        do {
            let bookmark = try url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
            UserDefaults.standard.set(bookmark, forKey: Self.bookmarkKey)
            log("saved folder bookmark (\(bookmark.count) bytes)")
        } catch {
            log("could not save bookmark: \(error.localizedDescription)")
        }
        scan(url)
    }

    private func checkSavedFolder() {
        guard let data = UserDefaults.standard.data(forKey: Self.bookmarkKey) else {
            log("no game folder imported yet; tap Import game folder")
            return
        }
        var stale = false
        if let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) {
            log("saved game folder: \(url.lastPathComponent)\(stale ? " (bookmark stale, re-import)" : "")")
            scan(url)
        } else {
            log("saved bookmark could not be resolved; re-import the folder")
        }
    }

    /// Counts files and bytes and looks for the expected top-level entries. Read-only.
    private func scan(_ url: URL) {
        DispatchQueue.global(qos: .utility).async {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let fm = FileManager.default
            let top = (try? fm.contentsOfDirectory(atPath: url.path)) ?? []
            log("folder top level: \(top.sorted().prefix(30).joined(separator: ", "))\(top.count > 30 ? ", ..." : "")")
            var files = 0
            var bytes: Int64 = 0
            if let e = fm.enumerator(at: url, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
                for case let f as URL in e {
                    let v = try? f.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                    if v?.isRegularFile == true {
                        files += 1
                        bytes += Int64(v?.fileSize ?? 0)
                    }
                }
            }
            log("folder contents: \(files) files, \(bytes / 1_048_576) MB")
            log("default.xex present at top level: \(top.contains("default.xex"))")
            log("M1 stops here: the recompiled game is not linked into this build yet")
        }
    }
}
