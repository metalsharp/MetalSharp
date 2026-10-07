## Fix a Broken Update


If your updater fails, you can fix it by doing the following:

1) Download [update.sh](https://github.com/metalsharp/MetalSharp/blob/main/app/updater/update.sh)

2) Close Metalsharp

3) Open the Terminal, and run `/bin/bash ~/Downloads/update.sh --recover`

The script reuses a valid update DMG already downloaded by MetalSharp or downloads the latest stable official release, 
verifies the DMG and Developer ID signature, and installs it using the fixed updater. It then relaunches MetalSharp and 
runs the normal migration handoff to restore the installation/runtime. The script asks before stopping MetalSharp and 
Steam/Wine processes and may request administrator approval. Once recovered, use MetalSharp's in-app updater for later releases.
