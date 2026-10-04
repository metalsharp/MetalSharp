# Release Signing and Notarization
**Updated:** 2026-10-04


MetalSharp DMG releases must be signed with a Developer ID Application certificate and notarized before upload. Without that, macOS Gatekeeper can show the "Apple could not verify this app is free of malware" prompt and force users through Security & Privacy.

Configure these GitHub Actions secrets to produce a signed and notarized DMG:

- `MACOS_CERTIFICATE_P12`: base64-encoded Developer ID Application `.p12`
- `MACOS_CERTIFICATE_PASSWORD`: password for the `.p12`

Configure one notarization credential set:

- Apple ID credentials: `APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD`, `APPLE_TEAM_ID`
- App Store Connect API key credentials: `APPLE_API_KEY_P8_BASE64`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER`

The release job imports the certificate into a temporary keychain (`tools/dmg/prepare-apple-signing.sh` exports `APPLE_SIGNING_IDENTITY`), then `app-gpui/package-app.sh` builds the GPUI app and, with `METALSHARP_GPUI_SIGN_IDENTITY` set to that identity, signs it inside-out with hardened runtime, a secure timestamp and `tools/dmg/entitlements.mac.plist` (loose Mach-O under Resources, then nested apps, then `MetalSharp.app`). `tools/dmg/package-gpui-dmg.sh` builds `MetalSharp-<version>-arm64.dmg`, `tools/dmg/sign-notarize-dmg.sh` signs, notarizes and staples the DMG, and `tools/dmg/verify-notarization.sh` validates the app and DMG. The notarization verifier requires a Developer ID Application identity on the app, a stapled ticket, accepted Gatekeeper assessment, and a structurally valid DMG image before upload.

If the Apple secrets are not configured yet, Release CI falls back to an unsigned/ad-hoc DMG instead of skipping the release entirely. The fallback signs the app ad-hoc, packages the DMG, verifies the embedded runtime assets, and uploads a `DMG-SIGNING.txt` marker beside the DMG. This keeps release artifacts available during credential setup, but the unsigned DMG can still trigger Gatekeeper warnings until the Developer ID and notarization secrets are configured.

## In-app updater requirements

Installed copies update through `update.sh`, whose recovery mode (used by the in-app updater) downloads `MetalSharp-<version>-arm64.dmg` from the latest release and refuses the update unless the new app is `com.metalsharp.app` at the requested version, passes `codesign --verify --deep --strict`, and is signed by the **same Developer ID team** as the installed app. Keep the DMG name, bundle identifier and signing certificate team unchanged across releases; an unsigned/ad-hoc release cannot be installed through the updater.
