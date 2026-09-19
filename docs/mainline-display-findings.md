# Findings and evidence

On September 18, 2026, a warm-boot trial produced fine vertical HDMI pinstripe.
The driver selected the internal generator's vendor-named colour-bar mode;
correspondence between that selection and captured pixels remains unverified.
This advances [HDMI issue #4](https://github.com/NixVegas/BadgeOS/issues/4).

## Decisive change

Initial work established clock ownership, bridge sequencing and board wiring,
then compared display/scaler initialization with a working vendor Debian
system. Matching programmed registers did not establish a high-speed link.
In-kernel snapshots exposed the D-PHY while the driver owned its clocks.
An apparent dynamic indication at `+0x44` proved to be the driver's own write;
a bounded MAC-start probe still failed to enable the link.

The decisive difference was the D-PHY power-down word at `+0x64`: a masked
clear preserved `0x2000`, while vendor initialization wrote the whole word as
zero for this five-pad configuration. Vendor source, module disassembly and
a supervised reference read supported using that full write during prepare.
Shutdown masks and ordering were retained. The residual bit's meaning remains
undocumented.

| Observation | Before | After |
| --- | --- | --- |
| `DSI_MAC +0x000` after start | `0x00000000` | `0x00000004` |
| `D-PHY +0x064` | `0x00002000` | `0x00000000` |
| D-PHY words differing from reference | 6/51 | 0/51 at started/complete |
| MAC-start timeout | Present | Absent |
| HDMI appearance | No accepted pinstripe | Fine vertical pinstripe |

## Evidence and limits

The initial mainline trial decoded 882 frames over 15 seconds, from 0.321 to
14.986 seconds, with subsequent intervals around 17 ms. Its late first timestamp
failed the strict capture gate. It establishes appearance, not loss-free
capture acceptance.
The [settled frame](../tools/display/oracle/pinstripe-baseline.png) has mean
adjacent-column difference 57.89 and adjacent-row difference zero. PNG SHA-256:
`9df8c82bbcaba73d5b4574501de63036bfb8570457fb24b62be30d3c5db3c139`.
The bundled 51-word D-PHY fixture matches the supervised vendor reference and
initial mainline snapshot.

`PAT_CFG=0x0701000a` records that internal generator configuration, including
`PAT_PRD[23:16]=1` (vendor `reg_disp.h`). Its pixel-level effect has not been
verified: fine stripes could be the selected mode's output. This path submits
no CPU framebuffer. Solid-colour and period changes are needed to distinguish
faithful pattern output from content-invariant stripes.

Later unchanged-pattern trials yielded pixel-identical stripes, but one trial's
D-PHY differences rose from 0/51 to 1/51 at 0.629 seconds and 4/51
at 42.684 seconds. Offsets `0x28`, `0x2c`, `0x30` are vendor-named lane-state
words with unknown bit meanings; `0x34` is also unexplained. The trials stopped
cleanly before the commanded-payload experiment. These changes are unresolved.

## Public candidate trial

The public modules were tested once on September 18. Their driver, kernel,
DT and build inputs match commit `70edbaaa9d274648233cd47942df9ffd206feb60`;
later commits change observation tools and documentation only. All 13 settled
samples were pixel-identical to the reference.
Both startup snapshots had `MAC_EN=4`, `PD=0` and 0/51 D-PHY differences;
the driver stopped cleanly and remained idle.

The recording had 884 frames, PTS 0.000–14.999 seconds and no FFmpeg errors.
A 317 ms first gap was followed by a 4 ms interval at 1.250 seconds, so it
failed the capture gate, including the diagnostic exception. Two startup
samples were flat. This reproduces warm-boot pinstripe appearance; it is
**not a full acceptance pass**. No repeat cycle followed.

Offline comparison then found the same 4–5 ms settling interval in the earlier
Debian and mainline recordings. The new short-interval check was stricter than
the original duration check, which accepts this clip. Both toolsets now retain
anomalies confined to the first two seconds as diagnostic-only; this replay
does not change the saved failure or yield a strict pass. Publication validation
does not advance the payload or cold-start milestones.

## Source checks and attribution

Consolidating 26 DSI patches reproduced all 526 vendor-tree files and symlinks.
Subsequent cleanup changed comments and Kconfig descriptions only. Clock and
bridge implementations match their original patched Linux sources byte for
byte; the selected DT compiles to the same sorted DTS. Native test updates fix
stale expectations for the existing 256-read MAC probe and full PD write, plus
a missing delay shim. The trial above
uses the checks described in [reproduction](mainline-display.md).

Nick directed the RISC-V effort, preserved the Debian reference, performed
physical hardware work and arranged builds and trials. Implementation, tests,
analysis and documentation received substantial Claude Code and Codex assistance.
Morgan Jones contributed public BadgeOS board/runtime work, register-comparison
methodology and vendor-stack observations about sync polarity and geometry.
Sophgo's GPL HAL supplies register/calculation provenance; original source
notices retain vendor, Linux and board contributor credit. The separate
[I2C2 PR #5](https://github.com/NixVegas/BadgeOS/pull/5) supplies the ARM wiring
precedent; this RISC-V graph does not depend on that branch.
