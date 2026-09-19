# SG2000 mainline display bring-up

The Nix Badge 2.0 has produced HDMI pinstripe on its SG2000 RISC-V C906 core
with mainline-based Linux 7.2.2. The public modules reproduced that image in
one warm-boot trial, but capture timing failed acceptance. Commanded pixel content,
framebuffer scanout, DRM modesetting, Sway and cold-start reliability remain
unproven. The drivers are not upstream in Linux. See [findings](mainline-display-findings.md).

## Hardware and pins

SG2000 display → four-lane MIPI-DSI/D-PHY → LT8912B → HDMI, at 1280×720,
nominal 60 Hz, RGB888, positive H/V sync. Captures used an MS2130 USB device.

| Input | Pin |
| --- | --- |
| BadgeOS base | `NixVegas/BadgeOS`, `master`, `da589a0fe996318a42f69fc491521b42b5a52e23` |
| Nixpkgs kernel/toolchain | `a3116115851d68b8952a2a4221cc25a84e56b532` |
| Linux | `7.2.2`, tarball SHA-256 `7d0e7ce14f98c43efe880cffbf354a59be45928fdf7170d7333c374ae91c0d83` |
| Vendor HAL | `sophgo/osdrv`, `aa542c41df94f7bc656cb740f6622a5dca7dc403` |
| Wiring | `NixVegas/pcb`, `d97cfc1d2f50a0447eaf80c7d025e58900b8f5e6` |

The build applies separate clock-provider, LT8912B lifecycle and DSI patches.
The opt-in DT enables I2C2 at 100 kHz on J13/J14, the host/bridge graph, and
always-on 1.8 V bridge rails with RC reset (no CPU-controlled reset GPIO).
Both VIP syscon and DSI host require register-interface clock 128, named
`cfg_reg` on the host. Physical pad roles are `1,2,0,3,4`, separate from
logical data-lane indices. Ordinary configurations and the lockfile are unchanged.

## Build and checks

Inspect the plan before building. These checks do not build a full kernel:

```sh
nix build --dry-run .#sophgo-disp-test .#lt8912b-test .#cv18xx-pll-test .#display-dtb-test
nix build --no-link --cores 2 --max-jobs 1 .#sophgo-disp-test .#lt8912b-test .#cv18xx-pll-test .#display-dtb-test
nix develop .#display -c python3 tools/display/oracle.py selftest
nix develop .#display -c python3 tools/display/tests/test_pinstripe_registers.py
nix develop .#display -c python3 tools/display/tests/test_capture_timing.py
nix develop .#display -c python3 tools/display/tests/test_capture_device.py
nix develop .#display -c python3 tests/test-display-tools.py
```

The optional [GitHub workflow](../.github/workflows/display-checks.yml) runs
these native/DT and observation tests with two cores, one build job and a
30-minute limit. It retains the tested revision, lockfile hash and results.
The job does not build a full kernel/image or access hardware; green checks
establish these software contracts, not display acceptance. All tests remain
runnable with the commands above independently of adopting the workflow.

`nixosConfigurations.duo-s-riscv-pinstripe-x86_64` declares the complete image
configuration. Building its `config.system.build.toplevel` requires a separate
full-kernel build budget; it has been evaluated, not built. Diagnostics are
enabled at module load, but the display never starts automatically.

For the existing test installation, this adapter builds only the two modules:

```sh
nix-build pkgs/display/cached-modules.nix --argstr source "$(pwd)" \
  -A driver -A bridge --impure --cores 14 --max-jobs 1 --dry-run
nix-build pkgs/display/cached-modules.nix --argstr source "$(pwd)" \
  -A driver -A bridge --impure --cores 14 --max-jobs 1 --no-out-link
```

It requires the historical kernel development output declared in the recipe.
An alternative `--argstr kernelDev /nix/store/...` must match its `.config`,
`Module.symvers` and bridge-header hashes. Missing or mismatched cache inputs
are refused; this command cannot provision a new system. Modules compile with
`W=1 KCFLAGS=-Werror`. Native tests cover calculations and lifecycle failures
using mocks; they cannot establish electrical behavior.

For device discovery and a known-source dongle check, see the
[capture setup guide](hdmi-capture.md).

## One hardware validation cycle

Coordinate readiness and physical recovery with the operator. Preserve both
installed modules and their selectors as a rollback pair. Verify ISA, kernel,
live DT, idle driver state and restart support before staging the committed
candidate's modules. Record and verify their hashes and loaded ELF notes.
The test configuration disables systemd watchdog timers; the kernel restart
handler is separate. Verify the actual image's reboot behavior before using
a guarded warm reboot.

This procedure assumes that the operator has already staged the candidate,
verified rollback and performed the guarded reboot. Image installation and
reboot provisioning are not supplied by this public toolkit.

In `nix develop .#display`, follow the capture guide's device discovery and
[Prepare the recording](hdmi-capture.md#prepare-the-recording) blocks first.
In that same shell, set `badge_target` to the verified SSH target,
`previous_boot` and `fresh_boot` to the recorded boot IDs, and `dsi_note_sha`,
`bridge_note_sha`, `live_fdt_sha` to the verified SHA-256 values. The first two
hashes are of the loaded modules' `/sys/module/{sophgo_dsi,lontium_lt8912b}/notes/.note.gnu.build-id`
files, not the `.ko` files; the last is of `/sys/firmware/fdt`.

```sh
ssh "${badge_target:?set verified SSH target}" sudo -n bash -s -- \
  "${previous_boot:?}" "${fresh_boot:?}" "${dsi_note_sha:?}" \
  "${bridge_note_sha:?}" "${live_fdt_sha:?}" \
  < tools/display/hold-once.sh > "$capture_out/badge.log" 2>&1 &
hold_pid=$!
```

Watch `capture-01/badge.log` from a second terminal. Once it prints
`DISPLAY_HOLD_BEGIN seconds=45`, immediately run the capture guide's
[Record](hdmi-capture.md#record) block in the original shell. If the hold
never begins or has already ended, retain the log and stop this trial.
The helper starts once, holds for 45 seconds, retrieves a cached snapshot
and attempts one stop. Do not unload/retry, issue an extra stop or reboot
at the end. Userspace timeouts cannot interrupt stalled MMIO.

After recording, wait for the helper's own cleanup, then extract and decode
the snapshot. The extractor verifies the transported size and hash, and
refuses an existing output file. It does not establish hardware acceptance.
Never use `/dev/mem`:

```sh
wait "$hold_pid"
hold_rc=$?
printf 'hold=%s\n' "$hold_rc" > "$capture_out/hold-exit.txt"
python3 tools/display/snapshot-from-log.py "$capture_out/badge.log" "$capture_out/snapshot.bin"
python3 tools/display/pinstripe-registers.py "$capture_out/snapshot.bin" "$capture_out/decoded" \
  --golden tools/display/reference
```

Require three slots, six blocks, fences `64/256/16/64/704/51`, and successful
started/complete slots (`valid=0x3f`, `error=0`). Require `MAC_EN=0x4`,
`PAT_CFG=0x0701000a`, D-PHY PD zero and 0/51 D-PHY differences. Other blocks
have no bundled golden reference. Read the values; decoder exit 0 is not acceptance.

The recording block already probes and samples the clip. Validate those same
files, using its recorded exit statuses in the same shell:

```sh
python3 tools/display/validate-capture.py "$capture_out/frames.json" \
  "$capture_out/samples/samples.json" "$capture_out/ffmpeg.log" \
  --ffmpeg-exit "${capture_rc:?}" --ffprobe-exit "${probe_rc:?}"
```

Use fresh output paths. The sampler verifies timestamps against the clip and
preserves source frame indices and full oracle results. Acceptance requires
15 seconds of increasing 60 Hz timestamps (±1 ms median interval tolerance),
no irregular intervals, all 15 observations and 13/13 settled pinstripe samples.
Validator exit 0 passes. Exit 2 retains anomalies confined to the first 2 seconds
with at least 13 seconds afterward as diagnostic-only evidence; exit 1 rejects
the clip. Require `hold_rc=0`, `DISPLAY_STOP_EXIT=0` and final status
`selected=1 terminal=0 started=0 owned=0 stage=idle error=0`.

Snapshots are cached at prepared/started/complete: reading later does **not**
resample hardware or qualify the later D-PHY drift described in the findings.
This cycle tests warm-boot pinstripe only. A failure ends the cycle; retain
evidence and agree on the next change before another attempt. Never flash
eMMC, repartition storage, disturb the preserved vendor card or perform
unplanned recovery. Physical power/card/cabling changes belong to the operator.
