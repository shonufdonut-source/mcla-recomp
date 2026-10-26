import Foundation
import Metal
import MetalFX
import UIKit
import os

/// Facts that decide the port's budgets. Everything here is read from the
/// device at launch and written to the log; nothing is assumed.
enum SystemReport {
    static func run() {
        let info = ProcessInfo.processInfo
        log("MCLA iOS port, \(mcla_game_available() != 0 ? "game build (private)" : "public build, no game code")")
        log("device: \(machine()) iPadOS/iOS \(UIDevice.current.systemVersion)")
        log("physical memory: \(info.physicalMemory / 1_048_576) MB")
        log("os_proc_available_memory: \(os_proc_available_memory() / 1_048_576) MB (this is the per-app budget iOS grants right now)")
        log("thermal state: \(info.thermalState.rawValue), low power mode: \(info.isLowPowerModeEnabled)")

        guard let device = MTLCreateSystemDefaultDevice() else {
            log("Metal: no default device")
            return
        }
        log("Metal device: \(device.name)")
        log("max buffer length: \(device.maxBufferLength / 1_048_576) MB")
        log("recommended max working set: \(device.recommendedMaxWorkingSetSize / 1_048_576) MB")
        log("unified memory: \(device.hasUnifiedMemory)")
        for (name, family) in [("apple4", MTLGPUFamily.apple4), ("apple5", .apple5),
                               ("apple6", .apple6), ("apple7", .apple7)] {
            log("supportsFamily(\(name)): \(device.supportsFamily(family))")
        }
        log("argument buffers tier: \(device.argumentBuffersSupport.rawValue)")
        log("read-write texture tier: \(device.readWriteTextureSupport.rawValue)")
        if #available(iOS 16.4, *) {
            log("BC texture compression: \(device.supportsBCTextureCompression)")
        } else {
            log("BC texture compression: unknown (needs iOS 16.4 API)")
        }
        log("32-bit float filtering: \(device.supports32BitFloatFiltering)")
        log("raster order groups: \(device.areRasterOrderGroupsSupported)")
        log("MetalFX spatial upscaling: \(MTLFXSpatialScalerDescriptor.supportsDevice(device)), temporal: \(MTLFXTemporalScalerDescriptor.supportsDevice(device))")
    }

    private static func machine() -> String {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: size)
        sysctlbyname("hw.machine", &buf, &size, nil, 0)
        return String(cString: buf)
    }
}
