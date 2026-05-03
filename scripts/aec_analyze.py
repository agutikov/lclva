#!/usr/bin/env python3
"""Offline analysis for `acva demo aec-record` outputs.

Reads the three WAVs the demo writes and answers, in order:

  1. What is the cold-start offset between the original PCM and the
     mic recording?  (cross-correlation peak between original and raw)
     This is dominated by the playback queue prefill + PortAudio
     output buffer; it is NOT the AEC-relevant acoustic delay.  APM's
     adaptive delay estimator (~28 ms on the dev workstation per
     `acva demo aec-hw`) is the value the cancellation algorithm
     actually uses.
  2. How does the mic-side signal differ from the original
     by frequency band? (per-band attenuation)
  3. How much of the raw signal does APM actually cancel?
     (per-band ERLE between raw and aec, reported two ways: the
     AS-OUTPUT result the production VAD/STT actually sees, and the
     LEVEL-MATCHED result that isolates the AEC subsystem from AGC
     gain — AGC otherwise lifts the cleaned signal back up by 5-9 dB
     in the speech band and makes "no cancellation" indistinguishable
     from "10 dB cancellation + 5 dB AGC gain")
  4. What is the dominant failure mode?  (verdict line mapped
     from the per-band table to the three hypotheses in
     docs/aec_report.md S 6)

numpy is required.  scipy + matplotlib are optional; when
matplotlib is available the script also writes per-stage
spectrogram + waveform + ratio charts to <out-dir>.

Usage
-----
    scripts/aec_analyze.py <out-dir>          # default /tmp/acva-aec-rec/

For the cleanest read on AEC effectiveness, run the demo with
`ACVA_AEC_NO_POSTPROC=1 ./acva demo aec-record` to disable AGC + NS
upstream.  The level-matched table here gives the same answer when
they are on, but the as-output table is then more honest about what
the production pipeline actually receives.

Inputs (in <out-dir>):
    original.wav         (mono int16, ~22050 Hz)
    raw_recording.wav    (mono int16, 48000 Hz)
    aec_recording.wav    (mono int16, 16000 Hz)
"""

import argparse
import os
import sys
import wave
from dataclasses import dataclass

try:
    import numpy as np
except ImportError as e:
    sys.stderr.write("aec_analyze: numpy is required (pip install numpy)\n")
    raise SystemExit(2) from e


def _have_matplotlib() -> bool:
    import importlib.util
    return importlib.util.find_spec("matplotlib") is not None


# Frequency bands the report aggregates into.  Bounded by the lowest
# rate in the chain (16 kHz aec -> Nyquist 8 kHz).
BANDS_HZ = [
    (100, 300),
    (300, 1000),
    (1000, 3000),
    (3000, 8000),
]


@dataclass
class Wav:
    samples: np.ndarray  # float32, [-1, 1]
    rate: int
    name: str

    @property
    def duration_s(self) -> float:
        return len(self.samples) / self.rate

    @property
    def rms(self) -> float:
        if len(self.samples) == 0:
            return 0.0
        return float(np.sqrt(np.mean(self.samples.astype(np.float64) ** 2)))

    @property
    def peak(self) -> float:
        if len(self.samples) == 0:
            return 0.0
        return float(np.max(np.abs(self.samples)))


def read_wav(path: str) -> Wav:
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1:
            raise ValueError(f"{path}: expected mono, got {w.getnchannels()} channels")
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: expected 16-bit, got {w.getsampwidth()*8}")
        rate = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    return Wav(samples=samples, rate=rate, name=os.path.basename(path))


def resample_to(wav: Wav, target_rate: int) -> np.ndarray:
    """Linear resampler good enough for analysis (NOT for playback)."""
    if wav.rate == target_rate:
        return wav.samples.copy()
    ratio = target_rate / wav.rate
    n_out = int(round(len(wav.samples) * ratio))
    if n_out == 0:
        return np.zeros(0, dtype=np.float32)
    x_in = np.arange(len(wav.samples), dtype=np.float64)
    x_out = np.linspace(0, len(wav.samples) - 1, n_out, dtype=np.float64)
    return np.interp(x_out, x_in, wav.samples).astype(np.float32)


# ---------- 1. cross-correlation delay ----------

def cross_correlate_delay_ms(ref: np.ndarray,
                              probe: np.ndarray,
                              rate: int,
                              max_search_s: float = 1.0) -> float:
    """Return the lag (ms) at which `probe` best matches `ref`.

    Positive lag means probe arrives AFTER ref (the speaker -> air -> mic
    path delay, what we expect).
    """
    if len(ref) == 0 or len(probe) == 0:
        return float("nan")
    # Truncate to a common analysable window.  AEC delay budgets are
    # < 250 ms for desktop air, so a 1 s search is generous.
    max_lag_samples = int(max_search_s * rate)
    n = min(len(ref), len(probe))
    n = min(n, 8 * rate)        # 8 s is plenty
    a = ref[:n] - np.mean(ref[:n])
    b = probe[:n] - np.mean(probe[:n])
    # Use FFT-based correlation when scipy is around; otherwise fall
    # back to numpy's mode='full' which is O(N^2) but fine at < 8 s.
    try:
        from scipy.signal import correlate
        c = correlate(b, a, mode="full", method="fft")
    except ImportError:
        c = np.correlate(b, a, mode="full")
    centre = len(a) - 1
    lo = centre - max_lag_samples
    hi = centre + max_lag_samples + 1
    lo = max(lo, 0)
    hi = min(hi, len(c))
    window = c[lo:hi]
    if len(window) == 0:
        return float("nan")
    peak_idx = int(np.argmax(window)) + (lo - centre)
    return peak_idx / rate * 1000.0


# ---------- 2 + 3. per-band energy ratios ----------

def stft(samples: np.ndarray, rate: int, frame_ms: int = 25,
         hop_ms: int = 10) -> tuple[np.ndarray, np.ndarray]:
    """Plain STFT using a Hann window.  Returns (freqs, |X|)."""
    if len(samples) == 0:
        return np.zeros(0), np.zeros((0, 0))
    frame = int(rate * frame_ms / 1000)
    hop = int(rate * hop_ms / 1000)
    if len(samples) < frame:
        return np.zeros(0), np.zeros((0, 0))
    win = np.hanning(frame).astype(np.float32)
    n_frames = 1 + (len(samples) - frame) // hop
    out = np.zeros((n_frames, frame // 2 + 1), dtype=np.float32)
    for i in range(n_frames):
        seg = samples[i * hop : i * hop + frame] * win
        out[i] = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(frame, 1.0 / rate)
    return freqs, out


def band_energy_db(freqs: np.ndarray, mag: np.ndarray,
                    lo: int, hi: int) -> float:
    """Sum |X|^2 inside [lo, hi) Hz across all frames; convert to dB
    with a tiny epsilon to avoid -inf on dead bands."""
    if mag.size == 0:
        return float("-inf")
    mask = (freqs >= lo) & (freqs < hi)
    if not np.any(mask):
        return float("-inf")
    energy = float(np.sum(mag[:, mask] ** 2))
    return 10.0 * np.log10(energy + 1e-20)


def per_band_diff_db(a: np.ndarray, a_rate: int,
                      b: np.ndarray, b_rate: int,
                      bands: list[tuple[int, int]]) -> list[float]:
    """For each band, return energy(b) - energy(a) in dB.

    Negative means b lost energy vs a (attenuation / cancellation).
    """
    fa, ma = stft(a, a_rate)
    fb, mb = stft(b, b_rate)
    out = []
    for lo, hi in bands:
        ea = band_energy_db(fa, ma, lo, hi)
        eb = band_energy_db(fb, mb, lo, hi)
        out.append(eb - ea)
    return out


def rms(x: np.ndarray) -> float:
    if x.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


def level_match(reference: np.ndarray,
                 probe: np.ndarray) -> tuple[np.ndarray, float]:
    """Scale `probe` so its overall RMS matches `reference`.

    Returns (scaled_probe, gain) where gain is the linear scale factor
    applied (>1 means probe was quieter and we boosted; <1 means probe
    was louder and we attenuated).  Used to strip AGC contribution
    before per-band ERLE so the table reflects what AEC alone did.
    """
    r = rms(reference)
    p = rms(probe)
    if p < 1e-9:
        return probe, 1.0
    gain = r / p
    return probe * gain, gain


# ---------- 4. AEC residual ----------

def time_align(ref: np.ndarray, probe: np.ndarray,
                rate: int, lag_ms: float) -> tuple[np.ndarray, np.ndarray]:
    """Shift probe back by lag_ms so it lines up with ref, truncate to
    common length.  Returns (ref_aligned, probe_aligned)."""
    lag_samples = int(round(lag_ms / 1000.0 * rate))
    if lag_samples > 0 and lag_samples < len(probe):
        probe = probe[lag_samples:]
    elif lag_samples < 0 and -lag_samples < len(ref):
        ref = ref[-lag_samples:]
    n = min(len(ref), len(probe))
    return ref[:n], probe[:n]


# ---------- charts (optional) ----------

def write_charts(out_dir: str,
                  orig: Wav, raw: Wav, aec: Wav,
                  delay_ms: float):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    # 01: first 2 s waveforms.
    fig, axes = plt.subplots(3, 1, figsize=(10, 6), sharex=True)
    for ax, w in zip(axes, [orig, raw, aec]):
        n = min(len(w.samples), int(2.0 * w.rate))
        t = np.arange(n) / w.rate
        ax.plot(t, w.samples[:n], linewidth=0.4)
        ax.set_title(f"{w.name}  ({w.rate} Hz)")
        ax.set_ylabel("amp")
        ax.set_ylim(-1.0, 1.0)
    axes[-1].set_xlabel("time (s)")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "01_waveforms.png"), dpi=120)
    plt.close(fig)

    # 02: spectrograms side by side.
    fig, axes = plt.subplots(1, 3, figsize=(15, 4))
    for ax, w in zip(axes, [orig, raw, aec]):
        _, mag = stft(w.samples, w.rate)
        if mag.size == 0:
            ax.set_title(f"{w.name} (empty)")
            continue
        ax.imshow(20 * np.log10(mag.T + 1e-9), origin="lower", aspect="auto",
                  extent=(0, len(w.samples) / w.rate, 0, w.rate / 2),
                  cmap="magma", vmin=-100, vmax=0)
        ax.set_title(f"{w.name}")
        ax.set_xlabel("time (s)")
        ax.set_ylabel("Hz")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "02_spectrogram.png"), dpi=120)
    plt.close(fig)

    # 03: original -> raw attenuation per frequency.
    common_rate = min(orig.rate, raw.rate)
    o16 = resample_to(orig, common_rate)
    r16 = resample_to(raw, common_rate)
    fo, mo = stft(o16, common_rate)
    _, mr = stft(r16, common_rate)
    if mo.size and mr.size:
        n = min(mo.shape[0], mr.shape[0])
        ratio = (np.mean(mr[:n] ** 2, axis=0) + 1e-20) / \
                (np.mean(mo[:n] ** 2, axis=0) + 1e-20)
        fig, ax = plt.subplots(figsize=(10, 4))
        ax.semilogx(fo, 10 * np.log10(ratio))
        ax.set_xlabel("Hz")
        ax.set_ylabel("dB (raw / original)")
        ax.set_title("Per-frequency attenuation: original -> raw")
        ax.grid(True, which="both", alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "03_attenuation.png"), dpi=120)
        plt.close(fig)

    # 04: AEC residual per frequency.
    raw16 = resample_to(raw, aec.rate)
    raw16_a, aec_a = time_align(raw16, aec.samples, aec.rate, lag_ms=delay_ms)
    f1, m_raw = stft(raw16_a, aec.rate)
    _, m_aec = stft(aec_a, aec.rate)
    if m_raw.size and m_aec.size:
        n = min(m_raw.shape[0], m_aec.shape[0])
        ratio_out = (np.mean(m_aec[:n] ** 2, axis=0) + 1e-20) / \
                    (np.mean(m_raw[:n] ** 2, axis=0) + 1e-20)
        # Level-matched: scale aec to raw's overall RMS so AGC is removed
        # from the per-band picture.
        aec_lm_a, _ = level_match(raw16_a, aec_a)
        _, m_aec_lm = stft(aec_lm_a, aec.rate)
        n_lm = min(m_raw.shape[0], m_aec_lm.shape[0])
        ratio_lm = (np.mean(m_aec_lm[:n_lm] ** 2, axis=0) + 1e-20) / \
                   (np.mean(m_raw[:n_lm] ** 2, axis=0) + 1e-20)
        fig, ax = plt.subplots(figsize=(10, 4))
        ax.semilogx(f1, 10 * np.log10(ratio_out),
                     label="as-output (raw -> aec)", color="C0")
        ax.semilogx(f1, 10 * np.log10(ratio_lm),
                     label="level-matched (AEC isolated)", color="C2")
        ax.set_xlabel("Hz")
        ax.set_ylabel("dB")
        ax.set_title("Per-frequency AEC residual (negative = APM cancelled)")
        ax.axhline(-25, color="r", linestyle="--", alpha=0.5,
                    label="-25 dB target")
        ax.axhline(0, color="k", linestyle=":", alpha=0.3)
        ax.grid(True, which="both", alpha=0.3)
        ax.legend()
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "04_aec_residual.png"), dpi=120)
        plt.close(fig)


# ---------- verdict ----------

def verdict(orig_to_raw_db: list[float],
             raw_to_aec_lm_db: list[float],
             raw_to_aec_out_db: list[float],
             inferred_gain_db: float) -> str:
    """Map per-band numbers to the three hypotheses in
    docs/aec_report.md S 6.

    Uses the level-matched table (`raw_to_aec_lm_db`) as the primary
    AEC signal — it strips AGC and reflects what the cancellation
    algorithm actually accomplished.  The as-output table
    (`raw_to_aec_out_db`) shows up only as a separate AGC-masking
    callout when the two tables disagree.
    """
    speech_attn = orig_to_raw_db[1:3]                   # 300-3000 Hz
    erle_lm     = -float(np.mean(raw_to_aec_lm_db[1:3]))   # +ve = cancellation
    erle_out    = -float(np.mean(raw_to_aec_out_db[1:3]))
    attn_avg    = float(np.mean(speech_attn))

    # Detect "our APM is in pass-through" — happens when the user runs
    # with apm.aec_enabled=false (or the all-off short-circuit fires).
    # In passthrough, our APM doesn't change levels at all — total RMS
    # is unchanged.  The 48->16 kHz soxr resampler does add a few dB
    # of high-band rolloff (anti-alias filter near Nyquist), so per-band
    # checks are noisy in the upper bands; total energy is the cleanest
    # signal.
    apm_passthrough = abs(inferred_gain_db) < 1.0

    lines = []

    # 1. System-AEC PASS path: APM is passthrough AND raw is heavily
    #    attenuated below original.  This is the M6B Step 3 result.
    if apm_passthrough and attn_avg < -25.0:
        lines.append(
            f"  M6 acceptance gate 4: PASS via upstream (system) AEC.  "
            f"raw_recording is {-attn_avg:.1f} dB below the speaker "
            f"output in the speech band (300-3000 Hz)."
        )
        lines.append(
            "  Our in-process APM is in pass-through "
            "(aec_enabled=false), so the cancellation came from "
            "upstream — typically PipeWire's module-echo-cancel via "
            "cfg.apm.use_system_aec=true."
        )
        return "\n".join(lines)

    # 2. AGC-mask callout: if the two tables disagree by more than 3 dB
    #    in the speech band, AGC is in the picture.  Tell the user.
    if abs(erle_lm - erle_out) > 3.0:
        lines.append(
            f"  AGC is masking the AEC measurement: as-output ERLE = "
            f"{erle_out:+.1f} dB but level-matched ERLE = {erle_lm:+.1f} "
            f"dB (delta {erle_lm - erle_out:+.1f} dB)."
        )
        lines.append(
            "  Re-run with `ACVA_AEC_NO_POSTPROC=1 ./acva demo aec-record` "
            "for a clean AEC-only measurement."
        )

    # 3. Acoustic-path failure: ground-truth attenuation > 30 dB means
    #    the signal isn't reaching APM with enough fidelity to cancel.
    #    (Skipped above for the apm_passthrough+attn case — that's the
    #    system-AEC PASS path, not codec failure.)
    if attn_avg < -30.0:
        lines.append(
            "  codec/PA chain attenuates the speaker signal by "
            f"{-attn_avg:.1f} dB in the speech band before APM ever "
            "sees it.  APM has no acoustic signal to cancel."
        )
        lines.append(
            "  Recommendation: try `acva demo aec-record` with a USB "
            "microphone (cfg.audio.input_device contains 'USB')."
        )

    # 3. AEC verdict from the level-matched table.
    if erle_lm >= 25.0:
        lines.append(
            f"  AEC subsystem is cancelling well in the speech band "
            f"(level-matched ERLE {erle_lm:+.1f} dB).  M6 acceptance "
            f"gate 4 looks satisfied."
        )
    elif erle_lm >= 10.0:
        lines.append(
            f"  AEC subsystem is cancelling partially (level-matched "
            f"ERLE {erle_lm:+.1f} dB).  Below the 25 dB acceptance bar; "
            f"try lower amplitude or a USB mic."
        )
    else:
        if attn_avg >= -30.0:
            lines.append(
                f"  speaker -> mic path is intact (band attenuation "
                f"{-attn_avg:+.1f} dB), but the AEC subsystem is "
                f"achieving only {erle_lm:+.1f} dB level-matched ERLE."
            )
            lines.append(
                "  Likely contributors: speaker non-linearity at high "
                "amplitude, APM delay misalignment, or NS reconstruction "
                "artifacts.  Things to try, in order:"
            )
            lines.append(
                "    - re-run with ACVA_AEC_NO_POSTPROC=1 (isolates AEC "
                "from NS/AGC reshaping the spectrum)"
            )
            lines.append(
                "    - lower the speaker amplitude (defeats class-D DRC)"
            )
            lines.append(
                "    - swap to a USB mic (bypasses laptop codec DSP)"
            )
            lines.append(
                "    - check /metrics voice_aec_delay_estimate_ms — if it "
                "is at the cfg.apm.max_delay_ms ceiling, raise the ceiling"
            )

    # 4. Net-output footnote.
    if inferred_gain_db < -3.0:
        lines.append(
            f"  net result: aec_recording is {-inferred_gain_db:.1f} dB "
            f"LOUDER than raw (AGC overshoot)."
        )
    elif inferred_gain_db > 3.0:
        lines.append(
            f"  net result: aec_recording is {inferred_gain_db:.1f} dB "
            f"quieter than raw."
        )

    return "\n".join(lines)


# ---------- main ----------

def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out_dir", nargs="?", default="/tmp/acva-aec-rec",
                    help="directory written by `acva demo aec-record`")
    p.add_argument("--no-charts", action="store_true",
                    help="skip matplotlib chart generation")
    args = p.parse_args(argv)

    out_dir = args.out_dir
    paths = {
        "original":  os.path.join(out_dir, "original.wav"),
        "raw":       os.path.join(out_dir, "raw_recording.wav"),
        "aec":       os.path.join(out_dir, "aec_recording.wav"),
    }
    for name, path in paths.items():
        if not os.path.isfile(path):
            sys.stderr.write(f"aec_analyze: missing {name}: {path}\n")
            return 1

    orig = read_wav(paths["original"])
    raw  = read_wav(paths["raw"])
    aec  = read_wav(paths["aec"])

    # -------- file summaries --------
    print(f"{orig.name:22s}: {orig.duration_s:5.2f} s, "
          f"{orig.rate} Hz, RMS {orig.rms*32768:6.0f}, peak {orig.peak*32768:6.0f}")
    print(f"{raw.name:22s}: {raw.duration_s:5.2f} s, "
          f"{raw.rate} Hz, RMS {raw.rms*32768:6.0f}, peak {raw.peak*32768:6.0f}")
    print(f"{aec.name:22s}: {aec.duration_s:5.2f} s, "
          f"{aec.rate} Hz, RMS {aec.rms*32768:6.0f}, peak {aec.peak*32768:6.0f}")
    print()

    # -------- 1. cross-correlation cold-start offset --------
    # NB: this includes the playback queue prefill (cfg.playback.prefill_ms,
    # default 100 ms) + PortAudio output buffer + actual air delay.  It is
    # NOT the AEC-relevant acoustic delay.  APM has its own adaptive
    # estimator that converges to the air-only component (~28 ms on the
    # dev workstation, per `acva demo aec-hw`).
    common = min(orig.rate, raw.rate)
    o = resample_to(orig, common)
    r = resample_to(raw,  common)
    delay_ms = cross_correlate_delay_ms(o, r, common)
    print(f"cold-start offset (original -> raw, x-corr peak): {delay_ms:6.1f} ms")
    print("  (queue prefill + output buffer + air; NOT the AEC delay —")
    print("   see /metrics voice_aec_delay_estimate_ms for that)")
    print()

    # -------- 2. per-band attenuation: original -> raw --------
    orig_to_raw = per_band_diff_db(orig.samples, orig.rate,
                                    raw.samples, raw.rate, BANDS_HZ)
    print("per-band attenuation (original -> raw, dB):")
    for (lo, hi), db in zip(BANDS_HZ, orig_to_raw):
        marker = "  <-- heavy"  if db < -30.0 else ""
        print(f"  {lo:4d}-{hi:4d} Hz  : {db:+6.1f} dB{marker}")
    print()

    # -------- 3. per-band ERLE: raw -> aec --------
    # Two tables because AGC, when enabled, lifts the cleaned signal back
    # up by 5-9 dB and obscures whether AEC is actually cancelling:
    #   (a) AS-OUTPUT — what production downstream (VAD, STT) sees
    #   (b) LEVEL-MATCHED — strip AGC by RMS-matching aec to raw
    #       first; isolates AEC's per-band contribution
    # If they agree, AGC is not in the picture.  If (a) shows positive dB
    # but (b) shows negative dB, AEC IS cancelling but AGC is putting
    # the energy back.
    raw_at_aec = resample_to(raw, aec.rate)

    raw_to_aec_out = per_band_diff_db(raw_at_aec, aec.rate,
                                       aec.samples, aec.rate, BANDS_HZ)
    aec_lm, lm_gain = level_match(raw_at_aec, aec.samples)
    raw_to_aec_lm = per_band_diff_db(raw_at_aec, aec.rate,
                                      aec_lm, aec.rate, BANDS_HZ)

    inferred_gain_db = 20.0 * np.log10(max(lm_gain, 1e-9))
    print(f"inferred APM linear gain (raw / aec): {inferred_gain_db:+6.1f} dB")
    print(f"  (positive = APM net-reduced energy;  "
          f"negative = AGC added more than AEC subtracted)")
    print()

    def _emit(label: str, table: list[float]):
        print(label)
        for (lo, hi), db in zip(BANDS_HZ, table):
            marker = ""
            if db < -25.0:
                marker = "  <-- gate 4 ok"
            elif db < -10.0:
                marker = "  <-- partial"
            elif db > -1.0:
                marker = "  <-- no cancellation"
            print(f"  {lo:4d}-{hi:4d} Hz  : {db:+6.1f} dB{marker}")

    _emit("per-band APM as-output (raw -> aec, dB; negative = cancelled):",
          raw_to_aec_out)
    print()
    _emit("per-band APM level-matched (raw -> aec_scaled, isolates AEC from AGC):",
          raw_to_aec_lm)
    print()

    print("verdict:")
    print(verdict(orig_to_raw, raw_to_aec_lm, raw_to_aec_out, inferred_gain_db))

    # -------- charts --------
    if args.no_charts:
        pass
    elif _have_matplotlib():
        write_charts(out_dir, orig, raw, aec, delay_ms)
        print(f"\ncharts written to {out_dir}/0[1-4]_*.png")
    else:
        print("\n(matplotlib not installed; skipping charts)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
