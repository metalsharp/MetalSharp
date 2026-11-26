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
    /// the sidebar: regular glass in the page's color, not the accent's, so the theme's color doesn't wash into it
    case sidebar = 5
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
    /// each region's kind, so a new transparency retints it in place
    var kinds: [UInt64: Kind] = [:]
    /// Settings' Glass Transparency, 0 (tinted) to 1 (clear): thins the veil and the panels' tint, and clears the
    /// panels' blur, so the art and the desktop show through them
    var clarity: CGFloat = 0
    /// the electric rim and glow of a selected surface, over its glass
    var glows: [UInt64: NSView] = [:]
    var painted = Set<UInt64>()
    var dark = true
    /// the theme: its page color (the veil and the deep glass), its accent (the tint of panels and controls) and
    /// the color a selected surface glows
    var page: UInt32 = 0x080a0d
    var accent: UInt32 = 0xe8d6b7
    var glowColor: UInt32 = 0x5cc8ff

    func color(_ rgb: UInt32, alpha: CGFloat, towardWhite mix: CGFloat = 0) -> NSColor {
        let r = CGFloat((rgb >> 16) & 0xff) / 255, g = CGFloat((rgb >> 8) & 0xff) / 255, b = CGFloat(rgb & 0xff) / 255
        return NSColor(
            calibratedRed: r + (1 - r) * mix, green: g + (1 - g) * mix, blue: b + (1 - b) * mix, alpha: alpha)
    }

    /// A new theme: every surface is made again in its colors.
    func retheme(dark: Bool, page: UInt32, accent: UInt32, glow: UInt32) {
        guard dark != self.dark || page != self.page || accent != self.accent || glow != glowColor else { return }
        self.dark = dark
        self.page = page
        self.accent = accent
        glowColor = glow
        windowGlass?.removeFromSuperview()
        windowGlass = nil
        for view in regions.values { view.removeFromSuperview() }
        for view in glows.values { view.removeFromSuperview() }
        regions.removeAll()
        kinds.removeAll()
        glows.removeAll()
        placed.removeAll()
    }

    /// The veil's alpha: what evens the desktop toward the theme, thinned as the glass is made clearer.
    func veilAlpha() -> CGFloat {
        (dark ? 0.52 : 0.40) * (1 - 0.85 * clarity)
    }

    /// A new transparency: the veil and every surface follow in place (a drag of the slider rebuilds nothing).
    func setClarity(_ value: CGFloat) {
        let value = min(max(value, 0), 1)
        guard value != clarity else { return }
        clarity = value
        veil.layer?.backgroundColor = color(page, alpha: veilAlpha()).cgColor
        if #available(macOS 26.0, *) {
            for (id, view) in regions {
                guard let glass = view as? NSGlassEffectView, let kind = kinds[id] else { continue }
                glass.tintColor = tint(for: kind)
                glass.style = style(for: kind)
            }
        }
    }

    /// Panels go from regular glass to clear, lightly blurred glass past the middle of the transparency; the
    /// selected card is always clear, and deep glass (settings) and the sidebar stay regular so their text reads
    /// and no color washes into the sidebar.
    @available(macOS 26.0, *)
    func style(for kind: Kind) -> NSGlassEffectView.Style {
        switch kind {
        case .selected: return .clear
        case .panel, .control: return clarity >= 0.5 ? .clear : .regular
        default: return .regular
        }
    }

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
            veil.layer?.backgroundColor = color(page, alpha: veilAlpha()).cgColor
        }
    }

    func remove() {
        container.removeFromSuperview()
    }

    func makeGlass(kind: Kind, radius: CGFloat, tint: NSColor?) -> NSView {
        if #available(macOS 26.0, *) {
            let glass = NSGlassEffectView()
            glass.style = style(for: kind)
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
    /// Glass Transparency thins the panels' and controls' tint (to a quarter at 100 %) and the deep glass's a little
    /// (its text must still read); the primary action and the selected card keep theirs.
    func tint(for kind: Kind) -> NSColor? {
        let thin = 1 - 0.75 * clarity
        switch kind {
        case .panel: return color(accent, alpha: (dark ? 0.14 : 0.24) * thin, towardWhite: 0.25)
        case .control: return color(accent, alpha: (dark ? 0.42 : 0.50) * thin, towardWhite: 0.20)
        case .primary: return NSColor(white: 1.0, alpha: 0.80)
        case .selected: return color(glowColor, alpha: 0.50)
        case .sheet: return color(page, alpha: (dark ? 0.62 : 0.55) * (1 - 0.3 * clarity))
        // the transparency thins it as it does the panels, to two fifths at 100 %, but its hue stays the page's
        case .sidebar: return color(page, alpha: (dark ? 0.50 : 0.42) * (1 - 0.6 * clarity))
        }
    }

    /// How far a surface from the window's top edge reaches above it.
    static let overTop: CGFloat = 120

    /// Deep glass (panels over the page) stays over every other surface, whichever was made first.
    var sheets = Set<ObjectIdentifier>()
    func addBelowSheets(_ view: NSView) {
        if let sheet = container.subviews.first(where: { sheets.contains(ObjectIdentifier($0)) }) {
            container.addSubview(view, positioned: .below, relativeTo: sheet)
        } else {
            container.addSubview(view)
        }
    }

    /// Where each view was last put. A view is touched only when that changes: moving a turned view every frame
    /// keeps AppKit rebuilding the window's tracking, and the mouse stops reaching GPUI's hover.
    var placed: [ObjectIdentifier: (NSRect, CGFloat)] = [:]

    func place(_ view: NSView, rect: NSRect, rotation: CGFloat) {
        let key = ObjectIdentifier(view)
        if let last = placed[key], last.0 == rect, last.1 == rotation { return }
        placed[key] = (rect, rotation)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        // rotation is about the center and changes what the frame means: set it straight, then turn it
        if view.frameCenterRotation != 0 {
            view.frameCenterRotation = 0
        }
        view.frame = rect
        if rotation != 0 {
            view.frameCenterRotation = rotation
        }
        CATransaction.commit()
    }

    func region(id: UInt64, rect requestedRect: NSRect, radius requested: CGFloat, kind: Kind, rotation: CGFloat) {
        // a radius past half the shorter side is a capsule, as GPUI draws it
        let radius = min(requested, min(requestedRect.width, requestedRect.height) / 2)
        // A surface from the window's top edge (the page's panel) starts above it: glass blends with glass near it,
        // and its top edge, just above the header's controls, ran into each of them.
        var rect = requestedRect
        if rotation == 0, rect.minY <= 0 {
            rect.origin.y -= Host.overTop
            rect.size.height += Host.overTop
        }
        painted.insert(id)
        let view: NSView
        if let existing = regions[id] {
            view = existing
        } else {
            view = makeGlass(kind: kind, radius: radius, tint: tint(for: kind))
            regions[id] = view
            kinds[id] = kind
            if kind == .sheet {
                sheets.insert(ObjectIdentifier(view))
                container.addSubview(view)
            } else {
                addBelowSheets(view)
            }
            if kind == .selected {
                // over the glass: a selected surface's electric rim and glow (the primary action's light is GPUI's,
                // the theme's accent)
                let over = FlippedView()
                over.wantsLayer = true
                do {
                    let electric = color(glowColor, alpha: 1.0, towardWhite: 0.15)
                    over.layer?.borderColor = electric.cgColor
                    over.layer?.borderWidth = 2.5
                    over.layer?.shadowColor = electric.cgColor
                    over.layer?.shadowOpacity = 1.0
                    over.layer?.shadowRadius = 26
                    over.layer?.shadowOffset = .zero
                }
                over.layer?.masksToBounds = false
                addBelowSheets(over)
                glows[id] = over
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
            sheets.remove(ObjectIdentifier(view))
            placed[ObjectIdentifier(view)] = nil
            regions[id] = nil
            kinds[id] = nil
            if let glow = glows.removeValue(forKey: id) {
                placed[ObjectIdentifier(glow)] = nil
                glow.removeFromSuperview()
            }
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
public func ms_glass_begin(
    _ view: UnsafeMutableRawPointer, _ dark: Bool, _ page: UInt32, _ accent: UInt32, _ glow: UInt32,
    _ clarity: Double
) {
    MainActor.assumeIsolated {
        guard let host = host(view) else { return }
        host.retheme(dark: dark, page: page, accent: accent, glow: glow)
        host.setClarity(CGFloat(clarity))
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
