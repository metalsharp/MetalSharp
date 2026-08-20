# VKMT runtime migration

`migrate-to-vkmt-runtime.sh` is the MetalSharp migration path used after a
new DMG is installed. The DMG carries only the small Steam and Goldberg
bootstrap archives; the script downloads the pinned VKMT-Wine release
installer, verifies it, installs the complete runtime, and then performs the
prefix migration.

The default installer release is `VKMT-1.0` and its installer SHA-256 is
pinned in the script. A future VKMT release must update the tag and pinned
installer digest together.

For an existing installation it creates a fresh `~/.metalsharp/prefix-steam`,
restores Steam, `steamapps`, `userdata`, configuration, drive mappings, and
permissions, and preserves MetalSharp caches and UI storage (including the
Steam API-key cache and saved theme). It does not copy bottles or games.

For a first install, when no `prefix-steam` exists, it wineboots a fresh VKMT
prefix and installs Steam from `metalsharp-steam.tar.zst`. In both modes the
Goldberg archive is staged into `runtime/goldberg`.

The script defaults to a non-mutating plan. The application invokes it with
`--apply` from the installed migration resource. Developers can run it with:

```sh
METALSHARP_VKMT_INSTALLER=/path/to/install-metalsharp-wine-runtime.sh \
  tools/migrator/migrate-to-vkmt-runtime.sh --apply
```

Use `--keep-old-prefix` only when an additional local rollback prefix is
wanted. The release installer retains its previous runtime rollback, and the
full pre-migration baseline is external to this script.
