import { type App, watch } from "vue";
import { i18n } from "./i18n";

// Static template copy that predates vue-i18n is translated at the DOM boundary as
// a safety net. Components with dynamic values should use t() directly; this map
// keeps legacy/static panels from leaking English while those components migrate.
const phraseKeys: Record<string, string> = {
  Copy: "ui.logs.copy",
  "Clear View": "ui.logs.clearView",
  Live: "ui.logs.live",
  "Crash Reports": "ui.logs.crashReports",
  "Log Files": "ui.logs.logFiles",
  "Open Logs": "ui.logs.openLogs",
  "Live log stream": "ui.logs.liveStream",
  "Recent log files": "ui.logs.recentFiles",
  "No crash reports found.": "ui.logs.noCrashReports",
  "Sharp App": "ui.sharp.source",
  "Open launch log": "ui.logs.openLogs",
  Stop: "ui.sharp.stop",
  Play: "ui.sharp.play",
  Tools: "ui.sharp.tools",
  "Set Cover": "ui.sharp.addAsset",
  "Add Asset": "ui.sharp.addAsset",
  Uninstall: "ui.sharp.uninstall",
  "Last launch failed": "ui.source.lastLaunchFailed",
  "Launch Doctor": "ui.source.launchDoctor",
  "Checking launch prerequisites...": "ui.source.checkingPrereq",
  Ready: "ui.source.ready",
  Blocked: "ui.source.blocked",
  "Logs and crash reports": "ui.source.logsCrash",
  "Clear All Shader Caches": "ui.settings.clear",
  "Copy Bundle": "ui.source.copyBundle",
  "Recent crash reports": "ui.source.recentCrash",
  "Recent launch log": "ui.source.recentLaunch",
  Auto: "ui.source.auto",
  "Bottle Options": "ui.source.bottleOptions",
  "Graphics Backend": "ui.source.graphicsBackend",
  Mouse: "ui.source.mouse",
  "No Recenter": "ui.source.noRecenter",
  "Mouse Auto": "ui.source.mouseAuto",
  "Stop Download": "ui.source.stopDownload",
  "No bottle required": "ui.source.noBottle",
  Runtime: "ui.source.runtime",
  Firmware: "ui.source.firmware",
  Library: "ui.source.library",
  Host: "ui.source.host",
  Required: "ui.source.required",
  Installed: "ui.source.installed",
  "Not installed": "ui.source.notInstalled",
  Running: "ui.source.running",
  "Runtime & support": "ui.source.runtimeSupport",
  "Game folders": "ui.source.gameFolders",
  "No game folders added yet.": "ui.source.noGameFolders",
  Remove: "ui.source.remove",
  "Open Folder": "ui.source.openFolder",
  Log: "ui.logs.title",
  Compatibility: "ui.source.compatibility",
  "Update dump found": "ui.source.updateDump",
  "Add games": "ui.source.addGames",
  "Scan library": "ui.source.scanLibrary",
  "Install package": "ui.source.installPackage",
  "Add layouts": "ui.source.addLayouts",
  "Official FAQ": "ui.source.officialFaq",
  "Source & GPL License": "ui.source.sourceLicense",
  "Official Releases": "ui.source.officialReleases",
  "Remove Runtime": "ui.source.removeRuntime",
  "Open Environment": "ui.source.openEnvironment",
  "BIOS Dump Guide": "ui.source.biosGuide",
  "Disc Dumping Guide": "ui.source.discGuide",
  "Pin Current": "ui.source.pinCurrent",
  "Pin Current Build": "ui.source.pinCurrent",
  "Unpin Version": "ui.source.unpin",
  "Unpin Build": "ui.source.unpin",
  Rollback: "ui.source.rollback",
  "Rollback Runtime": "ui.source.rollback",
  "Clear Skip": "ui.source.clearSkipped",
  "Clear Skipped Update": "ui.source.clearSkipped",
  Skip: "ui.source.skip",
  "Skip Update": "ui.source.skip",
  "Check PCSX2": "ui.sharp.check",
  "Check RPCS3": "ui.sharp.check",
  "Check shadPS4": "ui.sharp.check",
  "Check SharpEmu": "ui.sharp.check",
  "Checking…": "ui.sharp.checking",
  "Installing…": "ui.sharp.installing",
  "Steam Emu": "ui.game.steamEmu",
  MetalFX: "ui.game.metalFx",
  msync: "ui.game.msync",
  Off: "ui.game.off",
  On: "ui.game.on",
  "Controller input": "ui.game.controllerInput",
  "Full Effects": "ui.settings.fullEffects",
  "Reduced Effects": "ui.settings.reducedEffects",
  "High Resolution (Retina)": "ui.settings.retina",
  "Data Access Check": "ui.settings.dataAccess",
  "Shader Cache": "ui.settings.shaderCache",
  "Pipeline Cache": "ui.settings.pipelineCache",
  Version: "ui.settings.version",
  "You're up to date": "ui.settings.upToDate",
  "Danger Zone": "ui.settings.dangerZone",
  "Game Streaming": "ui.streaming.title",
  "Sunshine Host (this Mac)": "ui.streaming.host",
  "Pair your device": "ui.streaming.pairDevice",
  "Good to know": "ui.streaming.goodToKnow",
  "Start Streaming Host": "ui.streaming.startHost",
  "Stop Streaming": "ui.streaming.stopHost",
  "Pair Device": "ui.streaming.pair",
  "Missing Windows Steam?": "ui.source.missingSteam",
  "Re-run the setup wizard to install or repair the Steam runtime": "ui.source.missingSteamDesc",
  "Run Setup Wizard": "ui.source.setup",
  "Change Folder": "ui.sharp.open",
  "Choose GameJolt Folder": "ui.sharp.open",
  Folder: "ui.sharp.tools",
  Setup: "ui.sharp.tools",
  Login: "ui.sharp.open",
  Connected: "ui.settings.connected",
  "Stop Steam": "ui.settings.stopSteam",
  "Start Steam": "ui.settings.startSteam",
  "Stop Steam Mac": "ui.settings.stopSteamMac",
  "Start Steam Mac": "ui.settings.startSteamMac",
  "Install macOS Steam": "ui.settings.installMacSteam",
  "Not Installed": "ui.source.notInstalled",
  "Opt in to DXMT graphics logs for future launches. Off by default to keep routine launches quiet unless requested.":
    "ui.source.graphicsLogsDesc",
  "Do not close MetalSharp during the update": "ui.settingsDesc.doNotClose",
  "Opt in to DXMT graphics logs for future launches.": "ui.source.graphicsLogsDesc",
  "Off by default to keep routine launches quiet unless requested.": "ui.source.graphicsLogsDesc",
  "The upstream macOS archive is not Developer ID signed or notarized.": "ui.source.runtimeSupport",
  "SharpEmu is early-stage research software. Most games do not run, Windows is upstream's primary target, and macOS support is experimental. MetalSharp is not affiliated with Sony or SharpEmu.":
    "ui.source.sharpemuDescription",
  Controller: "ui.game.controllerInput",
  "Controller 1": "ui.source.controller1",
  "Controller 2": "ui.source.controller2",
  Renderer: "ui.source.renderer",
  "No game locations added yet.": "ui.source.noGameFolders",
  "No applications installed": "sharp.noApps",
  "Install a Windows program to add it to the Sharp Library.": "sharp.noAppsDescription",
  Change: "ui.settings.change",
  "Remove Reference": "ui.source.remove",
  "Apple Silicon · Rosetta": "ui.source.appleSilicon",
  "Intel · SSE4.1": "ui.source.intel",
  "Close application": "ui.streaming.close",
  "Args:": "ui.source.sourceLicense",
  "Selections are saved directly to the isolated PCSX2 configuration.": "ui.source.setup",
  "Loading PCSX2 settings…": "ui.source.setup",
  "Install PCSX2 first": "ui.sharp.install",
  "Disc image or folder": "ui.sharp.open",
  "Refresh metadata": "ui.sharp.refresh",
  "Refresh games and artwork": "ui.sharp.refresh",
  "Choose a library folder": "ui.sharp.open",
  "Add an owned PKG": "ui.sharp.addAsset",
  "Install PS3UPDAT.PUP": "ui.source.installPackage",
  "Open PlayStation support": "ui.source.openEnvironment",
  "Choose dumped CUSA folders": "ui.sharp.open",
  "Console-dumped SPRX files": "ui.sharp.addAsset",
  "Console-dumped font content": "ui.sharp.addAsset",
  "Reference owned eboot.bin folders": "ui.sharp.open",
  "Refresh bounded local metadata": "ui.sharp.refresh",
  "Open sharpemu.app": "ui.source.openEnvironment",
  "View upstream reports": "ui.source.officialFaq",
  "Off by default. When enabled, emulated game code may create host sockets, use DNS, and contact local or internet services. Every network-enabled launch asks again.":
    "ui.source.guestNetworkingDesc",
  "Remove reference": "ui.source.remove",
  Add: "ui.source.addGames",
  "Stable runtime": "ui.source.stableRuntime",
  "Latest verified release": "ui.source.latestVerified",
  "Optional compatibility files": "ui.source.compatibilityFiles",
  "Last exit:": "ui.source.lastExit",
  "Last signal:": "ui.source.lastSignal",
  "Host advisory:": "ui.source.hostAdvisory",
  "No recent log lines loaded.": "ui.source.recentLaunch",
};

const attributeKeys: Record<string, string> = {
  "Open Logs": "ui.logs.openLogs",
  Copy: "ui.logs.copy",
  "Clear View": "ui.logs.clearView",
  "Game settings": "library.gameSettings",
  "Download update": "ui.footer.downloadUpdate",
  "Choose Sharp Library source": "ui.sharp.source",
  "Sharp Library sources": "ui.sharp.source",
};

function textNodes(root: Node): Text[] {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const result: Text[] = [];
  let node: Node | null = walker.nextNode();
  while (node) {
    if (node.nodeType === Node.TEXT_NODE && node.parentElement?.tagName !== "SCRIPT") result.push(node as Text);
    node = walker.nextNode();
  }
  return result;
}

export function installDomLocalization(app: App): void {
  void app;
  const root = document.getElementById("app");
  if (!root) return;

  const originalText = new WeakMap<Text, string>();
  const translatedText = new WeakMap<Text, string>();
  const originalAttributes = new WeakMap<HTMLElement, Map<string, string>>();

  function translateText(node: Text): void {
    const current = node.nodeValue ?? "";
    const priorTranslation = translatedText.get(node);
    if (!originalText.has(node) || (priorTranslation !== undefined && current !== priorTranslation)) {
      originalText.set(node, current);
    }
    const source = originalText.get(node) ?? current;
    const normalizedSource = source.trim().replace(/\s+/g, " ");
    const key = phraseKeys[normalizedSource] ?? phraseKeys[source.trim()];
    if (!key) return;
    const translated = String(i18n.global.t(key));
    const leading = source.match(/^\s*/)?.[0] ?? "";
    const trailing = source.match(/\s*$/)?.[0] ?? "";
    const next = `${leading}${translated}${trailing}`;
    translatedText.set(node, next);
    if (current !== next) node.nodeValue = next;
  }

  function translateAttributes(element: HTMLElement): void {
    const attrs = ["title", "aria-label", "placeholder"];
    let originals = originalAttributes.get(element);
    if (!originals) {
      originals = new Map();
      originalAttributes.set(element, originals);
    }
    for (const attr of attrs) {
      const current = element.getAttribute(attr);
      if (current === null) continue;
      const prior = originals.get(attr);
      if (prior === undefined || current !== String(i18n.global.t(attributeKeys[prior] ?? "")))
        originals.set(attr, current);
      const source = originals.get(attr) ?? current;
      const key = attributeKeys[source];
      if (!key) continue;
      const translated = String(i18n.global.t(key));
      if (current !== translated) element.setAttribute(attr, translated);
    }
  }

  function translateTree(node: Node): void {
    for (const text of textNodes(node)) translateText(text);
    if (node instanceof HTMLElement) {
      translateAttributes(node);
      for (const child of Array.from(node.querySelectorAll<HTMLElement>("[title], [aria-label], [placeholder]"))) {
        translateAttributes(child);
      }
    }
  }

  translateTree(root);
  const observer = new MutationObserver((records) => {
    for (const record of records) {
      if (record.type === "characterData") translateText(record.target as Text);
      for (const node of Array.from(record.addedNodes)) translateTree(node);
    }
  });
  observer.observe(root, { subtree: true, childList: true, characterData: true });

  watch(
    () => i18n.global.locale.value,
    () => {
      for (const node of textNodes(root)) {
        const source = originalText.get(node);
        if (source !== undefined) node.nodeValue = source;
      }
      translateTree(root);
    },
    { flush: "post" },
  );
}
