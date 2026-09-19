#!/usr/bin/env python3
"""Classify capture appearance as pinstripe, flat, or unclassified ``hit``.

This is an observation tool, not a payload or DMA acceptance test. A ``hit``
means only that the frame is neither the stripe signature nor flat; wrong
geometry also returns ``hit``. Flat images do not establish absence of sync.

The retained structural and synthetic tests cover noise, colour/phase changes,
bars and wrong geometry. The bundled 1280x720 reference is a settled mainline
capture; see docs/mainline-display-findings.md for its limits. Nothing touches hardware.

Commands: classify FRAME.png; fingerprint FRAME.png; selftest.
Dependencies: numpy and Pillow (the display dev shell supplies both).
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# The committed baseline lives beside the tool so the classifier is portable and
# the reference frame is version-controlled next to the code that reads it.
BASELINE_DIR = os.path.join(HERE, "oracle")
BASELINE_PNG = os.path.join(BASELINE_DIR, "pinstripe-baseline.png")

# Classification tolerance. docs/mainline-display-findings.md measured 2.3 mean-abs diff
# between two known-pinstripe captures (capture noise only); 8.0 leaves a wide
# margin over that floor while staying far below any real content change.
HIT_THRESHOLD = float(os.environ.get("ORACLE_HIT_THRESHOLD", "8.0"))

# Vertical-stripe signature thresholds. A 1-px green/white pinstripe alternates
# every column (huge horizontal-neighbour diff) but is constant down each column
# (vertical-neighbour diff = noise only). Real content has no such asymmetry.
# These are deliberately lenient so dongle colour/scaling can't fail a real
# pinstripe; they only need to reject solids, gradients and video frames.
STRIPE_RATIO_MIN = 2.5   # horizontal-diff / vertical-diff
STRIPE_HDIFF_MIN = 15.0  # absolute horizontal-neighbour diff (0..255)

# Structural pinstripe signature , evaluated on a frame alone — no
# baseline. A pinstripe is a short-period vertical pattern with no horizontal
# structure, so its column-mean profile repeats at the pinstripe lags while its
# row-mean profile is flat. This is what makes a colour/phase-shifted pinstripe
# still a pinstripe even when its pixels diverge from the stored baseline.
# Requiring BOTH lag 4 and lag 8 is the discriminator against wide colour bars:
# bars correlate strongly at lag 4 (still inside one bar) but not at lag 8.
PINSTRIPE_ACF_LAGS = (4, 8)
PINSTRIPE_ACF_MIN = float(os.environ.get("ORACLE_PINSTRIPE_ACF_MIN", "0.9"))
PINSTRIPE_ROW_STD_MAX = float(os.environ.get("ORACLE_PINSTRIPE_ROW_STD_MAX", "3.0"))

# Exit codes (see module docstring).
EXIT_PINSTRIPE = 0
EXIT_HIT = 10
EXIT_FLAT = 20   # no signal: a uniform frame is never a hit
EXIT_USAGE = 2
EXIT_BAD_BASELINE = 3


def load_frame(path):
    """Load an image as an HxWx3 uint8 RGB array."""
    try:
        with Image.open(path) as im:
            return np.asarray(im.convert("RGB"), dtype=np.uint8)
    except FileNotFoundError:
        raise
    except Exception as e:  # noqa: BLE001 — surface any decode error the same way
        raise ValueError("cannot read image %r: %s" % (path, e))


def mean_abs_diff(a, b):
    """Mean absolute per-pixel difference; +inf when the geometry differs."""
    if a.shape != b.shape:
        return float("inf")
    return float(np.mean(np.abs(a.astype(np.int16) - b.astype(np.int16))))


def stripe_signature(img):
    """Vertical-stripe fingerprint of a frame.

    Returns (hdiff, vdiff, ratio): mean absolute neighbour differences across
    columns (hdiff) and down rows (vdiff), and their ratio. A 1-px vertical
    pinstripe has hdiff >> vdiff; a solid/gradient/video frame does not.
    """
    a = img.astype(np.int16)
    hdiff = float(np.mean(np.abs(a[:, 1:, :] - a[:, :-1, :]))) if img.shape[1] > 1 else 0.0
    vdiff = float(np.mean(np.abs(a[1:, :, :] - a[:-1, :, :]))) if img.shape[0] > 1 else 0.0
    ratio = hdiff / vdiff if vdiff > 1e-6 else float("inf")
    return hdiff, vdiff, ratio


def is_pinstripe_shape(img):
    """True when the frame carries the vertical-stripe signature of a pinstripe.

    Used to guard baseline *adoption* (cmd_baseline). Classification uses the
    period-based pinstripe_signature below instead ."""
    hdiff, _vdiff, ratio = stripe_signature(img)
    return hdiff >= STRIPE_HDIFF_MIN and ratio >= STRIPE_RATIO_MIN


def _luma(img):
    """Grayscale HxW float profile of a frame (plain channel mean)."""
    return img.astype(np.float64).mean(axis=2)


def autocorr(profile, lag):
    """Normalised autocorrelation of a 1-D profile at `lag`.

    Pearson correlation of the profile with its lag-shifted self; 1.0 when the
    profile repeats exactly with a period dividing `lag`. Returns 0.0 for a flat
    (zero-variance) profile or a lag not shorter than the profile."""
    n = len(profile)
    if lag <= 0 or lag >= n:
        return 0.0
    a = profile[:-lag]
    b = profile[lag:]
    a = a - a.mean()
    b = b - b.mean()
    denom = float(np.sqrt(np.sum(a * a) * np.sum(b * b)))
    if denom <= 1e-9:
        return 0.0
    return float(np.sum(a * b) / denom)


def pinstripe_signature(img):
    """Baseline-independent structural fingerprint of a frame .

    Returns (col_acf, row_std): col_acf maps each lag in PINSTRIPE_ACF_LAGS to
    the column-mean profile's autocorrelation there (how strongly the frame
    repeats at the short pinstripe period), and row_std is the standard
    deviation of the row-mean profile (0 for a purely vertical pattern)."""
    luma = _luma(img)
    col_profile = luma.mean(axis=0)  # one value per column
    row_profile = luma.mean(axis=1)  # one value per row
    col_acf = {lag: autocorr(col_profile, lag) for lag in PINSTRIPE_ACF_LAGS}
    row_std = float(row_profile.std())
    return col_acf, row_std


def is_pinstripe_signature(img):
    """True when the frame IS a pinstripe by structure, regardless of colour.

    Three conditions, all baseline-independent :
      * short-period vertical repetition — the column-mean profile's
        autocorrelation is >= PINSTRIPE_ACF_MIN at every pinstripe lag (4 and
        8); this is what a colour/phase-shifted pinstripe still satisfies even
        though its pixel diff against the stored baseline is large, and what
        non-periodic content (a real hit) fails;
      * no horizontal structure — a flat row-mean profile (row_std small);
      * high vertical-neighbour contrast (is_pinstripe_shape) — a wide-margin
        guard that excludes wide colour bars, whose neighbour contrast is near
        zero (hdiff ~5) while a pinstripe's is large (baseline hdiff ~101).
    The lag-8 autocorrelation of the synthetic colour-bar fixture sits right at
    ~0.9, so this contrast guard, not the lag-8 threshold alone, is what keeps
    colour bars a hit."""
    col_acf, row_std = pinstripe_signature(img)
    return (row_std <= PINSTRIPE_ROW_STD_MAX
            and all(col_acf[lag] >= PINSTRIPE_ACF_MIN for lag in PINSTRIPE_ACF_LAGS)
            and is_pinstripe_shape(img))


def fingerprint(img):
    """Machine-readable structural summary of a frame."""
    h, w = img.shape[0], img.shape[1]
    hdiff, vdiff, ratio = stripe_signature(img)
    col_acf, row_std = pinstripe_signature(img)
    return {
        "width": w,
        "height": h,
        "mean_rgb": [round(float(x), 3) for x in img.reshape(-1, 3).mean(axis=0)],
        "h_neighbor_diff": round(hdiff, 4),
        "v_neighbor_diff": round(vdiff, 4),
        "stripe_ratio": (None if ratio == float("inf") else round(ratio, 4)),
        "col_autocorr": {str(lag): round(col_acf[lag], 4) for lag in PINSTRIPE_ACF_LAGS},
        "row_profile_std": round(row_std, 4),
        "looks_like_pinstripe": is_pinstripe_shape(img),
        "is_pinstripe_signature": is_pinstripe_signature(img),
    }


# ---------------------------------------------------------------------------
# subcommands
# ---------------------------------------------------------------------------

def cmd_fingerprint(args):
    img = load_frame(args.frame)
    print(json.dumps(fingerprint(img), indent=2))
    return 0


FLAT_RANGE_MAX = 4  # per-channel (max - min) at or below this = visually flat


def is_flat(img):
    """True for a visually uniform frame (black, white, or a solid colour). MJPEG noise on a dead input is ±1-2 LSB, hence the small range."""
    arr = np.asarray(img).reshape(-1, 3)
    return bool((arr.max(axis=0) - arr.min(axis=0)).max() <= FLAT_RANGE_MAX)


def _classify(frame_path, baseline_path, threshold):
    """Core classification, factored out so selftest can drive it in-process.

    Returns (verdict, detail_dict). verdict is "pinstripe", "flat" or "hit".
    """
    frame = load_frame(frame_path)
    baseline = load_frame(baseline_path)
    if is_flat(frame):
        return "flat", {
            "reason": "uniform frame; sync and commanded content are not established",
            "mean_rgb": [round(float(x), 3) for x in frame.reshape(-1, 3).mean(axis=0)],
            "mean_abs_diff": None,
            "threshold": threshold,
        }

    # Structural signature and pixel diff are both computed and both reported;
    # the signature is the primary verdict, the diff the secondary check .
    col_acf, row_std = pinstripe_signature(frame)
    same_geometry = frame.shape == baseline.shape
    diff = mean_abs_diff(frame, baseline) if same_geometry else None
    detail = {
        "col_autocorr": {str(lag): round(col_acf[lag], 4) for lag in PINSTRIPE_ACF_LAGS},
        "row_profile_std": round(row_std, 4),
        "mean_abs_diff": (round(diff, 4) if diff is not None else None),
        "threshold": threshold,
    }

    # A change of capture geometry is always a hit (the sink resolution moved);
    # this geometry guard stays ahead of the pixel comparison.
    if not same_geometry:
        detail["reason"] = "geometry differs from baseline"
        detail["frame"] = [frame.shape[1], frame.shape[0]]
        detail["baseline"] = [baseline.shape[1], baseline.shape[0]]
        return "hit", detail

    # Structural pinstripe signature wins over the pixel diff : a colour/
    # phase-shifted pinstripe keeps the short vertical period and must not read
    # as a hit just because its colours drifted from the one stored baseline.
    if is_pinstripe_signature(frame):
        detail["reason"] = (
            "vertical pinstripe signature (col autocorr >= %.2f at lags %s, "
            "flat row profile) — structural pinstripe regardless of the diff"
            % (PINSTRIPE_ACF_MIN, ",".join(str(lag) for lag in PINSTRIPE_ACF_LAGS))
        )
        return "pinstripe", detail

    verdict = "pinstripe" if diff <= threshold else "hit"
    detail["reason"] = ("within capture-noise tolerance" if verdict == "pinstripe"
                        else "diverges from the stored pinstripe")
    return verdict, detail


def cmd_classify(args):
    baseline_path = args.baseline or BASELINE_PNG
    if not os.path.exists(baseline_path):
        sys.stderr.write(
            "oracle: no committed baseline at %s — capture a known-pinstripe "
            "frame and supply it using --baseline "
            "(see docs/mainline-display.md).\n" % baseline_path
        )
        return EXIT_BAD_BASELINE

    verdict, detail = _classify(args.frame, baseline_path, args.threshold)
    result = {"verdict": verdict, "frame": os.path.basename(args.frame)}
    result.update(detail)
    print(json.dumps(result, indent=2))
    if verdict == "pinstripe":
        return EXIT_PINSTRIPE
    if verdict == "flat":
        return EXIT_FLAT
    return EXIT_HIT


# ---------------------------------------------------------------------------
# selftest — offline, no hardware, no committed baseline needed. CI gate.
# ---------------------------------------------------------------------------

def _synth_pinstripe(w=256, h=144, noise=0, seed=0):
    """A synthetic 1-px green/white vertical pinstripe, optional uniform noise."""
    img = np.empty((h, w, 3), dtype=np.uint8)
    green = np.array([0, 255, 0], dtype=np.uint8)
    white = np.array([255, 255, 255], dtype=np.uint8)
    img[:, 0::2] = green
    img[:, 1::2] = white
    if noise:
        rng = np.random.default_rng(seed)
        jitter = rng.integers(-noise, noise + 1, size=img.shape, dtype=np.int16)
        img = np.clip(img.astype(np.int16) + jitter, 0, 255).astype(np.uint8)
    return img


def _synth_pinstripe_shifted(w=256, h=144, period=4):
    """A colour- and phase-shifted pinstripe : the same short vertical
    period as the baseline but amber/indigo stripes far from green/white, so the
    pixel diff against the baseline is huge while the structure is unchanged. The
    two colours differ in luma (146.7 vs 93.3) so the column profile actually
    carries the period — an isoluminant swap would flatten it."""
    img = np.empty((h, w, 3), dtype=np.uint8)
    amber = np.array([220, 180, 40], dtype=np.uint8)   # luma 146.7
    indigo = np.array([40, 60, 180], dtype=np.uint8)   # luma 93.3
    half = period // 2
    for x in range(w):
        img[:, x] = amber if (x % period) < half else indigo
    return img


def _synth_colorbars(w=256, h=144):
    """A non-pinstripe frame: 8 wide SMPTE-ish vertical bars (a real 'hit')."""
    bars = np.array(
        [[255, 255, 255], [255, 255, 0], [0, 255, 255], [0, 255, 0],
         [255, 0, 255], [255, 0, 0], [0, 0, 255], [0, 0, 0]], dtype=np.uint8)
    img = np.empty((h, w, 3), dtype=np.uint8)
    for x in range(w):
        img[:, x] = bars[x * len(bars) // w]
    return img


def cmd_selftest(_args):
    import tempfile

    failures = []

    def check(name, cond):
        print(("  ok  " if cond else " FAIL ") + name)
        if not cond:
            failures.append(name)

    # structural signature — the baseline-adoption guard (hdiff/ratio)
    check("pinstripe has the stripe signature", is_pinstripe_shape(_synth_pinstripe()))
    check("noisy pinstripe still has the signature",
          is_pinstripe_shape(_synth_pinstripe(noise=3, seed=1)))
    check("colorbars are NOT a pinstripe", not is_pinstripe_shape(_synth_colorbars()))
    solid = np.full((144, 256, 3), 200, dtype=np.uint8)
    check("a solid frame is NOT a pinstripe", not is_pinstripe_shape(solid))
    check("a solid frame is flat", is_flat(solid))
    check("a black frame with capture noise is flat", is_flat(np.random.randint(6, 9, (144, 256, 3), dtype=np.uint8)))

    # period-based pinstripe signature — the classify verdict
    check("pinstripe carries the period signature",
          is_pinstripe_signature(_synth_pinstripe()))
    check("noisy pinstripe carries the period signature",
          is_pinstripe_signature(_synth_pinstripe(noise=3, seed=1)))
    check("a 4-px colour/phase-shifted pinstripe carries the period signature",
          is_pinstripe_signature(_synth_pinstripe_shifted(period=4)))
    check("colorbars lack the period signature (near-zero neighbour contrast)",
          not is_pinstripe_signature(_synth_colorbars()))
    check("a solid frame lacks the period signature",
          not is_pinstripe_signature(solid))

    # end-to-end classify against a written baseline
    with tempfile.TemporaryDirectory() as td:
        base = os.path.join(td, "base.png")
        Image.fromarray(_synth_pinstripe(), "RGB").save(base)

        noisy = os.path.join(td, "noisy.png")
        Image.fromarray(_synth_pinstripe(noise=2, seed=7), "RGB").save(noisy)
        v, d = _classify(noisy, base, HIT_THRESHOLD)
        check("noisy pinstripe classifies as pinstripe (diff=%.2f)" % d["mean_abs_diff"],
              v == "pinstripe")

        # a colour/phase-shifted pinstripe has a large diff from the
        # baseline yet must still classify as pinstripe on its structure.
        shifted = os.path.join(td, "shifted.png")
        Image.fromarray(_synth_pinstripe_shifted(period=4), "RGB").save(shifted)
        v, d = _classify(shifted, base, HIT_THRESHOLD)
        check("colour/phase-shifted pinstripe classifies as pinstripe "
              "despite diff=%.2f > %.1f" % (d["mean_abs_diff"], HIT_THRESHOLD),
              v == "pinstripe")

        bars = os.path.join(td, "bars.png")
        Image.fromarray(_synth_colorbars(), "RGB").save(bars)
        v, d = _classify(bars, base, HIT_THRESHOLD)
        bars_hdiff = stripe_signature(_synth_colorbars())[0]
        check("colorbars classify as HIT (acf@8=%s, hdiff=%.1f < %.1f)"
              % (d["col_autocorr"]["8"], bars_hdiff, STRIPE_HDIFF_MIN), v == "hit")

        wrong = os.path.join(td, "wrong.png")
        Image.fromarray(_synth_pinstripe(w=128, h=72), "RGB").save(wrong)
        v, d = _classify(wrong, base, HIT_THRESHOLD)
        check("wrong geometry classifies as HIT", v == "hit")

    # The bundled settled mainline frame must classify as pinstripe.
    committed = [
        ("baseline", BASELINE_PNG),
    ]
    if os.path.exists(BASELINE_PNG):
        for name, path in committed:
            if not os.path.exists(path):
                check("committed %s frame present (%s)"
                      % (name, os.path.basename(path)), False)
                continue
            v, d = _classify(path, BASELINE_PNG, HIT_THRESHOLD)
            check("committed %s frame classifies as pinstripe (diff=%s, acf=%s)"
                  % (name, d.get("mean_abs_diff"), d.get("col_autocorr")),
                  v == "pinstripe")
    else:
        check("bundled reference frame is present", False)

    if failures:
        print("\nselftest: %d FAILURE(S)" % len(failures))
        return 1
    print("\nselftest: all checks passed")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="Pinstripe appearance classifier (hit is unclassified)")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("fingerprint", help="dump a frame's structural fingerprint")
    s.add_argument("frame")
    s.set_defaults(func=cmd_fingerprint)

    s = sub.add_parser("classify", help="classify a frame as pinstripe or hit")
    s.add_argument("frame")
    s.add_argument("--baseline", help="baseline PNG (default: committed oracle/pinstripe-baseline.png)")
    s.add_argument("--threshold", type=float, default=HIT_THRESHOLD,
                   help="mean-abs-diff HIT threshold (default %(default)s)")
    s.set_defaults(func=cmd_classify)

    s = sub.add_parser("selftest", help="offline synthetic self-check (no hardware)")
    s.set_defaults(func=cmd_selftest)

    args = p.parse_args(argv)
    try:
        return args.func(args)
    except FileNotFoundError as e:
        sys.stderr.write("oracle: file not found: %s\n" % e)
        return EXIT_USAGE
    except ValueError as e:
        sys.stderr.write("oracle: %s\n" % e)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
