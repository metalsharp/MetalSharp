// Linux Steam, the FEX build's experimental Steam client: the contract between MetalSharp and MetalSharp Wine's
// metalsharp-steam command and bridge daemon, and what keeps the regular build and Windows Steam mode as they were.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repo = path.resolve(__dirname, '..', '..');
const read = (relative) => fs.readFileSync(path.join(repo, relative), 'utf8');

const linux = read('app/src-c/runtime/linux_steam.c');
const header = read('app/src-c/include/metalsharp_backend/linux_steam.h');
const config = read('app/src-c/runtime/config.c');
const actions = read('app/src-c/runtime/steam_actions.c');
const steam = read('app/src-c/runtime/steam.c');
const game = read('app/src-c/runtime/game.c');
const migration = read('app/src-c/runtime/migration.c');
const wineRuntime = read('app/src-c/runtime/wine_runtime.c');
const backend = read('app/src-c/runtime/backend.c');
const updater = read('app/updater/update.sh');
const release = read('.github/workflows/release.yml');
const shutdown = read('app-gpui/src/wine_shutdown.rs');
const live = read('app-gpui/src/live.rs');
const settings = read('app-gpui/src/settings_preview.rs');

test('the Steam client setting is steamClient, windows unless it says linux', () => {
  assert.match(config, /"steamClient"/);
  assert.match(config, /const char\* ms_config_steam_client\(const char\* metalsharp_home\)/);
  assert.match(config, /return linux_client \? "linux" : "windows";/);
  // Linux mode is the fex build's only: the regular build never takes it
  assert.match(linux, /bool ms_linux_steam_mode\(const char\* home\) \{\n\s+return home && ms_wine_is_fex\(\) && !strcmp\(ms_config_steam_client\(home\), "linux"\)/);
  assert.match(linux, /bool ms_linux_steam_available\(const char\* home\) \{\n\s+char command\[PATH_MAX\];\n\s+return ms_wine_is_fex\(\)/);
});

test('metalsharp-steam is MetalSharp Wine\'s, run with MetalSharp\'s folders', () => {
  assert.match(linux, /"%s\/share\/steamvm\/metalsharp-steam", contents/);
  assert.match(linux, /"%s\/linux-steam", home[\s\S]*?"METALSHARP_VM_DIR"/);
  assert.match(linux, /"%s\/run\/linux-steam", home[\s\S]*?"METALSHARP_VM_RUN"/);
  assert.match(linux, /setenv\("METALSHARP_STEAM_PREFIX", path, 1\);\n\s+setenv\("METALSHARP_GAME_PREFIX", path, 1\);/);
  assert.match(linux, /"%s\/MacOS\/wine", contents\)\)\n\s+setenv\("METALSHARP_WINE", path, 1\);/);
  // no terminal, a session of its own
  assert.match(linux, /open\("\/dev\/null", O_RDONLY\)/);
  assert.match(linux, /\(void\)setsid\(\);/);
  for (const command of ['"install"', '"start"', '"show"', '"stop"', '"status", "--json"', '"url", url']) {
    assert.ok(linux.includes(command), command);
  }
  // the stand-in command is for the tests only
  assert.match(linux, /\/\* for the tests only: a stand-in command \*\/\n\s+const char\* override = getenv\("METALSHARP_LINUX_STEAM_COMMAND"\);/);
});

test('a Play MetalSharp starts names its lane to the bridge daemon', () => {
  for (const [lane, backendName] of [['dxmt', 'mullion'], ['dxmt_32', 'mullion'], ['vkd3d', 'vkd3d'], ['dxvk', 'dxvk'], ['wine_bare', 'wine']]) {
    assert.match(linux, new RegExp(`!strcmp\\(lane, "${lane}"\\)[^\\n]*\\n\\s+return "${backendName}";`), lane);
  }
  assert.match(linux, /"%s\/run\/linux-steam\/launch-intent", home/);
  assert.match(linux, /mkdir\(path, 0700\)/);
  assert.match(linux, /O_WRONLY \| O_CREAT \| O_EXCL \| O_NOFOLLOW \| O_CLOEXEC, 0600/);
  assert.match(linux, /rename\(staging, path\)/);
  // the intent goes before the link, and a failed link takes it back
  assert.ok(linux.indexOf('ms_linux_steam_write_intent(home, appid, lane)') < linux.indexOf('run_command(home, args, URL_TIMEOUT_SECONDS'));
  assert.match(linux, /if \(intent\)\n\s+ms_linux_steam_clear_intent\(home, appid\);/);
});

test('Start Steam, Stop and Play go through Linux Steam in Linux mode only', () => {
  assert.match(actions, /char\* ms_steam_launch_json\(const char\* home, int\* status\) \{\n\s+if \(ms_linux_steam_mode\(home\)\)\n\s+return ms_linux_steam_launch_json\(home, status\);/);
  assert.match(actions, /if \(ms_linux_steam_mode\(home\)\)\n\s+linux_running = !ms_linux_steam_stop\(home, 60\);/);
  assert.match(actions, /if \(ms_linux_steam_mode\(home\)\)\n\s+return ms_linux_steam_client_running\(home\);/);
  assert.match(actions, /if \(ms_linux_steam_mode\(home\)\)\n\s+return launch_game_via_linux_steam\(home, id, pipeline, url, status, launched_pid\);/);
  assert.match(actions, /ms_linux_steam_ensure_running\(home, 90\)/);
  // no Windows Steam for direct launches, no pipeline marker, never the Windows Steam installer
  assert.match(actions, /static char\* ensure_wine_steam_running\([^)]*\) \{[\s\S]{0,220}if \(ms_linux_steam_mode\(home\)\)\n\s+return NULL;/);
  assert.match(actions, /Linux Steam's games get their backend from the launch intent[^\n]*\n\s+if \(ms_linux_steam_mode\(home\)\)\n\s+return NULL;/);
  assert.match(actions, /char\* ms_steam_install_json\(const char\* home, int\* status\) \{[\s\S]{0,300}if \(ms_linux_steam_mode\(home\)\) \{\n\s+if \(status\)\n\s+\*status = 409;/);
  // the default fex Play passes its lane
  assert.match(actions, /result = launch_game_via_steam_args_json\(home, id, NULL, pipeline, &steam_status, &steam_pid\);/);
  // a client switch stops the client that runs now
  assert.match(backend, /ms_steam_client_switch_prepare\(context->metalsharp_home[\s\S]{0,120}ms_config_set_json/);
});

test('Linux Steam\'s library is a Steam library everywhere the Steam roots are scanned', () => {
  assert.match(header, /#define MS_LINUX_STEAM_LIBRARY_STEAMAPPS "prefix-steam\/drive_c\/SteamLibrary\/steamapps"/);
  assert.match(steam, /const char\* own\[\] = \{"prefix-steam\/drive_c\/Program Files \(x86\)\/Steam\/steamapps", MS_LINUX_STEAM_LIBRARY_STEAMAPPS\};/);
  // library, game folder, native app and watch
  assert.equal((steam.match(/collect_known_steam_roots\(metalsharp_home, &games, &count, &capacity\);/g) || []).length, 4);
  assert.ok(!steam.includes('join_path(metalsharp_home, "prefix-steam/drive_c/Program Files (x86)/Steam/steamapps")'));
  assert.match(actions, /add_steamapps_candidate\(candidates, &count, max, linux_library\);/);
  assert.match(game, /MS_LINUX_STEAM_LIBRARY_STEAMAPPS/);
});

test('quit, updates and migrations stop the virtual machine with its command and never kill it', () => {
  assert.match(migration, /if \(ms_wine_is_fex\(\) && !ms_linux_steam_stop_if_running\(home, 60\)\)\n\s+return false;/);
  assert.match(updater, /stop_steam_and_wine\(\) \{\n\s+stop_linux_steam\n/);
  assert.match(updater, /"\$command" stop <\/dev\/null/);
  assert.match(shutdown, /if crate::linux_steam::is_vm_executable\(executable\) \{\n\s+return false;/);
  assert.match(live, /if crate::variant::is_fex\(\) \{\n\s+crate::linux_steam::stop_if_running\(/);
});

test('no prefix update at Play: the Steam prefix is updated after a Wine swap, and a newer Wine is kept', () => {
  assert.match(wineRuntime, /const char\* const boot\[\] = \{wine, "wineboot", "-u", "-r", NULL\};[\s\S]{0,200}run_wait\(wait_server, prefix, log, 300\)/);
  assert.match(wineRuntime, /if \(replaced\)\n\s+update_steam_prefix\(home\);/);
  assert.match(wineRuntime, /ms_wine_compare_versions\(installed_version, carried_version\) == 1/);
  assert.match(wineRuntime, /prefix_wineserver_running\(prefix\)/);
});

test('MetalSharp Wine 7.0-pre1 is the FEX build\'s release; the regular build keeps its own', () => {
  assert.match(wineRuntime, /#define MS_WINE_RELEASE_VERSION "7\.0-pre1"/);
  assert.match(wineRuntime, /#define MS_WINE_X86_64_RELEASE_VERSION "6\.0"/);
  const tags = [...release.matchAll(/METALSHARP_WINE_TAG: (\S+)/g)].map((m) => m[1]);
  assert.deepEqual(tags, ['v6.0', 'v7.0-pre1']);
  assert.match(release, /ZIP="MetalSharp-Wine-\$\{METALSHARP_WINE_TAG#v\}-arm64\.zip"/);
});

test('Settings shows the Steam client choice on FEX only, when MetalSharp Wine carries Linux Steam', () => {
  assert.match(settings, /let client_row = \(crate::variant::is_fex\(\) && app\.linux_steam_available\)\.then/);
  assert.match(settings, /"Linux \(experimental\)"/);
  assert.match(settings, /json!\(\{"steamClient": client\}\)/);
});
