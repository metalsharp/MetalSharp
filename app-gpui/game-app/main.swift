// The launcher inside a game MetalSharp packed as its own app (/Applications/<Game>.app).
//
// It reads its game from its Info.plist (MetalSharpGameKind, MetalSharpGameId), makes sure
// MetalSharp's backend answers on its socket (opening MetalSharp hidden in the background when it
// does not), asks the backend to launch the game (POST /pack-app/launch: the game's launcher, such
// as Wine Steam, first, then the game on its saved graphics route) and stays in the Dock while the
// game runs. When the game exits, or the app is quit from the Dock or with Cmd-Q, it stops the game
// and its launcher (POST /pack-app/stop) and quits MetalSharp again if it opened it.
//
// Built with Backend.swift by package-local-preview.sh into
// MetalSharp.app/Contents/Resources/tools/metalsharp-game-app; the backend copies it into each packed app.

import Cocoa
import Darwin

let metalSharpBundleID = "com.metalsharp.app"

struct PackedGame {
    let kind: String
    let id: String
    let name: String

    static func fromBundle() -> PackedGame? {
        let info = Bundle.main.infoDictionary ?? [:]
        guard let kind = info["MetalSharpGameKind"] as? String, !kind.isEmpty,
              let id = info["MetalSharpGameId"] as? String, !id.isEmpty
        else { return nil }
        let name = (info["MetalSharpGameName"] as? String)
            ?? (info["CFBundleDisplayName"] as? String)
            ?? (info["CFBundleName"] as? String)
            ?? "Game"
        return PackedGame(kind: kind, id: id, name: name)
    }

    var body: [String: Any] { ["kind": kind, "id": id] }

    var query: String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._")
        let k = kind.addingPercentEncoding(withAllowedCharacters: allowed) ?? kind
        let i = id.addingPercentEncoding(withAllowedCharacters: allowed) ?? id
        return "kind=\(k)&id=\(i)"
    }
}

final class LauncherDelegate: NSObject, NSApplicationDelegate {
    private let game: PackedGame?
    private let backend = Backend.standard()
    private let work = DispatchQueue(label: "com.metalsharp.game-app.work")
    /// Set from the work queue; read on the main thread once the work has stopped or is stopping.
    private var openedMetalSharp = false
    private var launchRequested = false
    private var stopping = false
    private var cleanedUp = false

    init(game: PackedGame?) {
        self.game = game
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        installMenu()
        guard let game = game else {
            fail("This app does not name a MetalSharp game. Pack the game again from MetalSharp.")
            return
        }
        work.async { self.run(game) }
    }

    private func installMenu() {
        let name = game?.name ?? ProcessInfo.processInfo.processName
        let main = NSMenu()
        let appItem = NSMenuItem()
        main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Hide \(name)", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = main
    }

    // MARK: work queue

    private func run(_ game: PackedGame) {
        guard ensureBackend() else {
            fail("MetalSharp did not start. Open MetalSharp, then try \(game.name) again.")
            return
        }
        if isStopping() { return }
        DispatchQueue.main.sync { self.launchRequested = true }
        // A launch can take minutes (Wine Steam starting, a route being staged).
        let launched = backend.request("POST", "/pack-app/launch", body: game.body, timeout: 15 * 60)
        guard let result = launched, result.ok else {
            let reason = launched?.error ?? "MetalSharp did not answer."
            fail("\(game.name) could not be launched: \(reason)")
            return
        }
        watch(game)
    }

    /// The backend answering, MetalSharp opened hidden and in the background if it has to be.
    private func ensureBackend() -> Bool {
        if backend.answering() { return true }
        let alreadyRunning = !NSRunningApplication.runningApplications(withBundleIdentifier: metalSharpBundleID).isEmpty
        if !alreadyRunning {
            guard let url = metalSharpURL() else { return false }
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.activates = false
            configuration.hides = true
            configuration.addsToRecentItems = false
            let opened = DispatchSemaphore(value: 0)
            var openError: Error?
            NSWorkspace.shared.openApplication(at: url, configuration: configuration) { _, error in
                openError = error
                opened.signal()
            }
            _ = opened.wait(timeout: .now() + 30)
            if openError != nil { return false }
            DispatchQueue.main.sync { self.openedMetalSharp = true }
        }
        let deadline = Date().addingTimeInterval(90)
        while Date() < deadline {
            if isStopping() { return false }
            if backend.answering() { return true }
            Thread.sleep(forTimeInterval: 1)
        }
        return false
    }

    private func metalSharpURL() -> URL? {
        let standard = URL(fileURLWithPath: "/Applications/MetalSharp.app")
        if FileManager.default.fileExists(atPath: standard.path) { return standard }
        return NSWorkspace.shared.urlForApplication(withBundleIdentifier: metalSharpBundleID)
    }

    /// Polls the game until it has run and stopped (or never appears), then quits.
    private func watch(_ game: PackedGame) {
        let path = "/pack-app/running?\(game.query)"
        let appearDeadline = Date().addingTimeInterval(5 * 60)
        var seenRunning = false
        var stoppedPolls = 0
        while !isStopping() {
            Thread.sleep(forTimeInterval: 3)
            guard let result = backend.request("GET", path, timeout: 60), result.ok else { continue }
            let running = result.json["running"] as? Bool ?? false
            if running {
                seenRunning = true
                stoppedPolls = 0
            } else if seenRunning {
                // Twice in a row: a game restarting itself (a launcher handing over) is not an exit.
                stoppedPolls += 1
                if stoppedPolls >= 2 { break }
            } else if Date() > appearDeadline {
                break
            }
        }
        DispatchQueue.main.async { NSApp.terminate(nil) }
    }

    private func isStopping() -> Bool {
        DispatchQueue.main.sync { self.stopping }
    }

    private func fail(_ message: String) {
        DispatchQueue.main.async {
            NSApp.activate(ignoringOtherApps: true)
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = self.game?.name ?? "MetalSharp game"
            alert.informativeText = message
            alert.addButton(withTitle: "OK")
            alert.runModal()
            NSApp.terminate(nil)
        }
    }

    // MARK: quitting

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if cleanedUp { return .terminateNow }
        if stopping { return .terminateLater }
        stopping = true
        let game = self.game
        let stopGame = launchRequested
        let quitMetalSharp = openedMetalSharp
        DispatchQueue.global(qos: .userInitiated).async {
            if stopGame, let game = game {
                // Waits behind a launch still in progress: the backend serves one request at a time.
                _ = self.backend.request("POST", "/pack-app/stop", body: game.body, timeout: 5 * 60)
            }
            if quitMetalSharp {
                for app in NSRunningApplication.runningApplications(withBundleIdentifier: metalSharpBundleID) {
                    app.terminate()
                }
            }
            DispatchQueue.main.async {
                self.cleanedUp = true
                NSApp.reply(toApplicationShouldTerminate: true)
            }
        }
        return .terminateLater
    }
}

let app = NSApplication.shared
let delegate = LauncherDelegate(game: PackedGame.fromBundle())
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
