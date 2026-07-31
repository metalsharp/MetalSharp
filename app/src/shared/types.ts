export interface Game {
  id: string;
  name: string;
  exePath: string;
  icon?: string;
  platform: "steam" | "local" | "epic" | "gog";
  steamAppId?: number;
  sizeBytes?: number;
  lastPlayed?: string;
  coverArt?: string;
  metalsharpCompatible: boolean;
}

export interface SteamStatus {
  installed: boolean;
  path?: string;
  loginState?: LoginState;
  macInstalled?: boolean;
  macRunning?: boolean;
  macPath?: string;
  macInstallUrl?: string;
  running: boolean;
}

export interface LoginState {
  state: "logged_in" | "logged_out" | "unknown";
  account?: { name: string; remembered: boolean }[] | null;
}

export interface LaunchOptions {
  fullscreen: boolean;
  debugMetal: boolean;
  verbose: boolean;
  prefix?: string;
  customArgs: string[];
  launchMode: "native" | "wine";
}

export interface AppConfig {
  ok: boolean;
  launchMode: "native" | "wine";
  wineAvailable: boolean;
  nativeAvailable: boolean;
  controllerMode?: "dinput" | "xinput";
  msyncEnabled?: boolean;
}

export interface RustResponse<T = unknown> {
  ok: boolean;
  data?: T;
  error?: string;
}

export interface ScanResult {
  games: Game[];
  steam: SteamStatus;
}
