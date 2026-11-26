# shellcheck shell=bash
# Sourced by the DMG scripts. retry_hdiutil runs hdiutil with its arguments and, when it fails, tries again after a
# pause, five tries in all. Right after hdiutil writes or detaches a multi-GB image the system can still hold the file
# (the 1.6.0 FEX release run's `hdiutil verify` failed with "Resource temporarily unavailable" the moment its
# `hdiutil create` finished), so one failure is not yet a bad image.
retry_hdiutil() {
  local attempt
  for attempt in 1 2 3 4 5; do
    if hdiutil "$@"; then
      return 0
    fi
    if [ "$attempt" -eq 5 ]; then
      echo "hdiutil $1 failed 5 times: $*" >&2
      return 1
    fi
    echo "hdiutil $1 failed (try $attempt of 5); trying again in $((attempt * 15))s" >&2
    sleep $((attempt * 15))
  done
}
