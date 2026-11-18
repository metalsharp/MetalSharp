// MetalSharp's Liquid Glass (macOS 26 and later): AppKit's own glass under GPUI's view.
//
// GPUI draws the whole app into one Metal layer, which cannot refract what lies behind it. So under that layer, in
// one container per window, MetalSharp keeps from bottom to top:
//   - the desktop, blurred through the window (a behind-window NSVisualEffectView: glass views refract what is in
//     the window, not what is behind it);
//   - the selected game's art, faded into that glass at its edges;
//   - a glass view for each surface GPUI marks (a panel, a control, the primary button, the selected card), each its
//     own kind of glass so they contrast.
// GPUI then draws text, icons and cards over them with clear backgrounds. Each frame GPUI begins, marks the surfaces
// it paints with their window rectangles (logical points, top-left origin) and ends; a surface it did not paint that
// frame goes, so a closed panel leaves no glass behind.
import AppKit
import QuartzCore

/// A view whose origin is its top-left, as GPUI's coordinates are.
private final class FlippedView: NSView {
    override var isFlipped: Bool { true }
    // GPUI's view above takes every event.
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

/// The hero's art: as wide as the window while it is resized (GPUI sets its frame a frame later), its fade kept to
/// its bounds.
private final class HeroView: NSView {
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer?.mask?.frame = bounds
        CATransaction.commit()
    }
}

private enum Kind: Int32 {
    case panel = 0
    case control = 1
    case primary = 2
    case selected = 3
    /// a panel over the art (settings): deeper, so its text reads over anything
    case sheet = 4
}

@MainActor
private final class Host {
    weak var gpuiView: NSView?
    let container = FlippedView()
    var windowGlass: NSView?
    let hero = HeroView()
    /// evens the desktop behind the window toward the theme, so text reads over a bright or a dark desktop alike
    let veil = FlippedView()
    var heroPath: String?
    var regions: [UInt64: NSView] = [:]
    /// the electric rim and glow of a selected surface, over its glass
    var glows: [UInt64: NSView] = [:]
    var painted = Set<UInt64>()
    var dark = true

    init(gpuiView: NSView) {
        self.gpuiView = gpuiView
        container.wantsLayer = true
        container.autoresizingMask = [.width, .height]
        hero.wantsLayer = true
        hero.layer?.contentsGravity = .resizeAspectFill
        hero.layer?.masksToBounds = true
        // the art fades out toward its bottom and sides, into the window's glass
        let fade = CAGradientLayer()
        fade.colors = [NSColor.black.cgColor, NSColor.black.cgColor, NSColor.clear.cgColor]
        fade.locations = [0.0, 0.55, 1.0]
        fade.startPoint = CGPoint(x: 0.5, y: 0.0)
        fade.endPoint = CGPoint(x: 0.5, y: 1.0)
        hero.layer?.mask = fade
        hero.alphaValue = 0
        hero.autoresizingMask = [.width]
        veil.wantsLayer = true
        veil.autoresizingMask = [.width, .height]
        container.addSubview(veil)
        container.addSubview(hero)
    }

    func install() {
        guard let gpuiView, let parent = gpuiView.superview else { return }
        container.frame = gpuiView.frame
        if container.superview !== parent {
            container.removeFromSuperview()
            parent.addSubview(container, positioned: .below, relativeTo: gpuiView)
        }
        if windowGlass == nil {
            let base = NSVisualEffectView()
            base.blendingMode = .behindWindow
            base.material = dark ? .hudWindow : .popover
            base.state = .active
            base.frame = container.bounds
            base.autoresizingMask = [.width, .height]
            container.addSubview(base, positioned: .below, relativeTo: veil)
            windowGlass = base
            veil.frame = container.bounds
            veil.layer?.backgroundColor =
                (dark ? NSColor(calibratedRed: 0.03, green: 0.05, blue: 0.09, alpha: 0.50) : NSColor(white: 1.0, alpha: 0.35)).cgColor
        }
    }

    func remove() {
        container.removeFromSuperview()
    }

    func makeGlass(kind: Kind, radius: CGFloat, tint: NSColor?) -> NSView {
        if #available(macOS 26.0, *) {
            let glass = NSGlassEffectView()
            glass.style = kind == .selected ? .clear : .regular
            glass.cornerRadius = radius
            glass.tintColor = tint
            return glass
        }
        let effect = NSVisualEffectView()
        effect.material = .hudWindow
        effect.blendingMode = .withinWindow
        effect.state = .active
        effect.wantsLayer = true
        effect.layer?.cornerRadius = radius
        effect.layer?.masksToBounds = true
        return effect
    }

    /// Each kind its own strength, so surfaces contrast: panels the quietest, then controls, then the primary action.
    func tint(for kind: Kind) -> NSColor? {
        switch kind {
        case .panel: return NSColor(white: 1.0, alpha: dark ? 0.07 : 0.30)
        case .control: return NSColor(white: 1.0, alpha: dark ? 0.24 : 0.45)
        case .primary: return NSColor(white: 1.0, alpha: 0.80)
        case .selected: return NSColor(calibratedRed: 0.30, green: 0.66, blue: 1.0, alpha: 0.50)
        case .sheet: return dark ? NSColor(calibratedRed: 0.03, green: 0.04, blue: 0.07, alpha: 0.55) : NSColor(white: 1.0, alpha: 0.55)
        }
    }

    func place(_ view: NSView, rect: NSRect, rotation: CGFloat) {
        // rotation is about the center and leaves the frame's meaning: set it straight, then turn it
        if view.frameCenterRotation != 0 {
            view.frameCenterRotation = 0
        }
        if view.frame != rect {
            view.frame = rect
        }
        if rotation != 0 {
            view.frameCenterRotation = rotation
        }
    }

    func region(id: UInt64, rect: NSRect, radius requested: CGFloat, kind: Kind, rotation: CGFloat) {
        // a radius past half the shorter side is a capsule, as GPUI draws it
        let radius = min(requested, min(rect.width, rect.height) / 2)
        painted.insert(id)
        let view: NSView
        if let existing = regions[id] {
            view = existing
        } else {
            view = makeGlass(kind: kind, radius: radius, tint: tint(for: kind))
            container.addSubview(view)
            regions[id] = view
            if kind == .selected {
                let glow = FlippedView()
                glow.wantsLayer = true
                let electric = NSColor(calibratedRed: 0.36, green: 0.78, blue: 1.0, alpha: 1.0)
                glow.layer?.borderColor = electric.cgColor
                glow.layer?.borderWidth = 2.5
                glow.layer?.shadowColor = electric.cgColor
                glow.layer?.shadowOpacity = 1.0
                glow.layer?.shadowRadius = 26
                glow.layer?.shadowOffset = .zero
                glow.layer?.masksToBounds = false
                container.addSubview(glow)
                glows[id] = glow
            }
        }
        place(view, rect: rect, rotation: rotation)
        if #available(macOS 26.0, *), let glass = view as? NSGlassEffectView, glass.cornerRadius != radius {
            glass.cornerRadius = radius
        }
        if let glow = glows[id] {
            place(glow, rect: rect, rotation: rotation)
            glow.layer?.cornerRadius = radius
        }
    }

    func setHero(path: String?, rect: NSRect) {
        if hero.frame != rect {
            hero.frame = rect
        }
        guard path != heroPath else { return }
        heroPath = path
        let image = path.flatMap { NSImage(contentsOfFile: $0) }
        let fade = CATransition()
        fade.type = .fade
        fade.duration = 0.32
        fade.timingFunction = CAMediaTimingFunction(controlPoints: 0.22, 1.0, 0.36, 1.0)
        hero.layer?.add(fade, forKey: "contents")
        hero.layer?.contents = image?.cgImage(forProposedRect: nil, context: nil, hints: nil)
        hero.alphaValue = image == nil ? 0 : 1
    }

    func begin() {
        install()
        painted.removeAll(keepingCapacity: true)
    }

    func end() {
        for (id, view) in regions where !painted.contains(id) {
            view.removeFromSuperview()
            regions[id] = nil
            glows.removeValue(forKey: id)?.removeFromSuperview()
        }
        // GPUI marks no hero when no game's art is on screen
        if !painted.contains(0) {
            hero.alphaValue = 0
            heroPath = nil
        }
    }
}

@MainActor private var hosts: [ObjectIdentifier: Host] = [:]

@MainActor private func host(_ pointer: UnsafeMutableRawPointer) -> Host? {
    let view = Unmanaged<NSView>.fromOpaque(pointer).takeUnretainedValue()
    if let host = hosts[ObjectIdentifier(view)] { return host }
    let host = Host(gpuiView: view)
    hosts[ObjectIdentifier(view)] = host
    return host
}

@_cdecl("ms_glass_begin")
public func ms_glass_begin(_ view: UnsafeMutableRawPointer, _ dark: Bool) {
    MainActor.assumeIsolated {
        guard let host = host(view) else { return }
        if host.dark != dark, let glass = host.windowGlass {
            glass.removeFromSuperview()
            host.windowGlass = nil
        }
        host.dark = dark
        host.begin()
    }
}

@_cdecl("ms_glass_region")
public func ms_glass_region(
    _ view: UnsafeMutableRawPointer, _ id: UInt64, _ x: Double, _ y: Double, _ width: Double, _ height: Double,
    _ radius: Double, _ kind: Int32, _ rotation: Double
) {
    MainActor.assumeIsolated {
        host(view)?.region(
            id: id, rect: NSRect(x: x, y: y, width: width, height: height), radius: radius,
            kind: Kind(rawValue: kind) ?? .panel, rotation: rotation)
    }
}

@_cdecl("ms_glass_hero")
public func ms_glass_hero(
    _ view: UnsafeMutableRawPointer, _ path: UnsafePointer<CChar>?, _ x: Double, _ y: Double, _ width: Double,
    _ height: Double
) {
    MainActor.assumeIsolated {
        guard let host = host(view) else { return }
        host.painted.insert(0)
        host.setHero(path: path.map { String(cString: $0) }, rect: NSRect(x: x, y: y, width: width, height: height))
    }
}

@_cdecl("ms_glass_end")
public func ms_glass_end(_ view: UnsafeMutableRawPointer) {
    MainActor.assumeIsolated {
        host(view)?.end()
    }
}

/// Liquid Glass turned off: the container goes, and the window is GPUI's alone again.
@_cdecl("ms_glass_detach")
public func ms_glass_detach(_ view: UnsafeMutableRawPointer) {
    MainActor.assumeIsolated {
        let gpuiView = Unmanaged<NSView>.fromOpaque(view).takeUnretainedValue()
        hosts.removeValue(forKey: ObjectIdentifier(gpuiView))?.remove()
    }
}
