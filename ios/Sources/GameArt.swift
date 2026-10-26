import UIKit

/// The launcher's background, taken from the owner's own copy of the game on the iPad.
///
/// The disc's `nxeart` is an STFS package (magic PIRS) holding the Xbox dashboard art as JPEG
/// files, among them a 1920x1080 background (as ps5/make_title_art.py reads it for the PS5
/// title). No artwork is in the repository or the app; this reads it from the game folder and
/// keeps a copy in the app's private data folder so later launches do not parse it again.
enum GameArt {
    private static let block = 0x1000
    private static let blocksPerHash = 0xAA
    private static let blocksPerLevel1 = 0x70E4

    private static var cacheURL: URL {
        GameFolder.dataFolder().appendingPathComponent("launcher-background.jpg")
    }

    /// Calls back on the main thread with the background, or nil when there is no game folder
    /// or no 1920x1080 image in its nxeart.
    static func loadBackground(_ completion: @escaping (UIImage?) -> Void) {
        let gameFolder = GameFolder.locate()
        DispatchQueue.global(qos: .userInitiated).async {
            let image = cachedBackground() ?? gameFolder.flatMap(extractBackground)
            DispatchQueue.main.async { completion(image) }
        }
    }

    private static func cachedBackground() -> UIImage? {
        guard let data = try? Data(contentsOf: cacheURL) else { return nil }
        return UIImage(data: data)
    }

    private static func extractBackground(from gameFolder: URL) -> UIImage? {
        let url = gameFolder.appendingPathComponent("nxeart")
        guard let package = try? Data(contentsOf: url, options: .mappedIfSafe), package.count > 0x344 else {
            log("launcher art: no nxeart in the game folder")
            return nil
        }
        let magic = String(decoding: package.prefix(4), as: UTF8.self)
        guard ["PIRS", "LIVE", "CON "].contains(magic) else {
            log("launcher art: nxeart is not an STFS package")
            return nil
        }
        let data = dataArea(package)
        var sizes: [String] = []
        var search = data.startIndex
        let marker = Data([0xFF, 0xD8, 0xFF])
        while let range = data.range(of: marker, in: search..<data.endIndex) {
            search = range.upperBound
            // ImageIO stops at the image's end marker, so the rest of the data can follow it.
            guard let image = UIImage(data: data.subdata(in: range.lowerBound..<data.endIndex)),
                  let cg = image.cgImage else { continue }
            sizes.append("\(cg.width)x\(cg.height)")
            if cg.width == 1920 && cg.height == 1080 {
                if let jpeg = image.jpegData(compressionQuality: 0.9) {
                    try? jpeg.write(to: cacheURL, options: .atomic)
                }
                log("launcher art: background read from nxeart")
                return image
            }
        }
        log("launcher art: no 1920x1080 image in nxeart (found \(sizes.joined(separator: ", ")))")
        return nil
    }

    /// The package's data blocks in order, with the interleaved hash blocks left out.
    private static func dataArea(_ package: Data) -> Data {
        let bytes = [UInt8](package)
        let headerSize = Int(bytes[0x340]) << 24 | Int(bytes[0x341]) << 16 | Int(bytes[0x342]) << 8 | Int(bytes[0x343])
        let firstTable = (headerSize + 0xFFF) & 0xFFFF_F000
        let shift = (firstTable >> 12) == 0xB ? 0 : 1

        func offset(_ index: Int) -> Int {
            var adjust = 0
            if index >= blocksPerHash { adjust += ((index / blocksPerHash) + 1) << shift }
            if index > blocksPerLevel1 { adjust += ((index / blocksPerLevel1) + 1) << shift }
            return (shift == 0 ? 0xC000 : 0xD000) + ((index + adjust) << 12)
        }

        var out = Data()
        var index = 0
        while true {
            let start = offset(index)
            if start >= bytes.count { break }
            out.append(contentsOf: bytes[start..<min(start + block, bytes.count)])
            index += 1
        }
        return out
    }
}
