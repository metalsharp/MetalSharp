## Fix a Broken Update


If your updater fails, you can fix it by doing the following:

1) Download [update.sh](https://github.com/metalsharp/MetalSharp/blob/main/app/updater/update.sh)

2) Close Metalsharp

3) Open the Terminal, and run `/bin/bash ~/Downloads/update.sh --recover`

The script reinstalls the build you have: the x86_64 build (`MetalSharp-<version>-arm64.dmg`) or the Arm64 FEX build
(`MetalSharp-<version>-arm64-FEX.dmg`, which carries MetalSharp Wine). It reuses a valid update DMG already downloaded by
MetalSharp or downloads the latest stable official release, verifies the DMG and Developer ID signature, and installs it
using the fixed updater. The x86_64 build needs a newer release than the one installed; the Arm64 FEX build can also
reinstall its own version. It then relaunches MetalSharp and 
runs the normal migration handoff to restore the installation/runtime. The script asks before stopping MetalSharp and 
Steam/Wine processes and may request administrator approval. Once recovered, use MetalSharp's in-app updater for later releases.
