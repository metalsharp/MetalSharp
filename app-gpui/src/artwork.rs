//! Library artwork resolution with the same candidate order as the Electron
//! LibraryView (`artworkCandidates`, `probeHeroArt`, `enrichArtwork`,
//! SteamGridDB lookups for Ubisoft titles). GPUI renders local files, so each
//! resolved image is cached under `~/.metalsharp/cache/gpui-artwork/`.
//!
//! Resolved art is recorded in `index.json` beside the files, so a relaunch
//! shows it immediately. Entries revalidate in the background without
//! blanking what is on screen: every session when a local source (custom grid
//! art, backend covers) won, weekly for remote art, and on candidate changes.
//! Misses are remembered for a day. Fetches run on a dedicated thread pool so
//! slow network chains never queue ahead of GPUI's own image decoding.
//!
//! Stored art is normalized for GPUI: SVG is rasterized, formats GPUI cannot
//! decode are converted with `sips`, and oversized stills are downscaled.
//! Animated GIF/WebP/APNG are kept byte-for-byte so they still animate.
use crate::live::Live;
use futures::channel::oneshot;
use gpui::{App, AppContext, Context, Entity, Global};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    collections::{HashMap, HashSet, VecDeque},
    io::Cursor,
    path::{Path, PathBuf},
    sync::{Condvar, Mutex, OnceLock},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ArtState {
    Pending,
    Ready(PathBuf),
    Missing,
}

/// One step in a candidate chain.
#[derive(Clone, Debug)]
pub enum Candidate {
    Url(String),
    /// Backend-relative grid art route (`/art/grid/<appid>/<kind>`).
    Backend(String),
    LocalFile(String),
    /// Steam store `appdetails` enrichment (main-process `steam:store-artwork`).
    StoreDetails {
        appid: u64,
        hero: bool,
    },
    /// SteamGridDB community art (main-process `steamgriddb:artwork`).
    SteamGridDb {
        name: String,
        hero: bool,
    },
}

/// Remote art is rechecked after a week; misses are retried after a day.
const REMOTE_TTL_SECS: u64 = 7 * 24 * 60 * 60;
const MISS_TTL_SECS: u64 = 24 * 60 * 60;
const FETCH_WORKERS: usize = 6;
const INDEX_VERSION: u32 = 1;
const INDEX_FILE: &str = "index.json";

/// Pixel budgets for stored stills. A budget (rather than a box) keeps wide
/// art used as a portrait card tall enough to crop: library_hero (3840x1240)
/// becomes ~1555x502 for cards.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ArtKind {
    Card,
    Hero,
}

impl ArtKind {
    fn for_key(key: &str) -> Self {
        if key.ends_with("-hero") {
            Self::Hero
        } else {
            Self::Card
        }
    }

    fn pixel_budget(self) -> u64 {
        match self {
            Self::Card => 720 * 1080,
            Self::Hero => 2560 * 1440,
        }
    }
}

const MAX_SIDE: u32 = 4096;

#[derive(Clone, Debug, Serialize, Deserialize)]
struct Entry {
    /// `None` records a miss.
    #[serde(default)]
    file: Option<PathBuf>,
    fetched: u64,
    /// Candidates without local files: a change refetches remote art.
    remote: u64,
    /// All candidates: a change retries a miss (e.g. an extracted EXE icon).
    full: u64,
    #[serde(default)]
    hash: u64,
    /// Won by a backend or local-file candidate: cheap to recheck each session.
    #[serde(default)]
    local: bool,
}

#[derive(Default, Serialize, Deserialize)]
struct Index {
    #[serde(default)]
    version: u32,
    #[serde(default)]
    entries: HashMap<String, Entry>,
    #[serde(default)]
    grid_signature: Option<Vec<(PathBuf, u128)>>,
}

struct Request {
    candidates: Vec<Candidate>,
    full: u64,
}

pub struct ArtCache {
    states: HashMap<String, ArtState>,
    requests: HashMap<String, Request>,
    /// Latest job per key; older results are discarded.
    jobs: HashMap<String, u64>,
    next_job: u64,
    /// Last ready tilted variant per `<card>-tilt<angle>` key.
    tilts: HashMap<String, PathBuf>,
    tilt_pending: HashSet<PathBuf>,
    index: Index,
    dir: Option<PathBuf>,
    save_pending: bool,
}

struct ArtGlobal(Entity<ArtCache>);
impl Global for ArtGlobal {}

pub fn install(cx: &mut App) -> Entity<ArtCache> {
    let cache = cx.new(|_| ArtCache {
        states: HashMap::new(),
        requests: HashMap::new(),
        jobs: HashMap::new(),
        next_job: 0,
        tilts: HashMap::new(),
        tilt_pending: HashSet::new(),
        index: Index::default(),
        dir: None,
        save_pending: false,
    });
    cx.set_global(ArtGlobal(cache.clone()));
    cache
}

pub fn cache(cx: &App) -> Option<Entity<ArtCache>> {
    cx.try_global::<ArtGlobal>().map(|global| global.0.clone())
}

pub fn steam_cdn(appid: u64, asset: &str) -> String {
    format!("https://cdn.cloudflare.steamstatic.com/steam/apps/{appid}/{asset}.jpg")
}

pub fn store_background(appid: u64) -> String {
    format!("https://store.akamai.steamstatic.com/images/storepagebackground/app/{appid}")
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;

/// FNV-1a: stable across builds, unlike `DefaultHasher`, so persisted
/// fingerprints survive app updates.
fn fnv(bytes: &[u8], mut hash: u64) -> u64 {
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

fn fingerprint(candidates: &[Candidate], include_local: bool) -> u64 {
    candidates
        .iter()
        .filter(|candidate| include_local || !matches!(candidate, Candidate::LocalFile(_)))
        .fold(FNV_OFFSET, |hash, candidate| {
            fnv(&[0xff], fnv(format!("{candidate:?}").as_bytes(), hash))
        })
}

#[derive(Clone, Copy)]
enum Priority {
    /// Nothing to show yet.
    Urgent,
    /// Cached art is on screen; refresh it when the pool is idle.
    Background,
}

struct Fetched {
    path: PathBuf,
    hash: u64,
    local: bool,
}

impl ArtCache {
    pub fn state(&self, key: &str) -> Option<&ArtState> {
        self.states.get(key)
    }

    /// Resolved local path, if any.
    pub fn path(&self, key: &str) -> Option<PathBuf> {
        match self.states.get(key) {
            Some(ArtState::Ready(path)) => Some(path.clone()),
            _ => None,
        }
    }

    /// Drop one entry so the next `resolve` refetches it (cover changed).
    pub fn forget(&mut self, key: &str, cx: &mut Context<Self>) {
        let tilt_prefix = format!("{key}-tilt");
        self.states.remove(key);
        self.requests.remove(key);
        self.jobs.remove(key);
        self.tilts.retain(|k, _| !k.starts_with(&tilt_prefix));
        if let Some(entry) = self.index.entries.remove(key) {
            if let Some(file) = entry.file {
                self.discard(&file);
            }
        }
        self.schedule_save(cx);
        cx.notify();
    }

    /// Refetch `key` with its last candidates while its current art stays on
    /// screen (custom grid art changed).
    pub fn revalidate(&mut self, key: &str, cx: &mut Context<Self>) {
        if self.requests.contains_key(key) {
            self.enqueue(key.to_owned(), Priority::Urgent, cx);
        } else if let Some(entry) = self.index.entries.get_mut(key) {
            // Not requested this session yet: make the next resolve recheck it.
            entry.fetched = 0;
            entry.local = true;
            self.schedule_save(cx);
        }
    }

    pub fn resolve(&mut self, key: String, candidates: Vec<Candidate>, cx: &mut Context<Self>) {
        let full = fingerprint(&candidates, true);
        if self
            .requests
            .get(&key)
            .is_some_and(|request| request.full == full)
        {
            return;
        }
        if self.ensure_loaded(cx).is_none() {
            return;
        }
        let remote = fingerprint(&candidates, false);
        self.requests
            .insert(key.clone(), Request { candidates, full });
        match self.states.get(&key) {
            // Candidates changed for art already resolved or in flight.
            Some(ArtState::Ready(_)) => return self.enqueue(key, Priority::Background, cx),
            Some(ArtState::Pending) => return self.enqueue(key, Priority::Urgent, cx),
            _ => {}
        }
        let now = unix_now();
        if let Some(entry) = self.index.entries.get(&key) {
            match &entry.file {
                Some(file) if file.is_file() => {
                    let stale = entry.local
                        || entry.remote != remote
                        || now.saturating_sub(entry.fetched) > REMOTE_TTL_SECS;
                    self.states
                        .insert(key.clone(), ArtState::Ready(file.clone()));
                    cx.notify();
                    if stale {
                        self.enqueue(key, Priority::Background, cx);
                    }
                    return;
                }
                None if entry.full == full && now.saturating_sub(entry.fetched) < MISS_TTL_SECS => {
                    self.states.insert(key, ArtState::Missing);
                    cx.notify();
                    return;
                }
                _ => {}
            }
        }
        self.states.insert(key.clone(), ArtState::Pending);
        self.enqueue(key, Priority::Urgent, cx);
    }

    /// Steam grid signature saved with the index, so art changed while the
    /// app was closed is still noticed on the next launch.
    pub fn saved_grid_signature(&mut self, cx: &mut Context<Self>) -> Option<Vec<(PathBuf, u128)>> {
        self.ensure_loaded(cx)?;
        self.index.grid_signature.clone()
    }

    pub fn save_grid_signature(&mut self, signature: Vec<(PathBuf, u128)>, cx: &mut Context<Self>) {
        if self.ensure_loaded(cx).is_none() {
            return;
        }
        if self.index.grid_signature.as_ref() != Some(&signature) {
            self.index.grid_signature = Some(signature);
            self.schedule_save(cx);
        }
    }

    fn ensure_loaded(&mut self, cx: &mut Context<Self>) -> Option<PathBuf> {
        if let Some(dir) = &self.dir {
            return Some(dir.clone());
        }
        let dir = Live::get(cx)?.home().join("cache").join("gpui-artwork");
        let index = std::fs::read(dir.join(INDEX_FILE))
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Index>(&bytes).ok())
            .filter(|index| index.version == INDEX_VERSION)
            .unwrap_or(Index {
                version: INDEX_VERSION,
                ..Index::default()
            });
        let referenced: HashSet<PathBuf> = index
            .entries
            .values()
            .filter_map(|entry| entry.file.clone())
            .collect();
        self.index = index;
        self.dir = Some(dir.clone());
        let started = SystemTime::now();
        cx.background_executor()
            .spawn(async move { sweep_orphans(&dir, &referenced, started) })
            .detach();
        self.dir.clone()
    }

    fn enqueue(&mut self, key: String, priority: Priority, cx: &mut Context<Self>) {
        let Some(request) = self.requests.get(&key) else {
            return;
        };
        let (Some(live), Some(dir)) = (Live::get(cx), self.dir.clone()) else {
            return;
        };
        self.next_job += 1;
        let job_id = self.next_job;
        self.jobs.insert(key.clone(), job_id);
        let known = self
            .index
            .entries
            .get(&key)
            .and_then(|entry| Some((entry.hash, entry.file.clone()?)));
        let remote = fingerprint(&request.candidates, false);
        let full = request.full;
        let (reply, result) = oneshot::channel();
        pool().push(
            Job {
                live,
                dir,
                kind: ArtKind::for_key(&key),
                key: key.clone(),
                candidates: request.candidates.clone(),
                known,
                reply,
            },
            priority,
        );
        cx.spawn(async move |this, cx| {
            let fetched = result.await.ok().flatten();
            let _ = this.update(cx, |cache, cx| {
                cache.finish(key, job_id, remote, full, fetched, cx)
            });
        })
        .detach();
    }

    fn finish(
        &mut self,
        key: String,
        job_id: u64,
        remote: u64,
        full: u64,
        fetched: Option<Fetched>,
        cx: &mut Context<Self>,
    ) {
        if self.jobs.get(&key) != Some(&job_id) {
            // Superseded (or forgotten) while in flight.
            if let Some(fetched) = fetched {
                if self.index.entries.get(&key).and_then(|e| e.file.as_ref()) != Some(&fetched.path)
                {
                    self.discard(&fetched.path);
                }
            }
            return;
        }
        self.jobs.remove(&key);
        let now = unix_now();
        match fetched {
            Some(fetched) => {
                let previous = self.index.entries.get(&key).and_then(|e| e.file.clone());
                if let Some(previous) = previous.filter(|p| *p != fetched.path) {
                    self.discard(&previous);
                }
                self.index.entries.insert(
                    key.clone(),
                    Entry {
                        file: Some(fetched.path.clone()),
                        fetched: now,
                        remote,
                        full,
                        hash: fetched.hash,
                        local: fetched.local,
                    },
                );
                self.states.insert(key, ArtState::Ready(fetched.path));
            }
            None if matches!(self.states.get(&key), Some(ArtState::Ready(_))) => {
                // A refresh failed (offline?): keep showing the cached art.
                if let Some(entry) = self.index.entries.get_mut(&key) {
                    entry.fetched = now;
                    entry.remote = remote;
                }
            }
            None => {
                self.index.entries.insert(
                    key.clone(),
                    Entry {
                        file: None,
                        fetched: now,
                        remote,
                        full,
                        hash: 0,
                        local: false,
                    },
                );
                self.states.insert(key, ArtState::Missing);
            }
        }
        self.schedule_save(cx);
        cx.notify();
    }

    /// Remove a cache file this module owns; local originals are left alone.
    fn discard(&self, path: &Path) {
        if self.dir.as_deref().is_some_and(|dir| path.starts_with(dir)) {
            let _ = std::fs::remove_file(path);
        }
    }

    fn schedule_save(&mut self, cx: &mut Context<Self>) {
        if self.save_pending || self.dir.is_none() {
            return;
        }
        self.save_pending = true;
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(800))
                .await;
            let Ok(Some((path, bytes))) = this.update(cx, |cache, _| {
                cache.save_pending = false;
                let bytes = serde_json::to_vec(&cache.index).ok()?;
                Some((cache.dir.as_ref()?.join(INDEX_FILE), bytes))
            }) else {
                return;
            };
            cx.background_executor()
                .spawn(async move {
                    let Some(dir) = path.parent() else { return };
                    let _ = std::fs::create_dir_all(dir);
                    let tmp = path.with_extension("json.tmp");
                    if std::fs::write(&tmp, &bytes).is_ok() {
                        let _ = std::fs::rename(&tmp, &path);
                    }
                })
                .await;
        })
        .detach();
    }
}

/// Delete files from earlier sessions that the index no longer references
/// (crashes, pre-index caches). Files newer than `started` belong to this
/// session's in-flight fetches and are kept.
fn sweep_orphans(dir: &Path, referenced: &HashSet<PathBuf>, started: SystemTime) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    let tilt_bases: HashSet<String> = referenced
        .iter()
        .map(|path| {
            format!(
                "{:016x}",
                fnv(path.to_string_lossy().as_bytes(), FNV_OFFSET)
            )
        })
        .collect();
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().into_owned();
        if name == INDEX_FILE || referenced.contains(&path) {
            continue;
        }
        let modified = entry.metadata().and_then(|m| m.modified()).ok();
        if modified.is_none_or(|time| time >= started) {
            continue;
        }
        let tilt_of_live_card = name.contains("-tilt")
            && name
                .rsplit_once('~')
                .and_then(|(_, rest)| rest.split_once('.'))
                .is_some_and(|(hash, _)| tilt_bases.contains(hash));
        if !tilt_of_live_card {
            let _ = std::fs::remove_file(path);
        }
    }
}

// ───────────────────────────── fetch pool ─────────────────────────────

struct Job {
    live: Live,
    dir: PathBuf,
    key: String,
    kind: ArtKind,
    candidates: Vec<Candidate>,
    /// Hash and file of the art currently shown; identical bytes reuse it.
    known: Option<(u64, PathBuf)>,
    reply: oneshot::Sender<Option<Fetched>>,
}

#[derive(Default)]
struct Queues {
    urgent: VecDeque<Job>,
    background: VecDeque<Job>,
}

struct Pool {
    queues: Mutex<Queues>,
    ready: Condvar,
}

impl Pool {
    fn push(&self, job: Job, priority: Priority) {
        let mut queues = self.queues.lock().unwrap();
        match priority {
            Priority::Urgent => queues.urgent.push_back(job),
            Priority::Background => queues.background.push_back(job),
        }
        drop(queues);
        self.ready.notify_one();
    }

    fn next(&self) -> Job {
        let mut queues = self.queues.lock().unwrap();
        loop {
            if let Some(job) = queues.urgent.pop_front() {
                return job;
            }
            if let Some(job) = queues.background.pop_front() {
                return job;
            }
            queues = self.ready.wait(queues).unwrap();
        }
    }
}

fn pool() -> &'static Pool {
    static POOL: OnceLock<&'static Pool> = OnceLock::new();
    POOL.get_or_init(|| {
        let pool: &'static Pool = Box::leak(Box::new(Pool {
            queues: Mutex::new(Queues::default()),
            ready: Condvar::new(),
        }));
        for n in 0..FETCH_WORKERS {
            let _ = std::thread::Builder::new()
                .name(format!("metalsharp-artwork-{n}"))
                .spawn(move || {
                    loop {
                        let job = pool.next();
                        let result = fetch_first(&job);
                        let _ = job.reply.send(result);
                    }
                });
        }
        pool
    })
}

fn http_agent() -> &'static ureq::Agent {
    static AGENT: OnceLock<ureq::Agent> = OnceLock::new();
    AGENT.get_or_init(|| {
        ureq::Agent::config_builder()
            .timeout_global(Some(Duration::from_secs(12)))
            .http_status_as_error(false)
            .build()
            .into()
    })
}

fn fetch_first(job: &Job) -> Option<Fetched> {
    let agent = http_agent();
    for candidate in &job.candidates {
        let (bytes, local) = match candidate {
            Candidate::Url(url) => (download(agent, url, None), false),
            Candidate::Backend(path) => (job.live.get_bytes(path, Duration::from_secs(10)), true),
            Candidate::LocalFile(path) => {
                let path = Path::new(path);
                let Ok(bytes) = std::fs::read(path) else {
                    continue;
                };
                if displayable_as_is(&bytes, job.kind) {
                    return Some(Fetched {
                        path: path.to_path_buf(),
                        hash: fnv(&bytes, FNV_OFFSET),
                        local: true,
                    });
                }
                (Some(bytes), true)
            }
            Candidate::StoreDetails { appid, hero } => (
                store_details(agent, *appid, *hero).and_then(|url| download(agent, &url, None)),
                false,
            ),
            Candidate::SteamGridDb { name, hero } => (
                steamgriddb(agent, &job.live.home(), name, *hero)
                    .and_then(|url| download(agent, &url, None)),
                false,
            ),
        };
        let Some(bytes) = bytes else { continue };
        let hash = fnv(&bytes, FNV_OFFSET);
        if let Some((known_hash, known_path)) = &job.known {
            if *known_hash == hash && known_path.is_file() {
                return Some(Fetched {
                    path: known_path.clone(),
                    hash,
                    local,
                });
            }
        }
        if let Some(path) = store(&job.dir, &job.key, &bytes, job.kind) {
            return Some(Fetched { path, hash, local });
        }
    }
    None
}

// ─────────────────────────── normalization ───────────────────────────

fn is_svg(bytes: &[u8]) -> bool {
    let head = &bytes[..bytes.len().min(1024)];
    let text = String::from_utf8_lossy(head);
    let text = text.trim_start_matches('\u{feff}').trim_start();
    (text.starts_with("<svg") || text.starts_with("<?xml") || text.starts_with("<!--"))
        && String::from_utf8_lossy(&bytes[..bytes.len().min(8192)]).contains("<svg")
}

/// HTML error pages and JSON bodies that a CDN served with a 200.
fn looks_like_text(bytes: &[u8]) -> bool {
    let head = &bytes[..bytes.len().min(256)];
    std::str::from_utf8(head)
        .map(|text| {
            let text = text.trim_start();
            text.starts_with('<') || text.starts_with('{') || text.starts_with('[')
        })
        .unwrap_or(false)
}

fn extension(format: image::ImageFormat) -> &'static str {
    use image::ImageFormat as F;
    match format {
        F::Jpeg => "jpg",
        F::Png => "png",
        F::WebP => "webp",
        F::Gif => "gif",
        F::Bmp => "bmp",
        F::Tiff => "tiff",
        _ => "png",
    }
}

/// Formats GPUI's `img` decodes itself.
fn gpui_native(format: image::ImageFormat) -> bool {
    use image::ImageFormat as F;
    matches!(
        format,
        F::Jpeg | F::Png | F::WebP | F::Gif | F::Bmp | F::Tiff
    )
}

fn is_animated(bytes: &[u8], format: image::ImageFormat) -> bool {
    use image::AnimationDecoder;
    match format {
        image::ImageFormat::Gif => image::codecs::gif::GifDecoder::new(Cursor::new(bytes))
            .map(|decoder| decoder.into_frames().take(2).count() > 1)
            .unwrap_or(false),
        image::ImageFormat::WebP => image::codecs::webp::WebPDecoder::new(Cursor::new(bytes))
            .map(|decoder| decoder.has_animation())
            .unwrap_or(false),
        image::ImageFormat::Png => image::codecs::png::PngDecoder::new(Cursor::new(bytes))
            .and_then(|decoder| decoder.is_apng())
            .unwrap_or(false),
        _ => false,
    }
}

fn within_budget(width: u32, height: u32, kind: ArtKind) -> bool {
    u64::from(width) * u64::from(height) <= kind.pixel_budget()
        && width <= MAX_SIDE
        && height <= MAX_SIDE
}

/// A local file GPUI can show directly, without a cached copy.
fn displayable_as_is(bytes: &[u8], kind: ArtKind) -> bool {
    let Ok(format) = image::guess_format(bytes) else {
        return false;
    };
    gpui_native(format)
        && (is_animated(bytes, format)
            || image::ImageReader::with_format(Cursor::new(bytes), format)
                .into_dimensions()
                .is_ok_and(|(w, h)| within_budget(w, h, kind)))
}

/// Bytes and extension to store, or `None` if this isn't usable artwork.
fn normalize(bytes: &[u8], kind: ArtKind) -> Option<(Vec<u8>, &'static str)> {
    if is_svg(bytes) {
        return rasterize_svg(bytes, kind).map(|png| (png, "png"));
    }
    if let Ok(format) = image::guess_format(bytes) {
        if gpui_native(format) {
            if is_animated(bytes, format) {
                return Some((bytes.to_vec(), extension(format)));
            }
            let fits = image::ImageReader::with_format(Cursor::new(bytes), format)
                .into_dimensions()
                .is_ok_and(|(w, h)| within_budget(w, h, kind));
            if fits {
                return Some((bytes.to_vec(), extension(format)));
            }
        }
        if let Ok(image) = image::load_from_memory_with_format(bytes, format) {
            return encode_still(image, kind);
        }
    }
    if looks_like_text(bytes) {
        return None;
    }
    // AVIF, HEIC, ICNS, PSD and friends: let ImageIO convert them.
    let png = convert_with_sips(bytes)?;
    encode_still(image::load_from_memory(&png).ok()?, kind)
}

fn encode_still(image: image::DynamicImage, kind: ArtKind) -> Option<(Vec<u8>, &'static str)> {
    use image::GenericImageView;
    let (width, height) = image.dimensions();
    if width == 0 || height == 0 {
        return None;
    }
    let image = if within_budget(width, height, kind) {
        image
    } else {
        let pixels = u64::from(width) * u64::from(height);
        let scale = (kind.pixel_budget() as f64 / pixels as f64)
            .sqrt()
            .min(f64::from(MAX_SIDE) / f64::from(width.max(height)));
        let w = ((f64::from(width) * scale).round() as u32).max(1);
        let h = ((f64::from(height) * scale).round() as u32).max(1);
        image.resize_exact(w, h, image::imageops::FilterType::Triangle)
    };
    let rgba = image.to_rgba8();
    let mut out = Vec::new();
    if rgba.pixels().any(|pixel| pixel.0[3] < 255) {
        rgba.write_to(&mut Cursor::new(&mut out), image::ImageFormat::Png)
            .ok()?;
        Some((out, "png"))
    } else {
        let rgb = image::DynamicImage::ImageRgba8(rgba).to_rgb8();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, 90)
            .encode_image(&rgb)
            .ok()?;
        Some((out, "jpg"))
    }
}

/// Render SVG art at the kind's pixel budget (GPUI would rasterize it at its
/// small intrinsic size, and the tilted dock cards need pixels anyway).
fn rasterize_svg(bytes: &[u8], kind: ArtKind) -> Option<Vec<u8>> {
    use resvg::{tiny_skia, usvg};
    static FONTS: OnceLock<std::sync::Arc<usvg::fontdb::Database>> = OnceLock::new();
    let fonts = FONTS.get_or_init(|| {
        let mut db = usvg::fontdb::Database::new();
        db.load_system_fonts();
        std::sync::Arc::new(db)
    });
    let options = usvg::Options {
        fontdb: fonts.clone(),
        ..usvg::Options::default()
    };
    let tree = usvg::Tree::from_data(bytes, &options).ok()?;
    let size = tree.size();
    let (width, height) = (f64::from(size.width()), f64::from(size.height()));
    if width <= 0.0 || height <= 0.0 {
        return None;
    }
    let scale = (kind.pixel_budget() as f64 / (width * height))
        .sqrt()
        .min(f64::from(MAX_SIDE) / width.max(height));
    let pw = ((width * scale).round() as u32).max(1);
    let ph = ((height * scale).round() as u32).max(1);
    let mut pixmap = tiny_skia::Pixmap::new(pw, ph)?;
    resvg::render(
        &tree,
        tiny_skia::Transform::from_scale(pw as f32 / width as f32, ph as f32 / height as f32),
        &mut pixmap.as_mut(),
    );
    pixmap.encode_png().ok()
}

fn convert_with_sips(bytes: &[u8]) -> Option<Vec<u8>> {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let base = std::env::temp_dir().join(format!("metalsharp-art-{}-{n}", std::process::id()));
    let input = base.with_extension("src");
    let output = base.with_extension("png");
    std::fs::write(&input, bytes).ok()?;
    let mut command = std::process::Command::new("/usr/bin/sips");
    command
        .args(["-s", "format", "png"])
        .arg(&input)
        .arg("--out")
        .arg(&output)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::lifecycle::unmask_child_signals(&mut command);
    let converted = command
        .status()
        .ok()
        .filter(|status| status.success())
        .and_then(|_| std::fs::read(&output).ok());
    let _ = std::fs::remove_file(&input);
    let _ = std::fs::remove_file(&output);
    converted
}

/// GPUI caches decoded images by path, so every stored version gets a fresh
/// file name (`<key>~<n>.<ext>`); the index drops the previous one.
fn versioned_path(dir: &Path, key: &str, ext: &str) -> PathBuf {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    dir.join(format!("{}~{stamp}{n}.{ext}", sanitize(key)))
}

fn store(dir: &Path, key: &str, bytes: &[u8], kind: ArtKind) -> Option<PathBuf> {
    let (bytes, ext) = normalize(bytes, kind)?;
    std::fs::create_dir_all(dir).ok()?;
    let path = versioned_path(dir, key, ext);
    let tmp = path.with_extension(format!("{ext}.tmp"));
    std::fs::write(&tmp, &bytes).ok()?;
    std::fs::rename(&tmp, &path).ok()?;
    Some(path)
}

fn download(agent: &ureq::Agent, url: &str, auth: Option<&str>) -> Option<Vec<u8>> {
    if !url.starts_with("https://") && !url.starts_with("http://") {
        return None;
    }
    let mut request = agent.get(url);
    if let Some(auth) = auth {
        request = request.header("Authorization", format!("Bearer {auth}"));
    }
    let mut response = request.call().ok()?;
    if response.status().as_u16() != 200 {
        return None;
    }
    response
        .body_mut()
        .with_config()
        .limit(24 * 1024 * 1024)
        .read_to_vec()
        .ok()
}

fn sanitize(key: &str) -> String {
    key.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect()
}

fn store_details(agent: &ureq::Agent, appid: u64, hero: bool) -> Option<String> {
    let url = format!("https://store.steampowered.com/api/appdetails?appids={appid}&l=english");
    let bytes = download(agent, &url, None)?;
    let payload: Value = serde_json::from_slice(&bytes).ok()?;
    let entry = payload.get(appid.to_string())?;
    if entry.get("success").and_then(Value::as_bool) != Some(true) {
        return None;
    }
    let data = entry.get("data")?;
    let safe = |value: Option<&Value>| {
        value
            .and_then(Value::as_str)
            .filter(|url| {
                url::Url::parse(url).is_ok_and(|u| {
                    u.scheme() == "https"
                        && u.host_str()
                            .is_some_and(|h| h.ends_with(".steamstatic.com"))
                })
            })
            .map(str::to_owned)
    };
    let header = safe(data.get("header_image"));
    let screenshot = safe(
        data.get("screenshots")
            .and_then(Value::as_array)
            .and_then(|shots| shots.iter().find(|shot| shot.get("path_full").is_some()))
            .and_then(|shot| shot.get("path_full")),
    );
    if hero {
        safe(data.get("background_raw")).or(screenshot).or(header)
    } else {
        header.or(screenshot)
    }
}

fn steamgriddb_key(home: &Path) -> Option<String> {
    let files = [
        std::env::var_os("HOME").map(|h| {
            PathBuf::from(h)
                .join("Library/Application Support/dev.tormak.steam-art-manager/settings.json")
        }),
        Some(home.join("config/steam-art-manager-settings.json")),
    ];
    for file in files.into_iter().flatten() {
        if let Ok(text) = std::fs::read_to_string(file) {
            if let Ok(value) = serde_json::from_str::<Value>(&text) {
                if let Some(key) = value
                    .get("steamGridDbApiKey")
                    .and_then(Value::as_str)
                    .map(str::trim)
                    .filter(|k| !k.is_empty())
                {
                    return Some(key.to_owned());
                }
            }
        }
    }
    None
}

fn normalize_name(value: &str) -> String {
    let lower = value.to_lowercase();
    let mut out = String::new();
    let mut space = false;
    for c in lower.chars() {
        if c.is_ascii_alphanumeric() {
            if space && !out.is_empty() {
                out.push(' ');
            }
            space = false;
            out.push(c);
        } else {
            space = true;
        }
    }
    out
}

fn steamgriddb(agent: &ureq::Agent, home: &Path, name: &str, hero: bool) -> Option<String> {
    let key = steamgriddb_key(home)?;
    let fetch = |url: &str| -> Vec<Value> {
        download(agent, url, Some(&key))
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
            .filter(|payload| payload.get("success").and_then(Value::as_bool) == Some(true))
            .and_then(|payload| payload.get("data").and_then(Value::as_array).cloned())
            .unwrap_or_default()
    };
    let query: String = url::form_urlencoded::byte_serialize(name.trim().as_bytes()).collect();
    let games = fetch(&format!(
        "https://www.steamgriddb.com/api/v2/search/autocomplete/{}",
        query.replace('+', "%20")
    ));
    let wanted = normalize_name(name);
    if wanted.is_empty() {
        return None;
    }
    let matched = games
        .iter()
        .find(|game| {
            game.get("name").and_then(Value::as_str).map(normalize_name) == Some(wanted.clone())
        })
        .or_else(|| {
            games.iter().find(|game| {
                game.get("name")
                    .and_then(Value::as_str)
                    .is_some_and(|candidate| {
                        let candidate = normalize_name(candidate);
                        candidate.starts_with(&wanted) || wanted.starts_with(&candidate)
                    })
            })
        })?;
    let id = matched.get("id").and_then(Value::as_u64)?;
    let kind = if hero { "heroes" } else { "grids" };
    let items = fetch(&format!(
        "https://www.steamgriddb.com/api/v2/{kind}/game/{id}?types=static&nsfw=false&humor=false"
    ));
    let ideal: f64 = if hero { 3.1 } else { 2.0 / 3.0 };
    let mut ranked: Vec<(f64, String)> = items
        .iter()
        .filter_map(|item| {
            let url = item.get("url").and_then(Value::as_str)?;
            let parsed = url::Url::parse(url).ok()?;
            let host = parsed.host_str()?;
            let allowed = parsed.scheme() == "https"
                && (host == "cdn2.steamgriddb.com"
                    || host == "cdn.steamgriddb.com"
                    || (host == "s3.amazonaws.com" && parsed.path().starts_with("/steamgriddb/")));
            if !allowed {
                return None;
            }
            let w = item.get("width").and_then(Value::as_f64).unwrap_or(0.0);
            let h = item.get("height").and_then(Value::as_f64).unwrap_or(0.0);
            let ratio = if h > 0.0 { w / h } else { 0.0 };
            Some(((ratio.max(0.01) / ideal).ln().abs(), url.to_owned()))
        })
        .collect();
    ranked.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
    ranked.into_iter().next().map(|(_, url)| url)
}

/// Port of `generate-dock-art.swift`: rotate a 360x486 rounded card (aspect-fill
/// cover) by `angle` degrees inside a transparent 504x630 canvas with a 2 px
/// gray outline. Positive angles tilt clockwise, matching the bundled variants.
pub fn render_tilted_card(source: &Path, angle: i32, output: &Path) -> Option<()> {
    use image::{Rgba, RgbaImage};
    let src = image::open(source).ok()?.to_rgba8();
    let (sw, sh) = src.dimensions();
    if sw == 0 || sh == 0 {
        return None;
    }
    let (cw, ch) = (360.0f32, 486.0f32);
    // Aspect fill into the card rectangle.
    let scale = (cw / sw as f32).max(ch / sh as f32);
    let (dw, dh) = (sw as f32 * scale, sh as f32 * scale);
    let (ox, oy) = ((dw - cw) / 2.0, (dh - ch) / 2.0);
    let (w, h) = (504u32, 630u32);
    let (cx, cy) = (w as f32 / 2.0, h as f32 / 2.0);
    let theta = (angle as f32).to_radians();
    let (sin, cos) = theta.sin_cos();
    let radius = 12.0f32;
    let mut out = RgbaImage::new(w, h);
    let sample = |x: f32, y: f32| -> [f32; 4] {
        let x = x.clamp(0.0, sw as f32 - 1.0);
        let y = y.clamp(0.0, sh as f32 - 1.0);
        let (x0, y0) = (x.floor() as u32, y.floor() as u32);
        let (x1, y1) = ((x0 + 1).min(sw - 1), (y0 + 1).min(sh - 1));
        let (fx, fy) = (x - x0 as f32, y - y0 as f32);
        let p = |xx, yy| src.get_pixel(xx, yy).0;
        let (a, b, c, d) = (p(x0, y0), p(x1, y0), p(x0, y1), p(x1, y1));
        let mut result = [0.0; 4];
        for i in 0..4 {
            let top = a[i] as f32 * (1.0 - fx) + b[i] as f32 * fx;
            let bottom = c[i] as f32 * (1.0 - fx) + d[i] as f32 * fx;
            result[i] = top * (1.0 - fy) + bottom * fy;
        }
        result
    };
    // Signed distance to the rounded rectangle (negative inside).
    let rounded_sd = |x: f32, y: f32| -> f32 {
        let qx = x.abs() - (cw / 2.0 - radius);
        let qy = y.abs() - (ch / 2.0 - radius);
        let outside = (qx.max(0.0).powi(2) + qy.max(0.0).powi(2)).sqrt();
        outside + qx.max(qy).min(0.0) - radius
    };
    let _ = src.dimensions();
    for py in 0..h {
        for px in 0..w {
            // Card-local coordinates via inverse rotation (y-down image space).
            let (dx, dy) = (px as f32 + 0.5 - cx, py as f32 + 0.5 - cy);
            let lx = dx * cos + dy * sin;
            let ly = -dx * sin + dy * cos;
            let sd = rounded_sd(lx, ly);
            if sd > 1.5 {
                continue;
            }
            let coverage = (0.5 - sd).clamp(0.0, 1.0);
            let sxp = (lx + cw / 2.0 + ox) / scale;
            let syp = (ly + ch / 2.0 + oy) / scale;
            let mut color = sample(sxp, syp);
            // 2 px outline centred on the edge: 40% white at 80% alpha.
            let stroke = (1.0 - (sd.abs() - 1.0).max(0.0)).clamp(0.0, 1.0) * 0.8;
            for i in 0..3 {
                color[i] = color[i] * (1.0 - stroke) + 102.0 * stroke;
            }
            let alpha = (color[3] / 255.0) * coverage.max(stroke);
            out.put_pixel(
                px,
                py,
                Rgba([
                    color[0] as u8,
                    color[1] as u8,
                    color[2] as u8,
                    (alpha * 255.0).round() as u8,
                ]),
            );
        }
    }
    let tmp = output.with_extension("png.tmp");
    out.save_with_format(&tmp, image::ImageFormat::Png).ok()?;
    std::fs::rename(&tmp, output).ok()?;
    Some(())
}

impl ArtCache {
    /// Tilted dock variant of a resolved card image, generated in the
    /// background. The file name carries a hash of the card file, so a
    /// relaunch reuses it and a new cover regenerates it; until then the
    /// previous variant stays on screen.
    pub fn tilted(
        &mut self,
        card_key: &str,
        angle: i32,
        cx: &mut Context<Self>,
    ) -> Option<PathBuf> {
        let base = self.path(card_key)?;
        let dir = self.dir.clone()?;
        let key = format!("{card_key}-tilt{angle}");
        let stem = format!("{}-tilt{angle}", sanitize(card_key));
        let output = dir.join(format!(
            "{stem}~{:016x}.png",
            fnv(base.to_string_lossy().as_bytes(), FNV_OFFSET)
        ));
        if self.tilts.get(&key) == Some(&output) {
            return Some(output);
        }
        if output.is_file() {
            self.tilts.insert(key, output.clone());
            return Some(output);
        }
        if self.tilt_pending.insert(output.clone()) {
            let tilt_key = key.clone();
            cx.spawn(async move |this, cx| {
                let rendered = output.clone();
                let ok = cx
                    .background_executor()
                    .spawn(async move {
                        std::fs::create_dir_all(&dir).ok()?;
                        render_tilted_card(&base, angle, &rendered)?;
                        remove_other_tilts(&dir, &stem, &rendered);
                        Some(())
                    })
                    .await
                    .is_some();
                let _ = this.update(cx, |cache, cx| {
                    cache.tilt_pending.remove(&output);
                    if ok {
                        cache.tilts.insert(tilt_key, output);
                        cx.notify();
                    }
                });
            })
            .detach();
        }
        self.tilts.get(&key).cloned()
    }
}

fn remove_other_tilts(dir: &Path, stem: &str, keep: &Path) {
    let prefix = format!("{stem}~");
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path != keep && entry.file_name().to_string_lossy().starts_with(&prefix) {
            let _ = std::fs::remove_file(path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{ArtKind, Candidate, fingerprint, normalize};
    use std::io::Cursor;

    fn png(width: u32, height: u32, alpha: u8) -> Vec<u8> {
        let image = image::RgbaImage::from_pixel(width, height, image::Rgba([40, 90, 200, alpha]));
        let mut out = Vec::new();
        image
            .write_to(&mut Cursor::new(&mut out), image::ImageFormat::Png)
            .unwrap();
        out
    }

    fn dimensions(bytes: &[u8]) -> (u32, u32) {
        image::ImageReader::new(Cursor::new(bytes))
            .with_guessed_format()
            .unwrap()
            .into_dimensions()
            .unwrap()
    }

    #[test]
    fn small_native_art_is_stored_byte_for_byte() {
        let bytes = png(600, 900, 255);
        let (stored, ext) = normalize(&bytes, ArtKind::Card).unwrap();
        assert_eq!(ext, "png");
        assert_eq!(stored, bytes);
    }

    #[test]
    fn oversized_art_is_downscaled_to_the_kind_budget() {
        let (stored, ext) = normalize(&png(3840, 1240, 255), ArtKind::Card).unwrap();
        assert_eq!(ext, "jpg", "opaque stills re-encode as JPEG");
        let (w, h) = dimensions(&stored);
        assert!(u64::from(w) * u64::from(h) <= ArtKind::Card.pixel_budget());
        assert!(
            h >= 480,
            "wide art keeps enough height to crop a card: {w}x{h}"
        );
        let (stored, ext) = normalize(&png(3000, 4500, 128), ArtKind::Card).unwrap();
        assert_eq!(ext, "png", "transparency survives");
        let (w, h) = dimensions(&stored);
        assert!(u64::from(w) * u64::from(h) <= ArtKind::Card.pixel_budget());
    }

    #[test]
    fn svg_art_is_rasterized() {
        let svg = br##"<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" width="60" height="90" viewBox="0 0 60 90">
  <rect width="60" height="90" fill="#3d7bff"/><circle cx="30" cy="45" r="20" fill="#fff"/>
</svg>"##;
        let (stored, ext) = normalize(svg, ArtKind::Card).unwrap();
        assert_eq!(ext, "png");
        let (w, h) = dimensions(&stored);
        assert!(w >= 600 && h >= 900, "rendered at card resolution: {w}x{h}");
    }

    #[test]
    fn animated_gif_is_kept_so_it_still_animates() {
        use image::codecs::gif::{GifEncoder, Repeat};
        let mut bytes = Vec::new();
        {
            let mut encoder = GifEncoder::new(&mut bytes);
            encoder.set_repeat(Repeat::Infinite).unwrap();
            for shade in [0u8, 255] {
                let frame =
                    image::RgbaImage::from_pixel(4000, 300, image::Rgba([shade, 0, 0, 255]));
                encoder.encode_frame(image::Frame::new(frame)).unwrap();
            }
        }
        let (stored, ext) = normalize(&bytes, ArtKind::Card).unwrap();
        assert_eq!(ext, "gif");
        assert_eq!(stored, bytes);
    }

    #[test]
    fn error_pages_are_not_artwork() {
        assert!(
            normalize(
                b"<!DOCTYPE html><html><body>404</body></html>",
                ArtKind::Card
            )
            .is_none()
        );
        assert!(normalize(br#"{"success":false}"#, ArtKind::Hero).is_none());
    }

    #[test]
    fn fingerprints_ignore_local_files_only_when_asked() {
        let remote = vec![Candidate::Url("https://example.com/a.jpg".into())];
        let mut with_icon = remote.clone();
        with_icon.push(Candidate::LocalFile("/tmp/icon.png".into()));
        assert_eq!(fingerprint(&remote, false), fingerprint(&with_icon, false));
        assert_ne!(fingerprint(&remote, true), fingerprint(&with_icon, true));
        // Persisted across launches: must not change between builds.
        assert_eq!(fingerprint(&[], true), super::FNV_OFFSET);
    }

    #[test]
    fn tilted_card_matches_bundled_swift_variant_orientation() {
        let assets = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("assets");
        let reference = assets.join("dock/hades-6.png");
        if !reference.exists() {
            return; // generated by `swift generate-dock-art.swift`
        }
        let out = std::env::temp_dir().join(format!("metalsharp-tilt-{}.png", std::process::id()));
        super::render_tilted_card(&assets.join("hades.jpg"), 6, &out).unwrap();
        let a = image::open(&out).unwrap().to_rgba8();
        let b = image::open(&reference).unwrap().to_rgba8();
        assert_eq!(a.dimensions(), b.dimensions());
        // Alpha masks (card silhouette) must line up: same rotation direction.
        let mismatched = a
            .pixels()
            .zip(b.pixels())
            .filter(|(p, q)| (p.0[3] > 128) != (q.0[3] > 128))
            .count();
        let total = (a.width() * a.height()) as usize;
        let _ = std::fs::remove_file(&out);
        assert!(
            mismatched * 100 < total * 2,
            "silhouette mismatch {mismatched}/{total}"
        );
    }
}
