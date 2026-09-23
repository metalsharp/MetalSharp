const path = require("node:path");
const { notarize } = require("@electron/notarize");

function hasPasswordCredentials() {
  return Boolean(process.env.APPLE_ID && process.env.APPLE_APP_SPECIFIC_PASSWORD && process.env.APPLE_TEAM_ID);
}

function hasApiKeyCredentials() {
  return Boolean(process.env.APPLE_API_KEY && process.env.APPLE_API_KEY_ID && process.env.APPLE_API_ISSUER);
}

exports.default = async function notarizeMetalSharp(context) {
  if (context.electronPlatformName !== "darwin") {
    return;
  }

  if (process.env.METALSHARP_DEFER_NOTARIZATION_TO_DMG === "1") {
    console.log("Apple notarization deferred to the signed outermost DMG.");
    return;
  }

  const requireNotarization = process.env.METALSHARP_REQUIRE_NOTARIZATION === "1";
  if (!hasPasswordCredentials() && !hasApiKeyCredentials()) {
    const message =
      "Apple notarization skipped: missing APPLE_ID/APPLE_APP_SPECIFIC_PASSWORD/APPLE_TEAM_ID or APPLE_API_KEY/APPLE_API_KEY_ID/APPLE_API_ISSUER.";
    if (requireNotarization) {
      throw new Error(message);
    }
    console.log(message);
    return;
  }

  const appName = context.packager.appInfo.productFilename;
  const appPath = path.join(context.appOutDir, `${appName}.app`);
  const baseOptions = {
    appBundleId: context.packager.appInfo.appId,
    appPath,
    tool: "notarytool",
  };

  if (hasApiKeyCredentials()) {
    await notarize({
      ...baseOptions,
      appleApiKey: process.env.APPLE_API_KEY,
      appleApiKeyId: process.env.APPLE_API_KEY_ID,
      appleApiIssuer: process.env.APPLE_API_ISSUER,
    });
    return;
  }

  await notarize({
    ...baseOptions,
    appleId: process.env.APPLE_ID,
    appleIdPassword: process.env.APPLE_APP_SPECIFIC_PASSWORD,
    teamId: process.env.APPLE_TEAM_ID,
  });
};
