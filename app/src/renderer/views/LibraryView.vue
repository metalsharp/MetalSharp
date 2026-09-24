<script setup lang="ts">
import { computed, inject, onMounted, ref, watch, type Ref } from "vue";
import { useI18n } from "vue-i18n";
import IconChevronRight from "~icons/lucide/chevron-right";
import IconDownload from "~icons/lucide/download";
import IconLibrary from "~icons/lucide/library";
import IconPlay from "~icons/lucide/play";
import IconSettings from "~icons/lucide/settings";
import IconChevronLeft from "~icons/lucide/chevron-left";
import sharpLogoUrl from "../icon.png";
import { api } from "../composables/useApi";
import { useToast } from "../composables/useToast";
import { useTheme, type ThemeName } from "../composables/useTheme";
import LibraryTopbar from "../components/LibraryTopbar.vue";
import LibraryFooter from "../components/LibraryFooter.vue";
import type { UpdateStatus } from "../api-types";

interface SteamGame {
  appid: number;
  name: string;
  installed: boolean;
  state: "installed" | "not_installed" | "downloading";
  cover_url?: string;
  header_url?: string;
  size_bytes?: number | null;
  launch_method?: string;
  launch_method_name?: string;
  preferred_pipeline?: string | null;
  available_pipelines?: { id: string; name: string; recommended?: boolean }[];
  wine_game_path?: string | null;
  bottle_id?: string | null;
  embedded_icon_path?: string | null;
  last_played_at?: string | null;
  last_played?: string | null;
  playtime_2weeks?: number;
  playtime_forever?: number;
}

interface SteamLibrary {
  ok: boolean;
  total: number;
  installed_count: number;
  games: SteamGame[];
}

interface ShowcaseGame extends SteamGame {
  cover_url: string;
  header_url: string;
  hero_url: string;
  eyebrow: string;
  description: string;
  tags: string[];
  developer: string;
  version: string;
  isLive: boolean;
}

const emit = defineEmits<{ navigate: [view: string] }>();

const library = inject<Ref<SteamLibrary | null>>("library")!;
const wineSteamInstalled = inject<Ref<boolean>>("wineSteamInstalled")!;
const wineSteamRunning = inject<Ref<boolean>>("wineSteamRunning")!;
const backendConnected = inject<Ref<boolean>>("backendConnected")!;
const reloadLibrary = inject<() => Promise<void>>("loadLibrary")!;
const pendingLibraryTab = inject<Ref<string | null>>("pendingLibraryTab");
const updateStatus = inject<Ref<UpdateStatus | null>>("updateStatus")!;
const updateDownloading = inject<Ref<boolean>>("updateDownloading")!;
const toast = useToast();
const { t } = useI18n();
const { theme } = useTheme();
const collectionSaving = ref<Record<number, boolean>>({});

const search = ref("");
const filter = ref<"all" | "installed" | "not_installed">("installed");
const selectedGameId = ref<number | null>(null);
const launchingAppId = ref<number | null>(null);
const carouselOffset = ref(0);
const showFullLibrary = ref(false);
const gameSettingsOpen = ref(false);
const pipelineSaving = ref(false);
const selectedPipeline = ref("auto");
const metalFxMode = ref<"1.75" | "2.0" | "off">("2.0");
const metalFxBusy = ref(false);
const controllerInput = ref<"off" | "x" | "d">("off");
const controllerBusy = ref(false);
const msyncEnabled = ref(true);
const steamEmuActive = ref(false);
const steamEmuBusy = ref(false);
const msyncBusy = ref(false);
const artworkSources = ref<Record<number, string[]>>({});
const heroArtSources = ref<Record<number, string>>({});
const backendBase = ref("");
const artManagerOpening = ref(false);
// Bumped whenever grid artwork changes on disk (Steam Art Manager save) so
// cached /art/grid URLs re-resolve to the freshly written images.
const artVersion = ref(0);
const fallbackArtApps = ref(new Set<number>());

function markFallbackArt(appid: number) {
  if (fallbackArtApps.value.has(appid)) return;
  const next = new Set(fallbackArtApps.value);
  next.add(appid);
  fallbackArtApps.value = next;
}

function isFallbackArtUrl(url: string) {
  return url.includes("storepagebackground") || url.includes("store_item_assets") || /ss_[0-9a-f]+\./.test(url);
}

const themeDockGlow: Record<ThemeName, string> = {
  dark: "rgba(42, 161, 255, 0.78)",
  light: "rgba(255, 255, 255, 0.88)",
  skeleton: "rgba(214, 208, 196, 0.72)",
  forest: "rgba(111, 206, 136, 0.72)",
  "orange-peel": "rgba(255, 122, 26, 0.72)",
  dragonfruit: "rgba(255, 46, 136, 0.72)",
  lava: "rgba(255, 74, 61, 0.75)",
};
const themeControlTokens: Record<ThemeName, { bg: string; text: string; hover: string; border: string }> = {
  dark: { bg: "#080a0d", text: "#ffffff", hover: "#171a1e", border: "rgba(255,255,255,.22)" },
  light: { bg: "#fdfbf7", text: "#1e2732", hover: "#ffffff", border: "rgba(30,39,50,.24)" },
  skeleton: { bg: "#242424", text: "#eeeeee", hover: "#303030", border: "rgba(238,238,238,.24)" },
  forest: { bg: "#182219", text: "#dce8dc", hover: "#1e2b20", border: "rgba(140,190,150,.24)" },
  "orange-peel": { bg: "#231610", text: "#f2e4d8", hover: "#2b1b12", border: "rgba(255,170,120,.24)" },
  dragonfruit: { bg: "#2c182a", text: "#f8e4f0", hover: "#361e33", border: "rgba(255,170,210,.24)" },
  lava: { bg: "#2b0d12", text: "#fff2ee", hover: "#3b1117", border: "rgba(255,110,90,.3)" },
};
const themeAccent: Record<ThemeName, string> = {
  dark: "#e8d6b7",
  light: "#4db8ff",
  skeleton: "#d6d0c4",
  forest: "#6fce88",
  "orange-peel": "#ff9a45",
  dragonfruit: "#ff66aa",
  lava: "#ff6b52",
};
const libraryThemeStyle = computed<Record<string, string>>(() => ({
  "--library-dock-glow": themeDockGlow[theme.value],
  "--library-accent": themeAccent[theme.value],
  "--library-control-bg": themeControlTokens[theme.value].bg,
  "--library-control-text": themeControlTokens[theme.value].text,
  "--library-control-hover": themeControlTokens[theme.value].hover,
  "--library-control-border": themeControlTokens[theme.value].border,
}));

const pipelineOptions = [
  { id: "d3dmetal", label: "D3DMetal" },
  { id: "vkd3d", label: "VKD3D" },
  { id: "d3d9", label: "D3D9" },
  { id: "dxmt", label: "DXMT" },
  { id: "dxmt_32", label: "DXMT(32)" },
  { id: "fna_arm64", label: "Mono/FNA" },
];
const pipelineNames: Record<string, string> = {
  ...Object.fromEntries(pipelineOptions.map((option) => [option.id, option.label])),
  m9: "D3D9",
  dxvk: "D3D9",
  dxvk_32: "D3D9",
};
const pipelineLabel = (id: string | null | undefined) => (id && pipelineNames[id]) || "Auto";
const normalizePipeline = (id: string | null | undefined) =>
  pipelineOptions.some((option) => option.id === id) ? (id as string) : "auto";
const playHistoryKey = "metalsharp-library-play-history";

function readPlayHistory(): Record<string, number> {
  try {
    return JSON.parse(localStorage.getItem(playHistoryKey) || "{}") as Record<string, number>;
  } catch {
    return {};
  }
}

function rememberPlayed(appid: number) {
  const history = readPlayHistory();
  history[String(appid)] = Date.now();
  localStorage.setItem(playHistoryKey, JSON.stringify(history));
}

function steamArt(appid: number, asset: string) {
  return `https://cdn.cloudflare.steamstatic.com/steam/apps/${appid}/${asset}.jpg`;
}

// Unhashed store-page background — served for every app, including new-format
// games whose library_600x900/library_hero/header assets only exist under
// per-asset hashed paths we cannot construct offline.
function storeArt(appid: number) {
  return `https://store.akamai.steamstatic.com/images/storepagebackground/app/${appid}`;
}

// User-authored Steam grid artwork (Steam Art Manager / Steam "Set Custom
// Image") served by the backend from the Wine Steam userdata grid cache.
// When present, these override fetched CDN artwork.
function gridArtUrl(appid: number, kind: "hero" | "poster" | "header") {
  return backendBase.value ? `${backendBase.value}/art/grid/${appid}/${kind}?v=${artVersion.value}` : "";
}

function artworkCandidates(game: ShowcaseGame) {
  const primary = game.cover_url || steamArt(game.appid, "library_600x900_2x");
  const steamDbFallback = `https://steamdb.info/resize/600x900/${primary}`;
  const embedded = game.embedded_icon_path ? `file://${encodeURI(game.embedded_icon_path)}` : "";
  return [
    ...new Set(
      [
        gridArtUrl(game.appid, "poster"),
        gridArtUrl(game.appid, "header"),
        primary,
        steamDbFallback,
        steamArt(game.appid, "library_hero"),
        game.header_url,
        storeArt(game.appid),
        embedded,
        sharpLogoUrl,
      ].filter(Boolean),
    ),
  ];
}

function gameArt(game: ShowcaseGame) {
  return artworkSources.value[game.appid]?.[0] || artworkCandidates(game)[0];
}

type SteamArtEnrichment = { hero?: string; card?: string; shot?: string };
const artworkEnrichmentCache = new Map<number, SteamArtEnrichment | null>();

// Some games (e.g. new releases) only expose artwork under per-asset hashed
// CDN paths that cannot be constructed offline. Steam's store API returns the
// real URLs (the same images steamdb.info renders); the Vite dev proxy serves
// it same-origin since the store API sends no CORS headers.
// Note: background/background_raw is Steam's pre-graded store backdrop, so
// colorful key art (header_image) and screenshots are preferred instead.
async function enrichArtwork(appid: number): Promise<SteamArtEnrichment | null> {
  if (artworkEnrichmentCache.has(appid)) return artworkEnrichmentCache.get(appid) ?? null;
  try {
    const response = await fetch(`/steam-store-api/api/appdetails?appids=${appid}&l=english`);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const payload = (await response.json()) as Record<
      string,
      {
        success?: boolean;
        data?: {
          header_image?: string;
          background_raw?: string;
          background?: string;
          screenshots?: Array<{ path_full?: string }>;
        };
      }
    >;
    const data = payload[appid]?.success ? payload[appid]?.data : undefined;
    const enrichment: SteamArtEnrichment = {
      hero: data?.header_image || data?.background_raw || data?.background,
      card: data?.header_image,
      shot: data?.screenshots?.find((screenshot) => screenshot.path_full)?.path_full,
    };
    const useful = Boolean(enrichment.hero || enrichment.card || enrichment.shot);
    artworkEnrichmentCache.set(appid, useful ? enrichment : null);
    return useful ? enrichment : null;
  } catch {
    artworkEnrichmentCache.set(appid, null);
    return null;
  }
}

const demoGames: ShowcaseGame[] = [
  {
    appid: 1091500,
    name: "Cyberpunk 2077",
    installed: true,
    state: "installed",
    cover_url: steamArt(1091500, "library_600x900_2x"),
    header_url: steamArt(1091500, "header"),
    hero_url: steamArt(1091500, "library_hero"),
    eyebrow: "NIGHT CITY AWAITS",
    description: "Become an urban mercenary in a sprawling city of chrome, danger, and impossible choices.",
    tags: ["Action RPG", "Open World", "CD PROJEKT RED"],
    developer: "CD PROJEKT RED",
    version: "2.1.0",
    isLive: false,
  },
  {
    appid: 1145360,
    name: "Hades",
    installed: true,
    state: "installed",
    cover_url: steamArt(1145360, "library_600x900_2x"),
    header_url: steamArt(1145360, "header"),
    hero_url: steamArt(1145360, "library_hero"),
    eyebrow: "ESCAPE FROM HELL",
    description: "Defy the god of the dead as you battle out of the Underworld in this rogue-like dungeon crawler.",
    tags: ["Action", "Roguelike", "Supergiant Games"],
    developer: "Supergiant Games",
    version: "1.38290",
    isLive: false,
  },
  {
    appid: 1245620,
    name: "ELDEN RING",
    installed: true,
    state: "installed",
    cover_url: steamArt(1245620, "library_600x900_2x"),
    header_url: steamArt(1245620, "header"),
    hero_url: steamArt(1245620, "library_hero"),
    eyebrow: "A WORLD AWAITS",
    description: "Rise, Tarnished. Explore a vast, mysterious world of danger, beauty, and discovery.",
    tags: ["Action RPG", "Open World", "FromSoftware"],
    developer: "FromSoftware",
    version: "1.12.3",
    isLive: false,
  },
  {
    appid: 1332010,
    name: "Stray",
    installed: true,
    state: "installed",
    cover_url: steamArt(1332010, "library_600x900_2x"),
    header_url: steamArt(1332010, "header"),
    hero_url: steamArt(1332010, "library_hero"),
    eyebrow: "LOST, ALONE, AND VULNERABLE",
    description: "Unravel an ancient mystery and find your way home through a forgotten cybercity.",
    tags: ["Adventure", "Atmospheric", "BlueTwelve Studio"],
    developer: "BlueTwelve Studio",
    version: "1.5.0",
    isLive: false,
  },
  {
    appid: 2379780,
    name: "Balatro",
    installed: true,
    state: "installed",
    cover_url: steamArt(2379780, "library_600x900_2x"),
    header_url: steamArt(2379780, "header"),
    hero_url: steamArt(2379780, "library_hero"),
    eyebrow: "ANYTHING GOES",
    description: "The poker roguelike where every hand becomes a new way to break the rules.",
    tags: ["Card Game", "Roguelike", "Playstack"],
    developer: "LocalThunk",
    version: "1.0.1",
    isLive: false,
  },
];

const demoByAppId = new Map(demoGames.map((game) => [game.appid, game]));

const liveGames = computed(() => {
  const history = readPlayHistory();
  return [...(library.value?.games ?? [])]
    .filter((game) => game.installed)
    .sort((a, b) => {
      const aHistory = history[String(a.appid)] || 0;
      const bHistory = history[String(b.appid)] || 0;
      if (aHistory !== bHistory) return bHistory - aHistory;
      const aLast = Date.parse(a.last_played_at || a.last_played || "") || 0;
      const bLast = Date.parse(b.last_played_at || b.last_played || "") || 0;
      if (aLast !== bLast) return bLast - aLast;
      const aPlaytime = (a.playtime_2weeks || 0) * 100 + (a.playtime_forever || 0);
      const bPlaytime = (b.playtime_2weeks || 0) * 100 + (b.playtime_forever || 0);
      return bPlaytime - aPlaytime || a.name.localeCompare(b.name);
    });
});

function showcaseGame(game: SteamGame): ShowcaseGame {
  return {
    ...game,
    cover_url: game.cover_url || steamArt(game.appid, "library_600x900_2x"),
    header_url: game.header_url || steamArt(game.appid, "header"),
    hero_url: steamArt(game.appid, "library_hero"),
    eyebrow: game.launch_method_name?.toUpperCase() || "READY TO PLAY",
    description: "Installed and ready to launch from your MetalSharp library.",
    tags: [game.launch_method_name || "Installed", game.state === "installed" ? "Ready to play" : game.state],
    developer: "MetalSharp library",
    version: "Ready",
    isLive: true,
  };
}

const allGames = computed<ShowcaseGame[]>(() => liveGames.value.map(showcaseGame));

const filteredGames = computed(() => {
  const query = search.value.trim().toLowerCase();
  return allGames.value.filter((game) => {
    const matchesFilter =
      filter.value === "all" ||
      (filter.value === "installed" && game.installed) ||
      (filter.value === "not_installed" && !game.installed);
    const matchesSearch =
      !query || [game.name, game.developer, ...game.tags].some((value) => value.toLowerCase().includes(query));
    return matchesFilter && matchesSearch;
  });
});

const jumpBackGames = computed(() => {
  const games = filteredGames.value;
  return games.slice(carouselOffset.value, carouselOffset.value + 5);
});

const featuredGame = computed<ShowcaseGame | null>(() => {
  return allGames.value.find((game) => game.appid === selectedGameId.value) || allGames.value[0] || null;
});
const heroBleedStyle = computed<Record<string, string>>(() => ({
  "--hero-bleed": featuredGame.value ? `url('${heroArt(featuredGame.value)}')` : "none",
}));

// Hero art is a CSS background so it has no @error fallback — probe candidates
// with Image() and keep the first one that actually loads. User grid artwork
// (Steam Art Manager) is probed first so it always overrides online fetches.
function probeHeroArt(game: ShowcaseGame) {
  // Wait for the backend base URL: probing before it loads would race, cache
  // a CDN candidate, and the guard below would then block the grid-art probe
  // forever. The watch re-fires once backendBase is set.
  if (!game || !backendBase.value || heroArtSources.value[game.appid]) return;
  const candidates = [gridArtUrl(game.appid, "hero"), game.hero_url, game.cover_url, game.header_url].filter(
    Boolean,
  ) as string[];
  const tail = [storeArt(game.appid), sharpLogoUrl];
  const probe = (index: number) => {
    const url = candidates[index];
    if (!url) {
      void enrichArtwork(game.appid).then((extra) => {
        const hero = extra?.hero || extra?.shot || extra?.card;
        if (hero) {
          heroArtSources.value = { ...heroArtSources.value, [game.appid]: hero };
          return;
        }
        probeTail(0);
      });
      return;
    }
    const image = new Image();
    image.onload = () => {
      heroArtSources.value = { ...heroArtSources.value, [game.appid]: url };
    };
    image.onerror = () => probe(index + 1);
    image.src = url;
  };
  const probeTail = (index: number) => {
    const url = tail[index];
    if (!url) return;
    const image = new Image();
    image.onload = () => {
      heroArtSources.value = { ...heroArtSources.value, [game.appid]: url };
    };
    image.onerror = () => probeTail(index + 1);
    image.src = url;
  };
  probe(0);
}

watch(
  [featuredGame, backendBase],
  ([game]) => {
    probeHeroArt(game);
  },
  { immediate: true },
);

async function openArtManager() {
  if (artManagerOpening.value) return;
  artManagerOpening.value = true;
  try {
    const result = await window.metalsharp.openSteamArtManager();
    if (!result?.ok) console.warn("Steam Art Manager could not be launched:", result?.error);
  } finally {
    artManagerOpening.value = false;
  }
}

function heroArt(game: ShowcaseGame) {
  return heroArtSources.value[game.appid] || game.hero_url;
}

const installedCount = computed(() => library.value?.installed_count ?? allGames.value.length);
const canScrollBack = computed(() => carouselOffset.value > 0);
const canScrollForward = computed(() => carouselOffset.value + 5 < filteredGames.value.length);
function selectGame(game: ShowcaseGame) {
  selectedGameId.value = game.appid;
  gameSettingsOpen.value = false;
}

type LibraryTab = "play" | "collection" | "sharp-library" | "logs";
const currentTab = computed<LibraryTab>(() => (showFullLibrary.value ? "collection" : "play"));

function openPlay() {
  showFullLibrary.value = false;
  filter.value = "installed";
}

function openCollection() {
  showFullLibrary.value = true;
  filter.value = "installed";
}

function onTopbarNavigate(tab: LibraryTab) {
  if (tab === "play") openPlay();
  else if (tab === "collection") openCollection();
  else emit("navigate", tab);
}
async function saveCollectionPipeline(game: ShowcaseGame, event: Event) {
  const select = event.target as HTMLSelectElement;
  const pipeline = select.value;
  const previous = game.preferred_pipeline || game.launch_method || "";
  if (!pipeline || pipeline === previous) return;
  collectionSaving.value = { ...collectionSaving.value, [game.appid]: true };
  game.preferred_pipeline = pipeline;
  game.launch_method = pipeline;
  game.launch_method_name = pipelineLabel(pipeline);
  const live = library.value?.games?.find((candidate) => candidate.appid === game.appid);
  if (live) {
    live.preferred_pipeline = pipeline;
    live.launch_method = pipeline;
    live.launch_method_name = pipelineLabel(pipeline);
  }
  const result =
    pipeline === "d3dmetal"
      ? await api<{ ok: boolean; error?: string }>(
          "POST",
          "/d3dmetal/bottles/save",
          {
            appid: game.appid,
            bottleId: game.bottle_id || `steam_${game.appid}`,
            name: game.name,
            gameDir: game.wine_game_path || "",
          },
          10 * 60 * 1000,
        )
      : await api<{ ok: boolean; error?: string }>("POST", "/bottles/edit", {
          id: game.bottle_id || `steam_${game.appid}`,
          name: game.name,
          preferredPipeline: pipeline,
        });
  collectionSaving.value = { ...collectionSaving.value, [game.appid]: false };
  if (result?.ok) {
    toast.show(`${game.name}: ${pipelineLabel(pipeline)} selected`, "success");
  } else {
    toast.show(result?.error || `Failed to update ${game.name}`, "error");
    game.preferred_pipeline = previous || null;
    game.launch_method = previous || game.launch_method;
  }
  await reloadLibrary();
}

function collectionPipelineValue(game: ShowcaseGame) {
  const effective = game.preferred_pipeline || game.launch_method || "";
  const recommended = game.available_pipelines?.find((pipeline) => pipeline.recommended)?.id || "";
  return (
    [effective, recommended].find((id) => pipelineOptions.some((option) => option.id === id)) || pipelineOptions[0].id
  );
}

function scrollDock(direction: -1 | 1) {
  const next = carouselOffset.value + direction;
  carouselOffset.value = Math.max(0, Math.min(next, Math.max(0, filteredGames.value.length - 5)));
}

function isWineSteamRouteId(launchMethod: string) {
  return ["d3dmetal", "vkd3d", "d3d9", "dxmt", "dxmt_32", "steam", "wine_steam"].includes(launchMethod.toLowerCase());
}

async function launchGame(game: ShowcaseGame) {
  if (!game.isLive) {
    toast.show("Connect your library to launch this game", "info");
    return;
  }
  launchingAppId.value = game.appid;
  const launchMethod = game.launch_method || "auto";
  const endpoint = isWineSteamRouteId(launchMethod) ? "/steam/launch-game" : "/game/launch-auto";
  const result = await api<{ ok: boolean; pid?: number; error?: string }>(
    "POST",
    endpoint,
    { appid: game.appid, launchMethod },
    10 * 60 * 1000,
  );
  launchingAppId.value = null;
  if (result?.ok) {
    rememberPlayed(game.appid);
    toast.show(`Launched ${game.name}`, "success");
    // Remind the player about the Cmd+Opt+Q escape hatch once the game has
    // had a moment to take over the screen.
    setTimeout(() => {
      void window.metalsharp.showLaunchOverlay(game.name).catch(() => {});
    }, 5000);
  } else toast.show(result?.error || `Failed to launch ${game.name}`, "error");
}

async function savePipeline() {
  const game = featuredGame.value;
  if (!game || pipelineSaving.value) return;
  const previous = normalizePipeline(game.preferred_pipeline || game.launch_method);
  pipelineSaving.value = true;
  game.preferred_pipeline = selectedPipeline.value;
  game.launch_method = selectedPipeline.value;
  // NOTE: never call /bottles/sync-steam here — it seeds a manifest with an
  // explicit vkd3d override for EVERY game, masking their recommended routes.
  const result =
    selectedPipeline.value === "d3dmetal"
      ? await api<{ ok: boolean; error?: string }>(
          "POST",
          "/d3dmetal/bottles/save",
          {
            appid: game.appid,
            bottleId: game.bottle_id || `steam_${game.appid}`,
            name: game.name,
            gameDir: game.wine_game_path || "",
          },
          10 * 60 * 1000,
        )
      : await api<{ ok: boolean; error?: string }>("POST", "/bottles/edit", {
          id: game.bottle_id || `steam_${game.appid}`,
          name: game.name,
          preferredPipeline: selectedPipeline.value,
        });
  pipelineSaving.value = false;
  if (result?.ok) {
    toast.show(`${game.name}: ${pipelineLabel(selectedPipeline.value)} selected`, "success");
    await reloadLibrary();
  } else {
    game.preferred_pipeline = previous === "auto" ? null : previous;
    game.launch_method = previous;
    selectedPipeline.value = previous;
    toast.show(result?.error || "Could not save bottle route", "error");
  }
}

async function loadGameSettings() {
  const [config, metalFx] = await Promise.all([
    api<{ ok: boolean; controllerInput?: "off" | "x" | "d"; msync?: boolean }>("GET", "/config"),
    api<{ ok: boolean; enabled?: boolean; factor?: number }>("GET", "/metalfx/state"),
  ]);
  if (config?.ok) {
    controllerInput.value = config.controllerInput || "off";
    msyncEnabled.value = config.msync !== false;
  }
  if (metalFx?.ok) {
    metalFxMode.value =
      metalFx.enabled === false ? "off" : Math.abs((metalFx.factor || 2.0) - 1.75) < 0.01 ? "1.75" : "2.0";
  }
}

async function loadSteamEmuStatus(appid: number) {
  const result = await api<{
    ok: boolean;
    goldberg_active: boolean;
    cache_files_ok?: boolean;
    backed_up_at?: number | null;
  }>("GET", `/goldberg/status?appid=${appid}`);
  if (result?.ok) steamEmuActive.value = result.goldberg_active;
}

async function setSteamEmu(enabled: boolean) {
  const game = featuredGame.value;
  if (!game || steamEmuBusy.value) return;
  steamEmuBusy.value = true;
  const result = await api<{ ok: boolean; goldberg_active: boolean; cache_files_ok?: boolean; error?: string }>(
    "POST",
    "/goldberg/toggle",
    { appid: game.appid, enable: enabled },
  );
  if (result?.ok) {
    steamEmuActive.value = result.goldberg_active;
    toast.show(
      enabled
        ? result.cache_files_ok === false
          ? "Steam Emu enabled, but no backup cache found — restore from OFF may rely on .orig files only"
          : "Steam Emu enabled; original Steam DLLs cached for safe restore"
        : "Steam Emu disabled; original Steam DLLs restored",
      "success",
    );
  } else {
    toast.show(result?.error || "Failed to toggle Steam Emu", "error");
  }
  steamEmuBusy.value = false;
}

async function setMetalFx(mode: "1.75" | "2.0" | "off") {
  if (metalFxBusy.value) return;
  metalFxBusy.value = true;
  const result = await api<{ ok: boolean }>(
    "POST",
    "/metalfx/toggle",
    mode === "off" ? { enabled: false } : { enabled: true, factor: Number(mode) },
  );
  if (result?.ok) metalFxMode.value = mode;
  else toast.show("Failed to update MetalFX", "error");
  metalFxBusy.value = false;
}

async function setController(mode: "off" | "x" | "d") {
  if (controllerBusy.value) return;
  controllerBusy.value = true;
  const result = await api<{ ok: boolean }>("POST", "/config", { controllerInput: mode });
  if (result?.ok) controllerInput.value = mode;
  else toast.show("Failed to update controller input", "error");
  controllerBusy.value = false;
}

async function setMsync(enabled: boolean) {
  if (msyncBusy.value) return;
  msyncBusy.value = true;
  const result = await api<{ ok: boolean }>("POST", "/config", { msync: enabled });
  if (result?.ok) msyncEnabled.value = enabled;
  else toast.show("Failed to update msync", "error");
  msyncBusy.value = false;
}

async function toggleSteam() {
  if (wineSteamRunning.value) {
    const result = await api<{ ok: boolean; running?: boolean; error?: string }>("POST", "/steam/stop");
    if (result?.ok) wineSteamRunning.value = false;
    toast.show(result?.error || "Wine Steam stopped", result?.ok ? "success" : "error");
  } else {
    const result = await api<{ ok: boolean; error?: string }>("POST", "/steam/launch");
    if (result?.ok) wineSteamRunning.value = true;
    toast.show(result?.error || "Starting Wine Steam...", result?.ok ? "success" : "error");
  }
  await reloadLibrary();
}

watch(
  featuredGame,
  (game) => {
    // Show the game's assigned route: an explicit user choice when one is
    // saved, otherwise the backend's recommended default (launch_method is
    // the effective route the backend launches with).
    const effective = game?.preferred_pipeline || game?.launch_method || "";
    const recommended = game?.available_pipelines?.find((pipeline) => pipeline.recommended)?.id || "";
    const selectable = [effective, recommended].find((id) => pipelineOptions.some((option) => option.id === id));
    selectedPipeline.value = selectable || pipelineOptions[0].id;
    if (game) void loadGameSettings();
    if (game?.installed) void loadSteamEmuStatus(game.appid);
    else steamEmuActive.value = false;
  },
  { immediate: true },
);

const defaultRulesAppliedKey = "metalsharp-default-rules-applied-v1";

// One-time repair: earlier builds seeded every Steam bottle manifest with an
// explicit vkd3d override (via /bottles/sync-steam), which masked each game's
// backend-recommended route. Any installed game still pinned to vkd3d that the
// backend does not recommend is reset to its recommended route and saved.
// Genuine user choices — anything other than the untouched vkd3d seed — are
// left alone, and this runs only once per machine.
async function applyDefaultRulesOnce() {
  if (localStorage.getItem(defaultRulesAppliedKey)) return;
  localStorage.setItem(defaultRulesAppliedKey, "1");
  const games = allGames.value.filter((game) => game.installed && game.preferred_pipeline === "vkd3d");
  const repairs = games
    .map((game) => {
      const recommended = game.available_pipelines?.find((pipeline) => pipeline.recommended)?.id;
      if (!recommended || recommended === "vkd3d") return null;
      return api<{ ok: boolean }>("POST", "/bottles/edit", {
        id: game.bottle_id || `steam_${game.appid}`,
        name: game.name,
        preferredPipeline: recommended,
      })
        .then((result) => {
          if (result?.ok) {
            game.preferred_pipeline = recommended;
            game.launch_method = recommended;
            game.launch_method_name = pipelineLabel(recommended);
          }
        })
        .catch(() => {});
    })
    .filter((repair): repair is Promise<void> => repair !== null);
  if (!repairs.length) return;
  await Promise.all(repairs);
  await reloadLibrary();
}

watch(
  allGames,
  (games) => {
    if (games.length) void applyDefaultRulesOnce();
  },
  { immediate: true },
);

watch(
  () => filteredGames.value.length,
  (length) => {
    carouselOffset.value = Math.min(carouselOffset.value, Math.max(0, length - 5));
  },
);

onMounted(() => {
  // Honor a Play/Collection request made while another page was active.
  if (pendingLibraryTab.value === "collection") {
    pendingLibraryTab.value = null;
    openCollection();
  } else if (pendingLibraryTab.value === "play") {
    pendingLibraryTab.value = null;
    openPlay();
  }
  void loadGameSettings();
  window.metalsharp
    .backendBaseUrl()
    .then((base) => {
      backendBase.value = base;
    })
    .catch(() => {});
  // Fired when the Steam Art Manager save button (or Steam's "Set Custom
  // Image") writes grid artwork. Bust the per-game artwork caches so every
  // app card and the hero re-probe against the new images.
  window.metalsharp.onGridArtChanged?.(() => {
    artVersion.value = Date.now();
    artworkSources.value = {};
    heroArtSources.value = {};
    fallbackArtApps.value = new Set<number>();
    if (featuredGame.value) probeHeroArt(featuredGame.value);
  });
});

function handleImageError(event: Event, game: ShowcaseGame) {
  const image = event.target as HTMLImageElement;
  const candidates = artworkSources.value[game.appid] || artworkCandidates(game);
  const current = image.currentSrc || image.src;
  const foundIndex = candidates.findIndex((candidate) => current.endsWith(candidate) || candidate.endsWith(current));
  const currentIndex = foundIndex < 0 ? 0 : foundIndex;
  const next = candidates[currentIndex + 1];
  if (next) {
    artworkSources.value = { ...artworkSources.value, [game.appid]: candidates.slice(currentIndex + 1) };
    image.src = next;
    return;
  }
  // Every static candidate failed — ask Steam for the hashed artwork.
  void enrichArtwork(game.appid).then((extra) => {
    const card = extra?.card || extra?.shot;
    if (card && !current.endsWith(card)) {
      image.classList.remove("image-missing");
      image.src = card;
      markFallbackArt(game.appid);
      artworkSources.value = {
        ...artworkSources.value,
        [game.appid]: [card, ...(extra?.shot && extra.shot !== card ? [extra.shot] : [])],
      };
      return;
    }
    image.src = sharpLogoUrl;
    image.classList.add("image-missing");
  });
}

// Steam's unhashed storepagebackground is a pre-graded/darkened backdrop —
// fine as a placeholder, but swap in the colorful key art once enriched.
function handleImageLoad(event: Event, game: ShowcaseGame) {
  const image = event.target as HTMLImageElement;
  const current = image.currentSrc || image.src;
  if (!isFallbackArtUrl(current)) return;
  markFallbackArt(game.appid);
  void enrichArtwork(game.appid).then((extra) => {
    const card = extra?.card || extra?.shot;
    if (card && !current.endsWith(card)) {
      image.src = card;
      artworkSources.value = {
        ...artworkSources.value,
        [game.appid]: [card, ...(extra?.shot && extra.shot !== card ? [extra.shot] : [])],
      };
    }
  });
}
</script>

<template>
  <svg class="library-filter-defs" aria-hidden="true">
    <defs>
      <filter id="hero-sharpen-50" x="-5%" y="-5%" width="110%" height="110%">
        <feConvolveMatrix order="3" kernelMatrix="0 -0.5 0 -0.5 3 -0.5 0 -0.5 0" preserveAlpha="true" />
      </filter>
    </defs>
  </svg>
  <div class="library-view" :style="libraryThemeStyle">
    <LibraryTopbar
      :active-tab="currentTab"
      :search="search"
      @update:search="search = $event"
      @navigate="onTopbarNavigate"
    />

    <main class="library-scroll" :style="heroBleedStyle">
      <section v-if="showFullLibrary" class="collection-page">
        <div class="collection-page-header">
          <div>
            <p class="library-hero-eyebrow">{{ t("library.collectionEyebrow") }}</p>
            <h1>{{ t("library.installedGames") }}</h1>
            <p>{{ installedCount }} games installed and ready in your MetalSharp library.</p>
          </div>
          <button class="collection-back-button" type="button" @click="openPlay">
            <IconGamepad width="16" height="16" />
            <span>{{ t("library.backToPlay") }}</span>
          </button>
        </div>
        <div v-if="filteredGames.length" class="collection-grid">
          <article v-for="game in filteredGames" :key="game.appid" class="collection-card">
            <img
              :src="gameArt(game)"
              :alt="game.name"
              loading="lazy"
              :class="{ 'fallback-sharpen': fallbackArtApps.has(game.appid) }"
              @error="handleImageError($event, game)"
              @load="handleImageLoad($event, game)"
            />
            <div class="collection-card-shade"></div>
            <div class="collection-card-info">
              <strong>{{ game.name }}</strong>
              <div class="collection-card-actions">
                <select
                  class="collection-bottle-select"
                  :value="collectionPipelineValue(game)"
                  :disabled="collectionSaving[game.appid]"
                  title="Bottle pipeline"
                  @change="saveCollectionPipeline(game, $event)"
                  @click.stop
                >
                  <option v-for="option in pipelineOptions" :key="option.id" :value="option.id">
                    {{ option.label }}
                  </option>
                </select>
                <button
                  type="button"
                  @click="
                    selectGame(game);
                    openPlay();
                    launchGame(game);
                  "
                >
                  <IconPlay width="14" height="14" fill="currentColor" />
                  <span>{{ t("library.play") }}</span>
                </button>
              </div>
            </div>
          </article>
        </div>
        <div v-else class="collection-empty">
          <IconLibrary width="42" height="42" />
          <h2>{{ t("library.noInstalledGames") }}</h2>
          <p>{{ t("library.noInstalledDescription") }}</p>
        </div>
      </section>
      <template v-else>
        <section v-if="featuredGame" class="library-hero">
          <div class="library-hero-art" :style="{ backgroundImage: `url('${heroArt(featuredGame)}')` }"></div>
          <div class="library-hero-wash"></div>
          <div class="library-hero-content">
            <h1>{{ featuredGame.name }}</h1>
            <div class="library-hero-actions">
              <button class="library-play-button" type="button" @click="launchGame(featuredGame)">
                <IconPlay width="18" height="18" fill="currentColor" />
                <span>{{ launchingAppId === featuredGame.appid ? "Launching" : "Play" }}</span>
              </button>
            </div>
          </div>
          <div class="library-hero-controls" @click.stop>
            <button
              class="library-art-button"
              type="button"
              title="Customize Steam artwork with Steam Art Manager"
              aria-label="Customize Steam artwork with Steam Art Manager"
              :disabled="artManagerOpening"
              @click="openArtManager"
            >
              <svg
                width="16"
                height="16"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
                aria-hidden="true"
              >
                <path d="M9.06 11.9l8.07-8.06a2.85 2.85 0 1 1 4.03 4.03l-8.06 8.08" />
                <path
                  d="M7.07 14.94c-1.66 0-3 1.35-3 3.02 0 1.33-2.5 1.52-2 2.02 1.08 1.1 2.49 2.02 4 2.02 2.2 0 4-1.8 4-4.04a3.01 3.01 0 0 0-3-3.02z"
                />
              </svg>
            </button>
            <div class="library-bottle-control">
              <span class="library-control-label">{{ t("library.bottle") }}</span>
              <select v-model="selectedPipeline" :disabled="pipelineSaving" @change="savePipeline">
                <option v-for="option in pipelineOptions" :key="option.id" :value="option.id">
                  {{ option.label }}
                </option>
              </select>
              <span v-if="pipelineSaving" class="library-control-saving">{{ t("library.saving") }}</span>
            </div>
            <button
              class="library-game-settings-button"
              type="button"
              :aria-label="t('library.gameSettings')"
              :title="t('library.gameSettings')"
              @click="gameSettingsOpen = !gameSettingsOpen"
            >
              <IconSettings width="17" height="17" />
            </button>
            <div v-if="gameSettingsOpen" class="game-settings-popover" @click.stop>
              <div class="game-settings-header">
                <div>
                  <span>{{ t("ui.game.title") }}</span>
                  <strong>{{ featuredGame.name }}</strong>
                </div>
                <button type="button" aria-label="Close settings" @click="gameSettingsOpen = false">×</button>
              </div>
              <div class="game-setting-row">
                <span>{{ t("ui.game.metalFx") }}</span>
                <div class="game-setting-options">
                  <button :class="{ active: metalFxMode === '1.75' }" type="button" @click="setMetalFx('1.75')">
                    1.75×
                  </button>
                  <button :class="{ active: metalFxMode === '2.0' }" type="button" @click="setMetalFx('2.0')">
                    2×
                  </button>
                  <button :class="{ active: metalFxMode === 'off' }" type="button" @click="setMetalFx('off')">
                    {{ t("ui.game.off") }}
                  </button>
                </div>
              </div>
              <div class="game-setting-row">
                <span>{{ t("ui.game.controllerInput") }}</span>
                <div class="game-setting-options">
                  <button :class="{ active: controllerInput === 'off' }" type="button" @click="setController('off')">
                    {{ t("ui.game.off") }}
                  </button>
                  <button :class="{ active: controllerInput === 'x' }" type="button" @click="setController('x')">
                    XInput
                  </button>
                  <button :class="{ active: controllerInput === 'd' }" type="button" @click="setController('d')">
                    DInput
                  </button>
                </div>
              </div>
              <div class="game-setting-row game-setting-toggle">
                <span>{{ t("ui.game.msync") }}</span>
                <button type="button" :class="{ active: msyncEnabled }" @click="setMsync(!msyncEnabled)">
                  {{ msyncEnabled ? t("ui.game.on") : t("ui.game.off") }}
                </button>
              </div>
              <div class="game-setting-row game-setting-toggle">
                <span>{{ t("ui.game.steamEmu") }}</span>
                <button
                  type="button"
                  :class="{ active: steamEmuActive }"
                  :disabled="steamEmuBusy || !featuredGame?.installed"
                  :title="featuredGame?.installed ? 'gbe_fork Steam emulator' : 'Requires an installed game'"
                  @click="setSteamEmu(!steamEmuActive)"
                >
                  {{ steamEmuActive ? t("ui.game.on") : t("ui.game.off") }}
                </button>
              </div>
            </div>
          </div>
        </section>
        <section v-else class="library-empty-hero">
          <div class="library-empty-hero-content">
            <IconLibrary width="42" height="42" />
            <p class="library-hero-eyebrow">{{ t("library.yourLibraryAwaits") }}</p>
            <h1>{{ t("library.noInstalledGamesShort") }}</h1>
            <p class="library-hero-description">{{ t("library.startSteamDescription") }}</p>
            <button class="library-play-button" type="button" @click="toggleSteam">
              <IconGamepad width="18" height="18" />
              <span>{{ t("library.startSteam") }}</span>
            </button>
          </div>
        </section>

        <section class="jump-back-section">
          <div class="jump-back-heading">
            <button type="button" @click="openCollection">
              <span>{{ t("library.viewAll") }}</span>
              <IconChevronRight width="18" height="18" />
            </button>
          </div>

          <div class="showcase-dock">
            <button
              class="dock-arrow"
              type="button"
              aria-label="Previous games"
              :disabled="!canScrollBack"
              @click="scrollDock(-1)"
            >
              <IconChevronLeft width="20" height="20" />
            </button>
            <div class="showcase-grid">
              <article
                v-for="(game, index) in jumpBackGames"
                :key="game.appid"
                class="showcase-card"
                :class="{
                  'showcase-card-featured': game.appid === featuredGame?.appid,
                  [`showcase-card-${index}`]: true,
                }"
                tabindex="0"
                role="button"
                @click="selectGame(game)"
                @keydown.enter="selectGame(game)"
              >
                <div class="showcase-cover">
                  <img
                    :src="gameArt(game)"
                    :alt="game.name"
                    loading="lazy"
                    :class="{ 'fallback-sharpen': fallbackArtApps.has(game.appid) }"
                    @error="handleImageError($event, game)"
                    @load="handleImageLoad($event, game)"
                  />
                  <div class="showcase-cover-shade"></div>
                  <button class="showcase-play" type="button" @click.stop="launchGame(game)">
                    <IconPlay width="16" height="16" fill="currentColor" />
                    <span>{{ launchingAppId === game.appid ? "Launching" : "Play" }}</span>
                  </button>
                </div>
                <div class="showcase-card-reflection" aria-hidden="true"></div>
              </article>
            </div>
            <button
              class="dock-arrow"
              type="button"
              aria-label="Next games"
              :disabled="!canScrollForward"
              @click="scrollDock(1)"
            >
              <IconChevronRight width="20" height="20" />
            </button>
          </div>
        </section>
      </template>
    </main>

    <LibraryFooter />
  </div>
</template>

<style scoped>
.library-filter-defs {
  position: fixed;
  width: 0;
  height: 0;
  pointer-events: none;
}
.library-view {
  --library-accent: #e8d6b7;
  --library-accent-glow: rgba(232, 214, 183, 0.28);
  --library-dock-glow: rgba(42, 161, 255, 0.78);
  --library-control-bg: #080a0d;
  --library-control-text: #ffffff;
  --library-control-hover: #171a1e;
  --library-control-border: rgba(255, 255, 255, 0.22);
  --surface: #111416;
  --surface-raised: #191c1f;
  --line: rgba(231, 234, 236, 0.14);
  --soft-line: rgba(231, 234, 236, 0.09);
  --muted: #a7aaad;
  width: 100%;
  height: 100%;
  min-width: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  color: #f1f1ef;
  background: var(--surface);
  font-family:
    "Rethink Sans",
    -apple-system,
    BlinkMacSystemFont,
    "Segoe UI",
    sans-serif;
}
:global(:root[data-theme="skeleton"]) .library-view .library-hero h1,
:global(:root[data-theme="skeleton"]) .library-view .collection-page-header h1 {
  color: #eeeeee;
  text-shadow: 0 3px 18px rgba(0, 0, 0, 0.5);
}
:global(:root[data-theme="skeleton"]) .library-view .library-play-button,
:global(:root[data-theme="skeleton"]) .library-view .collection-back-button,
:global(:root[data-theme="skeleton"]) .library-view .collection-card-info button {
  color: #181818;
  background: #d6d0c4;
  border-color: #eeeeee;
}
:global(:root[data-theme="skeleton"]) .library-view .showcase-card-featured .showcase-cover {
  border-color: #d6d0c4;
  box-shadow:
    0 17px 29px rgba(0, 0, 0, 0.5),
    0 0 20px var(--library-dock-glow),
    0 1px 0 rgba(255, 255, 255, 0.15) inset;
}
.library-scroll {
  position: relative;
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  overflow-x: hidden;
  background: var(--surface);
  scrollbar-color: #4b4f52 transparent;
}
.library-scroll::before {
  display: none;
  content: "";
  pointer-events: none;
}
.library-scroll > * {
  position: relative;
  z-index: 1;
}
.collection-page {
  min-height: 100%;
  padding: 54px 42px 60px;
  background:
    radial-gradient(circle at 13% 8%, rgba(148, 120, 73, 0.17), transparent 28%),
    linear-gradient(180deg, #151819 0%, #101314 100%);
}
.collection-page-header {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 24px;
  max-width: 1400px;
  margin: 0 auto 30px;
}
.collection-page-header .library-hero-eyebrow {
  margin-bottom: 12px;
}
.collection-page-header h1 {
  color: #eee9dd;
  font-family: Georgia, "Times New Roman", serif;
  font-size: clamp(42px, 5vw, 66px);
  font-weight: 500;
  line-height: 0.98;
}
.collection-page-header p:last-child {
  margin-top: 13px;
  color: #aeb3b2;
  font-size: 14px;
}
.collection-back-button {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  min-height: 38px;
  padding: 0 14px;
  border: 1px solid rgba(231, 234, 236, 0.23);
  border-radius: 8px;
  color: #e1e3e2;
  background: #292d2f;
  cursor: pointer;
  font: inherit;
  font-size: 13px;
}
.collection-back-button:hover {
  border-color: rgba(231, 234, 236, 0.5);
  background: #363a3c;
}
.collection-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(190px, 1fr));
  gap: 24px 18px;
  max-width: 1400px;
  margin: 0 auto;
}
.collection-card {
  position: relative;
  overflow: hidden;
  aspect-ratio: 0.735;
  border: 1px solid rgba(224, 226, 224, 0.23);
  border-radius: 8px;
  background: #242729;
  box-shadow: 0 12px 24px rgba(0, 0, 0, 0.32);
  transition:
    transform 0.2s ease,
    border-color 0.2s ease;
}
.collection-card:hover {
  border-color: rgba(231, 196, 131, 0.72);
  transform: translateY(-5px);
}
.collection-card > img {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.collection-card-shade {
  position: absolute;
  inset: 0;
  background: linear-gradient(180deg, transparent 34%, rgba(5, 7, 8, 0.9) 100%);
}
.collection-card-info {
  position: absolute;
  right: 12px;
  bottom: 12px;
  left: 12px;
  display: flex;
  flex-direction: column;
  gap: 5px;
}
.collection-card-info strong {
  color: #f0f0ed;
  font-size: 14px;
  line-height: 1.15;
}
.collection-card-info > span {
  color: #c7c9c7;
  font-size: 11px;
}
.collection-card-actions {
  display: flex;
  align-items: center;
  gap: 6px;
}
.collection-card-actions .collection-bottle-select {
  flex: 1 1 auto;
  min-width: 0;
}
.collection-card-actions button {
  margin-top: 0;
}
.collection-bottle-select {
  width: fit-content;
  min-height: 24px;
  padding: 0 4px 0 7px;
  border: 1px solid var(--library-control-border);
  border-radius: 5px;
  color: #fff;
  background: color-mix(in srgb, var(--library-control-bg) 82%, transparent);
  cursor: pointer;
  font: inherit;
  font-size: 10.5px;
  font-weight: 650;
  outline: none;
  backdrop-filter: blur(8px);
}
.collection-bottle-select:hover:not(:disabled) {
  border-color: var(--library-accent);
}
.collection-bottle-select:disabled {
  opacity: 0.6;
  cursor: default;
}
.collection-bottle-select option {
  color: #151718;
  background: #ece7dc;
}
/* base.css paints selects black in the lava theme; the card selector must
   stay a single themed surface. */
.library-view .collection-bottle-select,
.library-view .collection-bottle-select:focus {
  background: color-mix(in srgb, var(--library-control-bg) 82%, transparent) !important;
  border: 1px solid var(--library-control-border) !important;
  box-shadow: none !important;
  color: #fff !important;
}
.collection-card-info button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  width: fit-content;
  min-height: 28px;
  padding: 0 11px;
  border: 1px solid var(--library-accent);
  border-radius: 6px;
  color: var(--library-control-bg);
  background: var(--library-accent);
  cursor: pointer;
  font: inherit;
  font-size: 11px;
  font-weight: 700;
  transition:
    filter 0.16s ease,
    transform 0.16s ease;
}
.collection-card-info button:hover {
  filter: brightness(1.09);
  transform: translateY(-1px);
}
.collection-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  min-height: 360px;
  color: #9da3a2;
  text-align: center;
}
.collection-empty svg {
  margin-bottom: 16px;
  opacity: 0.7;
}
.collection-empty h2 {
  color: #e8e9e7;
  font-size: 20px;
}
.collection-empty p {
  margin-top: 8px;
  font-size: 13px;
}
.library-hero {
  position: relative;
  height: clamp(520px, 62vh, 595px);
  min-height: 520px;
  display: flex;
  align-items: center;
  overflow: visible;
  z-index: auto;
  background-color: #242629;
  background-position: center center;
  background-size: cover;
  isolation: isolate;
}
.library-hero-art {
  position: absolute;
  z-index: 0;
  inset: 0;
  background-position: center center;
  background-size: cover;
  background-repeat: no-repeat;
  filter: url("#hero-sharpen-50");
}
.library-hero::before {
  display: none;
  content: "";
  position: absolute;
  z-index: 0;
  inset: 0;
  pointer-events: none;
}
.library-hero::after {
  content: "";
  position: absolute;
  z-index: 1;
  inset: 0;
  background: linear-gradient(
    90deg,
    rgba(6, 8, 9, 0.91) 0%,
    rgba(6, 8, 9, 0.62) 30%,
    rgba(6, 8, 9, 0.08) 67%,
    rgba(6, 8, 9, 0.17) 100%
  );
  pointer-events: none;
}
.library-empty-hero {
  position: relative;
  min-height: clamp(390px, 50vh, 520px);
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  background:
    radial-gradient(circle at 50% 42%, rgba(129, 137, 141, 0.18), transparent 31%),
    linear-gradient(120deg, #15191b, #252a2d 54%, #151719);
}
.library-empty-hero::before {
  content: "";
  position: absolute;
  inset: 0;
  background: repeating-linear-gradient(
    125deg,
    transparent 0 18px,
    rgba(255, 255, 255, 0.02) 19px,
    transparent 20px 42px
  );
  opacity: 0.35;
}
.library-empty-hero-content {
  position: relative;
  z-index: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  max-width: 570px;
  padding: 54px 24px;
  text-align: center;
}
.library-empty-hero-content > svg {
  margin-bottom: 18px;
  color: #c7cbcb;
  opacity: 0.75;
}
.library-empty-hero-content .library-hero-eyebrow {
  margin-bottom: 14px;
}
.library-empty-hero-content h1 {
  margin-bottom: 14px;
  color: #ece9e0;
  font-family: Georgia, "Times New Roman", serif;
  font-size: clamp(42px, 5vw, 68px);
  font-weight: 500;
}
.library-empty-hero-content .library-hero-description {
  margin-bottom: 26px;
  color: #c4c8c7;
  text-shadow: none;
}
.library-hero-wash {
  position: absolute;
  z-index: 2;
  right: 0;
  bottom: -120px;
  left: 0;
  height: 300px;
  background: linear-gradient(
    180deg,
    transparent 0%,
    rgba(17, 20, 22, 0.22) 34%,
    rgba(17, 20, 22, 0.76) 72%,
    #111416 100%
  );
  pointer-events: none;
}
.library-hero-content {
  position: relative;
  z-index: 3;
  width: min(610px, 54%);
  padding: 0 0 0 52px;
}
.library-hero-eyebrow {
  margin-bottom: 16px;
  color: rgba(240, 239, 231, 0.82);
  font-family: Georgia, "Times New Roman", serif;
  font-size: 11px;
  letter-spacing: 0.52em;
  line-height: 1;
  text-transform: uppercase;
}
.library-hero h1 {
  margin: 0 0 12px;
  color: #efcf9d;
  font-family: Georgia, "Times New Roman", serif;
  font-size: clamp(24px, 2.625vw, 39px);
  font-weight: 500;
  letter-spacing: 0.01em;
  line-height: 0.96;
  text-transform: uppercase;
  text-shadow: 0 3px 18px rgba(0, 0, 0, 0.4);
}
.library-hero-description {
  max-width: 520px;
  color: #e1e2e1;
  font-size: 17px;
  line-height: 1.42;
  text-shadow: 0 2px 8px rgba(0, 0, 0, 0.66);
}
.library-hero-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 17px;
}
.library-hero-tags span {
  padding: 6px 12px;
  border: 1px solid rgba(219, 222, 222, 0.34);
  border-radius: 999px;
  color: #cfd1d0;
  background: rgba(10, 12, 13, 0.52);
  font-size: 11px;
  line-height: 1;
  white-space: nowrap;
}
.library-hero-actions {
  display: flex;
  align-items: center;
  gap: 22px;
  margin-top: 34px;
}
.library-play-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
  width: 204px;
  height: 48px;
  border: 1px solid var(--library-control-border);
  border-radius: 8px;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.27);
  cursor: pointer;
  font: inherit;
  font-size: 15px;
  font-weight: 700;
  transition:
    transform 0.18s ease,
    background 0.18s ease;
}
.library-play-button:hover {
  background: var(--library-control-hover);
  transform: translateY(-2px);
}
.library-ready-state {
  display: flex;
  align-items: center;
  gap: 9px;
  color: #ebeded;
}
.library-ready-state svg {
  color: #e2e5e3;
}
.library-ready-state div {
  display: flex;
  flex-direction: column;
  gap: 3px;
}
.library-ready-state strong {
  font-size: 14px;
  font-weight: 500;
}
.library-ready-state span {
  color: #adb1b1;
  font-size: 11px;
}
.library-hero-controls {
  position: absolute;
  z-index: 4;
  top: calc(50% + 78px);
  right: 44px;
  bottom: auto;
  display: flex;
  align-items: flex-end;
  transform: translateY(-50%);
  gap: 9px;
}
.library-bottle-control {
  display: flex;
  align-items: center;
  gap: 9px;
  min-height: 42px;
  padding: 0 10px 0 13px;
  border: 1px solid rgba(231, 234, 236, 0.3);
  border-radius: 8px;
  background: rgba(12, 15, 16, 0.72);
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.24);
  backdrop-filter: blur(14px);
}
.library-view .library-art-button {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 42px;
  padding: 0 12px;
  border: 1px solid rgba(231, 234, 236, 0.3);
  border-radius: 8px;
  background: rgba(12, 15, 16, 0.72);
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.24);
  backdrop-filter: blur(14px);
  color: #dfe3e2;
  cursor: pointer;
}
.library-view .library-art-button:hover {
  border-color: var(--library-accent, rgba(231, 234, 236, 0.55));
  color: #fff;
}
.library-view .library-art-button:disabled {
  opacity: 0.55;
  cursor: default;
}
.library-control-label {
  color: #aeb4b3;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.1em;
  text-transform: uppercase;
}
.library-bottle-control select {
  width: 88px;
  min-width: 88px;
  border: 0;
  outline: 0;
  color: var(--library-control-text);
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 13px;
  font-weight: 600;
}
/* base.css paints all inputs/selects black in the lava theme; the hero bottle
   control must stay one themed surface like the header search bar. */
.library-view .library-bottle-control select,
.library-view .library-bottle-control select:focus {
  background: transparent !important;
  border: 0 !important;
  box-shadow: none !important;
  color: #fff !important;
}
.library-bottle-control select option {
  color: #151718;
  background: #ece7dc;
}
.library-control-saving {
  color: #bfc2bf;
  font-size: 10px;
}
.library-game-settings-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 42px;
  height: 42px;
  border: 1px solid var(--library-control-border);
  border-radius: 8px;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.24);
  cursor: pointer;
  transition:
    background 0.18s ease,
    border-color 0.18s ease,
    transform 0.18s ease;
}
.library-game-settings-button:hover {
  border-color: var(--library-control-border);
  background: var(--library-control-hover);
  transform: translateY(-1px);
}
.game-settings-popover {
  position: absolute;
  right: 0;
  bottom: 52px;
  width: 330px;
  padding: 15px;
  border: 1px solid var(--library-accent);
  border-radius: 10px;
  color: var(--library-control-text);
  background: color-mix(in srgb, var(--library-control-bg) 96%, transparent);
  box-shadow: 0 18px 45px rgba(0, 0, 0, 0.46);
  backdrop-filter: blur(18px);
}
.game-settings-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--library-control-border);
}
.game-settings-header span {
  display: block;
  margin-bottom: 5px;
  color: color-mix(in srgb, var(--library-control-text) 62%, transparent);
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.12em;
}
.game-settings-header strong {
  display: block;
  max-width: 250px;
  overflow: hidden;
  color: var(--library-control-text);
  font-size: 14px;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.game-settings-header > button {
  border: 0;
  color: color-mix(in srgb, var(--library-control-text) 70%, transparent);
  background: transparent;
  cursor: pointer;
  font-size: 22px;
  line-height: 1;
}
.game-setting-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 13px 0 0;
}
.game-setting-row > span {
  color: color-mix(in srgb, var(--library-control-text) 82%, transparent);
  font-size: 12px;
}
.game-setting-options {
  display: flex;
  gap: 4px;
}
.game-setting-options button,
.game-setting-toggle button {
  min-height: 27px;
  padding: 0 8px;
  border: 1px solid var(--library-control-border);
  border-radius: 5px;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  cursor: pointer;
  font: inherit;
  font-size: 10px;
}
.game-setting-options button:hover,
.game-setting-toggle button:hover {
  background: var(--library-control-hover);
}
.game-setting-options button.active,
.game-setting-toggle button.active {
  border-color: var(--library-accent);
  color: var(--library-control-bg);
  background: var(--library-accent);
  box-shadow: 0 0 10px color-mix(in srgb, var(--library-accent) 45%, transparent);
}
.library-hero-quote {
  position: absolute;
  z-index: 3;
  right: 54px;
  bottom: 91px;
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: 7px;
  color: rgba(235, 234, 227, 0.9);
  font-family: Georgia, "Times New Roman", serif;
  font-size: 15px;
  text-shadow: 0 2px 8px #000;
}
.library-hero-quote span {
  font-family: "Rethink Sans", sans-serif;
  font-size: 11px;
  letter-spacing: 0.05em;
}
.library-hero-footer {
  position: absolute;
  z-index: 3;
  right: 53px;
  bottom: 20px;
  color: rgba(233, 232, 222, 0.72);
  font-family: Georgia, "Times New Roman", serif;
  font-size: 9px;
  letter-spacing: 0.42em;
}
.jump-back-section {
  position: relative;
  z-index: 2;
  margin-top: -165px;
  padding: 18px 34px 28px;
  background:
    linear-gradient(180deg, transparent 0%, transparent 84px, rgba(17, 20, 22, 0.64) 190px, #111416 300px),
    radial-gradient(circle at 17% 100%, var(--library-accent-glow), transparent 28%),
    linear-gradient(180deg, transparent 0%, #111416 300px);
}
.jump-back-section::before {
  display: none;
}
.jump-back-heading {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  margin-bottom: 7px;
}
.jump-back-heading h2 {
  color: #eeeeed;
  font-size: 18px;
  font-weight: 650;
  letter-spacing: -0.02em;
}
.jump-back-heading button {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  border: 0;
  color: #d4d5d4;
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 13px;
}
.jump-back-heading button:hover {
  color: #fff;
}
.showcase-dock {
  position: relative;
  z-index: 3;
  display: flex;
  align-items: center;
  gap: 10px;
  max-width: 1480px;
  margin: 0 auto;
}
.dock-arrow {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 38px;
  width: 38px;
  height: 66px;
  border: 1px solid var(--library-accent);
  border-radius: 8px;
  color: #fff;
  background: rgba(40, 44, 45, 0.72);
  cursor: pointer;
  transition:
    color 0.18s ease,
    border-color 0.18s ease,
    background 0.18s ease,
    transform 0.18s ease;
}
.dock-arrow:hover:not(:disabled) {
  border-color: var(--library-accent);
  color: #fff;
  background: color-mix(in srgb, var(--library-accent) 16%, rgba(62, 63, 61, 0.9));
  transform: translateY(-2px);
}
.dock-arrow:disabled {
  cursor: default;
  opacity: 0.28;
}
.showcase-grid {
  display: grid;
  grid-template-columns: repeat(5, minmax(0, 1fr));
  align-items: end;
  gap: clamp(12px, 1.7vw, 28px);
  flex: 1 1 auto;
  min-width: 0;
  max-width: 1390px;
  margin: 0 auto;
  padding: 8px 10px 20px;
  perspective: 1100px;
}
.showcase-card {
  position: relative;
  min-width: 0;
  cursor: pointer;
  outline: none;
  isolation: isolate;
  transform-origin: center bottom;
  transition:
    transform 0.25s ease,
    filter 0.25s ease;
}
.showcase-card::after {
  content: "";
  position: absolute;
  z-index: -1;
  top: 24px;
  right: -20px;
  bottom: -6px;
  left: -20px;
  border-radius: 18px;
  background: radial-gradient(
    ellipse at center 64%,
    var(--library-dock-glow) 0%,
    color-mix(in srgb, var(--library-dock-glow) 58%, transparent) 34%,
    transparent 74%
  );
  filter: blur(12px);
  opacity: 0.82;
  pointer-events: none;
}
.showcase-card.showcase-card-featured::after {
  top: 20px;
  right: -28px;
  bottom: -8px;
  left: -28px;
  filter: blur(14px);
  opacity: 1;
}
.showcase-card:hover,
.showcase-card:focus-visible {
  z-index: 5;
  transform: translateY(-10px) scale(1.035) !important;
  filter: brightness(1.08);
}
.showcase-card-0 {
  transform: perspective(1000px) rotateY(-7deg) rotateX(1deg) translateY(4px);
}
.showcase-card-1 {
  transform: perspective(1000px) rotateY(-3deg) rotateX(0.5deg) translateY(-2px);
}
.showcase-card-2 {
  transform: translateY(-3px);
}
.showcase-card-3 {
  transform: perspective(1000px) rotateY(3deg) rotateX(0.5deg) translateY(-2px);
}
.showcase-card-4 {
  transform: perspective(1000px) rotateY(7deg) rotateX(1deg) translateY(4px);
}
.showcase-card.showcase-card-featured {
  z-index: 2;
  transform: scale(1.07) translateY(-13px) !important;
}
.showcase-cover {
  position: relative;
  overflow: hidden;
  aspect-ratio: 0.735;
  /* Cover size tracks viewport height, not column width: widening the window
     must not grow the dock downward past the footer. At full height this cap
     matches the previous fullscreen card size (~252px wide / 342px tall). */
  width: min(100%, calc(clamp(200px, 100vh - 620px, 342px) * 0.735));
  margin: 0 auto;
  border: 1px solid rgba(224, 226, 224, 0.33);
  border-radius: 7px;
  background: #242729;
  box-shadow:
    0 15px 23px rgba(0, 0, 0, 0.42),
    0 1px 0 rgba(255, 255, 255, 0.13) inset;
}
.showcase-card-featured .showcase-cover {
  border-color: color-mix(in srgb, var(--library-accent) 82%, rgba(226, 177, 83, 0.84));
  box-shadow:
    0 17px 29px rgba(0, 0, 0, 0.5),
    0 0 20px var(--library-accent-glow),
    0 1px 0 rgba(255, 255, 255, 0.15) inset;
}
.showcase-cover img {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: cover;
  transition: transform 0.4s ease;
}
.showcase-card:hover .showcase-cover img {
  transform: scale(1.035);
}
.showcase-cover img.fallback-sharpen,
.collection-card img.fallback-sharpen {
  filter: url("#hero-sharpen-50");
}
.showcase-cover img.image-missing {
  opacity: 0;
}
.showcase-cover-shade {
  position: absolute;
  inset: 0;
  background: linear-gradient(180deg, rgba(0, 0, 0, 0.02), transparent 52%, rgba(0, 0, 0, 0.4));
  pointer-events: none;
}
.showcase-play {
  position: absolute;
  left: 50%;
  bottom: 16px;
  display: inline-flex;
  align-items: center;
  gap: 7px;
  min-width: 96px;
  min-height: 34px;
  justify-content: center;
  padding: 0 12px;
  border: 1px solid rgba(255, 255, 255, 0.5);
  border-radius: 6px;
  color: #171819;
  background: #eee5d6;
  cursor: pointer;
  font: inherit;
  font-size: 12px;
  font-weight: 700;
  opacity: 0;
  transform: translate(-50%, 8px);
  transition:
    opacity 0.2s ease,
    transform 0.2s ease;
}
.showcase-card:hover .showcase-play,
.showcase-card:focus-visible .showcase-play {
  opacity: 1;
  transform: translate(-50%, 0);
}
.showcase-card-reflection {
  height: 12px;
  margin: 1px 9% 0;
  opacity: 0.34;
  background: linear-gradient(180deg, color-mix(in srgb, var(--library-dock-glow) 68%, transparent), transparent 88%);
  box-shadow: 0 -3px 9px color-mix(in srgb, var(--library-dock-glow) 48%, transparent);
  filter: blur(5px);
  transform: scaleY(-1) perspective(250px) rotateX(38deg);
  transform-origin: top;
  pointer-events: none;
}
@media (max-width: 1100px) {
  .library-topbar {
    grid-template-columns: minmax(0, 1fr) minmax(250px, 390px) minmax(0, 1fr);
    gap: 12px;
  }
  .library-brand {
    padding-left: 0;
  }
  .library-nav-item {
    padding-inline: 8px;
  }
  .library-hero-content {
    width: 64%;
    padding-left: 38px;
  }
  .library-hero-quote,
  .library-hero-footer {
    right: 32px;
  }
}
@media (max-width: 780px) {
  .library-topbar {
    display: grid;
    grid-template-columns: 1fr auto;
    min-height: 118px;
    padding-top: 16px;
    padding-bottom: 12px;
  }
  .library-header-left {
    gap: 10px;
  }
  .library-brand {
    padding-left: 0;
  }
  .library-steam-button {
    padding-inline: 10px;
  }
  .library-search {
    position: static;
    grid-column: 1 / -1;
    grid-row: 2;
    width: 100%;
    max-width: none;
    margin: 0;
    transform: none;
  }
  .library-nav {
    gap: 2px;
  }
  .library-nav-item span {
    display: none;
  }
  .library-settings-button {
    margin-left: 3px;
  }
  .library-theme-button span {
    display: none;
  }
  .library-hero {
    min-height: 540px;
    align-items: flex-end;
    background-position: 62% center;
  }
  .library-hero-content {
    width: 100%;
    padding: 220px 24px 34px;
    background: linear-gradient(0deg, rgba(7, 9, 10, 0.92), transparent);
  }
  .library-hero h1 {
    font-size: 24px;
  }
  .library-hero-description {
    font-size: 15px;
  }
  .library-hero-controls {
    top: auto;
    right: 18px;
    bottom: 72px;
    left: 18px;
    justify-content: flex-end;
    transform: none;
  }
  .library-bottle-control {
    min-width: 0;
  }
  .library-bottle-control select {
    width: 82px;
    min-width: 82px;
  }
  .game-settings-popover {
    right: 0;
    width: min(330px, calc(100vw - 36px));
  }
  .library-hero-quote,
  .library-hero-footer {
    display: none;
  }
  .library-hero-actions {
    align-items: flex-start;
    flex-direction: column;
    gap: 15px;
    margin-top: 25px;
  }
  .jump-back-section {
    padding-inline: 18px;
  }
  .showcase-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    padding-inline: 18px;
  }
  .showcase-card-2 {
    transform: none;
  }
  .library-footer {
    flex-basis: auto;
    min-height: 66px;
    padding-inline: 16px;
  }
  .library-update-status {
    display: none;
  }
}
@media (prefers-reduced-motion: reduce) {
  .showcase-card,
  .showcase-cover img,
  .library-play-button,
  .showcase-play {
    transition: none;
  }
}

/* Light theme: dock scroll arrows get white borders (accent border is too
   low-contrast against the light dock surface). */
[data-theme="light"] .dock-arrow,
[data-theme="light"] .dock-arrow:hover:not(:disabled) {
  border-color: #fff;
}
</style>
