#!/usr/bin/env bash
set -euo pipefail

probe=(/usr/bin/bwrap --unshare-net --ro-bind / / -- /usr/bin/true)
if ! "${probe[@]}"; then
    # Ubuntu 24.04 ships this profile as opt-in. Only the system Bubblewrap
    # executable needs it. Run the probe and subsequent tests as the runner.
    sudo apt-get install --yes apparmor-profiles
    sudo apparmor_parser -r /usr/share/apparmor/extra-profiles/bwrap-userns-restrict
    "${probe[@]}"
fi
