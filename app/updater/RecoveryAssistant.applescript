on run
    set appBundlePath to POSIX path of (path to me)
    set helperPath to appBundlePath & "Contents/Resources/recover-update.sh"

    display dialog "This recovery assistant downloads the official MetalSharp update, verifies its Developer ID signature, and installs it. It will close MetalSharp and stop Steam/Wine processes. Save your work first." buttons {"Cancel", "Continue"} default button "Continue" cancel button "Cancel" with icon caution

    try
        set resultText to do shell script "/bin/bash " & quoted form of helperPath
        display dialog "Recovery finished." & return & return & resultText buttons {"OK"} default button "OK" with icon note
    on error errorMessage number errorNumber
        display dialog "Recovery did not complete (" & errorNumber & "):" & return & return & errorMessage buttons {"OK"} default button "OK" with icon stop
    end try
end run
