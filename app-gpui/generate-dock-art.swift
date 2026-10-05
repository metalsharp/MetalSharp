// GPUI 0.2.2 does not transform raster elements. Generate transparent, tilted
// cover variants for the preview's card fan, leaving originals unchanged.
// Run from app-gpui: swift generate-dock-art.swift
import AppKit
import Foundation
// Resolve relative to this script, not the caller's cwd (CI runs at repo root).
let assets = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("assets")
let output = assets.appendingPathComponent("dock")
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
// Each root JPEG that isn't a hero is a sample game's portrait cover.
let covers = try FileManager.default.contentsOfDirectory(at: assets, includingPropertiesForKeys: nil)
    .filter { $0.pathExtension == "jpg" && !$0.lastPathComponent.hasSuffix("-hero.jpg") }
    .sorted { $0.lastPathComponent < $1.lastPathComponent }
guard !covers.isEmpty else {
    throw NSError(domain: "MetalSharpDockArtwork", code: 1,
        userInfo: [NSLocalizedDescriptionKey: "No portrait covers found in \(assets.path)"])
}
for cover in covers {
    let name = cover.deletingPathExtension().lastPathComponent
    let source = NSImage(contentsOf: assets.appendingPathComponent("\(name).jpg"))!
    var rect = CGRect(origin: .zero, size: source.size)
    let image = source.cgImage(forProposedRect: &rect, context: nil, hints: nil)!
    for angle in [-12, -9, -6, -3, 3, 6, 9, 12] {
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 504, pixelsHigh: 630,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        let graphics = NSGraphicsContext(bitmapImageRep: bitmap)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = graphics
        let ctx = graphics.cgContext
        ctx.interpolationQuality = .high
        ctx.translateBy(x: 252, y: 315)
        ctx.rotate(by: -CGFloat(angle) * .pi / 180)
        let card = CGRect(x: -180, y: -243, width: 360, height: 486)
        let outline = CGPath(roundedRect: card, cornerWidth: 12, cornerHeight: 12, transform: nil)
        ctx.addPath(outline)
        ctx.clip()
        ctx.draw(image, in: card)
        ctx.addPath(outline)
        ctx.setStrokeColor(NSColor(calibratedWhite: 0.4, alpha: 0.8).cgColor)
        ctx.setLineWidth(2)
        ctx.strokePath()
        NSGraphicsContext.restoreGraphicsState()
        let data = bitmap.representation(using: .png, properties: [:])!
        try data.write(to: output.appendingPathComponent("\(name)-\(angle).png"))
    }
}
