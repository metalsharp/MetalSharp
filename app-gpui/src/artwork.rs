//! Library artwork resolution with the same candidate order as the Electron
//! LibraryView (`artworkCandidates`, `probeHeroArt`, `enrichArtwork`,
//! SteamGridDB lookups for Ubisoft titles). GPUI renders local files, so each
//! resolved image is cached under `~/.metalsharp/cache/gpui-artwork/`.
use crate::live::Live;
use gpui::{App, AppContext, Context, Entity, Global};
use serde_json::Value;
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    time::Duration,
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

pub struct ArtCache {
    states: HashMap<String, ArtState>,
    generation: u64,
}

struct ArtGlobal(Entity<ArtCache>);
impl Global for ArtGlobal {}

pub fn install(cx: &mut App) -> Entity<ArtCache> {
    let cache = cx.new(|_| ArtCache {
        states: HashMap::new(),
        generation: 0,
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

    /// Forget everything (Electron's `grid-art:changed` cache bust).
    pub fn invalidate(&mut self, cx: &mut Context<Self>) {
        self.states.clear();
        self.generation += 1;
        cx.notify();
    }

    /// Drop one entry so the next `resolve` refetches it (cover changed).
    pub fn forget(&mut self, key: &str, cx: &mut Context<Self>) {
        let tilt_prefix = format!("{key}-tilt");
        let before = self.states.len();
        self.states
            .retain(|k, _| k != key && !k.starts_with(&tilt_prefix));
        if self.states.len() != before {
            cx.notify();
        }
    }

    pub fn resolve(&mut self, key: String, candidates: Vec<Candidate>, cx: &mut Context<Self>) {
        if self.states.contains_key(&key) {
            return;
        }
        let Some(live) = Live::get(cx) else { return };
        self.states.insert(key.clone(), ArtState::Pending);
        let generation = self.generation;
        let dir = live.home().join("cache").join("gpui-artwork");
        cx.spawn(async move |this, cx| {
            let key_for_fetch = key.clone();
            let result = cx
                .background_executor()
                .spawn(async move { fetch_first(&live, &dir, &key_for_fetch, &candidates) })
                .await;
            let _ = this.update(cx, |cache, cx| {
                if cache.generation != generation {
                    return;
                }
                cache.states.insert(
                    key,
                    result.map(ArtState::Ready).unwrap_or(ArtState::Missing),
                );
                cx.notify();
            });
        })
        .detach();
    }
}

fn http_agent() -> ureq::Agent {
    ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(12)))
        .http_status_as_error(false)
        .build()
        .into()
}

fn image_extension(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(&[0xff, 0xd8, 0xff]) {
        Some("jpg")
    } else if bytes.starts_with(b"\x89PNG") {
        Some("png")
    } else if bytes.len() > 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some("webp")
    } else if bytes.starts_with(b"GIF8") {
        Some("gif")
    } else {
        None
    }
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

/// GPUI caches decoded images by path, so every refetch gets a fresh file name
/// (`<key>~<n>.<ext>`) and older versions of that key are removed.
fn versioned_path(dir: &Path, key: &str, ext: &str) -> PathBuf {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    dir.join(format!("{}~{stamp}{n}.{ext}", sanitize(key)))
}

fn remove_old_versions(dir: &Path, key: &str, keep: &Path) {
    let prefix = format!("{}~", sanitize(key));
    let legacy = sanitize(key);
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path == keep {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        let stem = name
            .rsplit_once('.')
            .map_or(name.as_str(), |(stem, _)| stem);
        if name.starts_with(&prefix) || stem == legacy {
            let _ = std::fs::remove_file(path);
        }
    }
}

fn store(dir: &Path, key: &str, bytes: &[u8]) -> Option<PathBuf> {
    let ext = image_extension(bytes)?;
    std::fs::create_dir_all(dir).ok()?;
    let path = versioned_path(dir, key, ext);
    let tmp = dir.join(format!(".{}.{ext}.tmp", sanitize(key)));
    std::fs::write(&tmp, bytes).ok()?;
    std::fs::rename(&tmp, &path).ok()?;
    remove_old_versions(dir, key, &path);
    Some(path)
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

fn fetch_first(live: &Live, dir: &Path, key: &str, candidates: &[Candidate]) -> Option<PathBuf> {
    let agent = http_agent();
    for candidate in candidates {
        let bytes = match candidate {
            Candidate::Url(url) => download(&agent, url, None),
            Candidate::Backend(path) => live.get_bytes(path, Duration::from_secs(10)),
            Candidate::LocalFile(path) => {
                let path = Path::new(path);
                if path.is_file() {
                    if let Ok(bytes) = std::fs::read(path) {
                        if image_extension(&bytes).is_some() {
                            return Some(path.to_path_buf());
                        }
                    }
                }
                None
            }
            Candidate::StoreDetails { appid, hero } => {
                store_details(&agent, *appid, *hero).and_then(|url| download(&agent, &url, None))
            }
            Candidate::SteamGridDb { name, hero } => steamgriddb(&agent, &live.home(), name, *hero)
                .and_then(|url| download(&agent, &url, None)),
        };
        if let Some(bytes) = bytes {
            if let Some(path) = store(dir, key, &bytes) {
                return Some(path);
            }
        }
    }
    None
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
    /// Tilted dock variant of a resolved card image; generated in the background.
    pub fn tilted(
        &mut self,
        card_key: &str,
        angle: i32,
        cx: &mut Context<Self>,
    ) -> Option<PathBuf> {
        let base = self.path(card_key)?;
        let key = format!("{card_key}-tilt{angle}");
        match self.states.get(&key) {
            Some(ArtState::Ready(path)) => return Some(path.clone()),
            Some(_) => return None,
            None => {}
        }
        let dir = Live::get(cx)?.home().join("cache").join("gpui-artwork");
        let output = versioned_path(&dir, &key, "png");
        self.states.insert(key.clone(), ArtState::Pending);
        let generation = self.generation;
        let tilt_key = key.clone();
        cx.spawn(async move |this, cx| {
            let result = cx
                .background_executor()
                .spawn(async move {
                    std::fs::create_dir_all(&dir).ok()?;
                    render_tilted_card(&base, angle, &output)?;
                    remove_old_versions(&dir, &tilt_key, &output);
                    Some(output)
                })
                .await;
            let _ = this.update(cx, |cache, cx| {
                if cache.generation == generation {
                    cache.states.insert(
                        key,
                        result.map(ArtState::Ready).unwrap_or(ArtState::Missing),
                    );
                    cx.notify();
                }
            });
        })
        .detach();
        None
    }
}

#[cfg(test)]
mod tests {
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
