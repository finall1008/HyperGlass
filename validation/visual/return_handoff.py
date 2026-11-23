"""Consolidated 10-case SF return handoff and wallpaper regression runner."""
import argparse
from contextlib import ExitStack, nullcontext
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import time

import numpy as np
from PIL import Image, ImageDraw

from capture import (
    HOME,
    SOURCE_TIMING,
    VIDEO_HEIGHT,
    VIDEO_WIDTH,
    Emulator,
    decode,
    decode_crops,
    settling_difference,
)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROFILE = json.loads((HERE / "scenes.json").read_text())
SERIAL = "emulator-5580"
SETTINGS = "com.android.settings"
CALCULATOR = "com.miui.calculator"
TAP = ("tap", *PROFILE["scenes"]["icon"]["tap"])
SWIPE = ("swipe", *PROFILE["home_gesture"])
BOX = (426, 815, 536, 919)
PLATE = {
    "left": (439, 845, 443, 853),
    "right": (508, 856, 513, 867),
    "top": (460, 828, 474, 832),
    "bottom": (458, 906, 477, 911),
}
CONTROL = (
    (426, 835, 433, 908),
    (525, 833, 536, 909),
    (437, 815, 519, 825),
    (437, 915, 519, 919),
)
LUMA = np.array([0.2126, 0.7152, 0.0722])
LUMA_LIMIT = 12.0
CHROMA_LIMIT = 22.0
CASES = (
    "folder-boundary",
    "unpaired-external",
    "page-return",
    "home-key-tap",
    "home-key-press",
    "home-key-unpaired",
    "mid-open-gesture",
    "mid-open-home-key",
    "wallpaper-after-mid-open",
    "wallpaper-after-recents",
)


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")


class HandoffEmulator(Emulator):
    """Emulator wrapper supporting fast top-activity checks for return flows."""

    def wait_top(self, package, seconds=10):
        until = time.monotonic() + seconds
        actual = "none"
        while time.monotonic() < until:
            activities = self.shell("dumpsys", "activity", "activities")
            display = re.search(r"(?ms)^Display #0\b(.*?)(?=^Display #\d+\b|\Z)", activities)
            if display:
                resumed = re.search(
                    r"(?m)^\s*(?:topResumedActivity=|Resumed: )ActivityRecord\{[^\n]* ([\w.]+)/",
                    display[1],
                )
                actual = resumed[1] if resumed else "none"
                if actual == package:
                    return
            time.sleep(0.16)
        raise RuntimeError(f"Expected {package} on display 0, observed {actual}")


def crop_array(images, box):
    x0, y0, x1, y1 = box
    return images[:, y0 - BOX[1]:y1 - BOX[1], x0 - BOX[0]:x1 - BOX[0]]


def settings_glyph(images):
    glyph = np.min(crop_array(images, (438, 827, 515, 908)), axis=-1) >= 145
    glyph[:, :23, 60:] = False
    return glyph


def baseline_glyph():
    rest = np.asarray(Image.open(HERE / "baseline/icon-rest.png").convert("RGB"))
    return settings_glyph(rest[:BOX[3] - BOX[1]][None, ...])[0]


def dice(left, right, axes):
    denom = np.maximum(np.count_nonzero(left, axis=axes) + np.count_nonzero(right, axis=axes), 1)
    return 2.0 * np.count_nonzero(left & right, axis=axes) / denom


def elapsed_timestamps(video, pts):
    data = video.read_bytes()
    magic = b"#VV1NSC0PET1ME!#"
    start = data.rfind(magic)
    if start < 0:
        return None
    count = struct.unpack_from("<I", data, start + len(magic))[0]
    if count != len(pts):
        raise ValueError(f"Screenrecord metadata has {count} timestamps for {len(pts)} frames")
    values = np.frombuffer(data, dtype="<u8", count=count, offset=start + len(magic) + 4) / 1000.0
    relative = np.array([float(frame["best_effort_timestamp_time"]) for frame in pts]) * 1000.0
    if np.max(np.abs(values - values[0] - (relative - relative[0]))) > 3.0:
        raise ValueError("Screenrecord elapsed times differ from relative frame timestamps")
    return values


def probe_video(video, reference):
    """Extract per-frame glyph overlap, stability and plate step deltas."""
    raw_motion = decode(
        video, "-vf", "scale=140:304:flags=area", *SOURCE_TIMING,
        "-an", "-pix_fmt", "rgb24", "-f", "rawvideo", "-",
    )
    motion = np.frombuffer(raw_motion, dtype=np.uint8).reshape(-1, 304, 140, 3)
    x0, y0, x1, y1 = BOX
    raw_crops = decode(
        video, "-vf", f"format=rgb24,crop={x1 - x0}:{y1 - y0}:{x0}:{y0}",
        *SOURCE_TIMING, "-an", "-pix_fmt", "rgb24", "-f", "rawvideo", "-",
    )
    images = np.frombuffer(raw_crops, dtype=np.uint8).reshape(-1, y1 - y0, x1 - x0, 3)
    if len(images) != len(motion) or len(images) < 10:
        raise ValueError("Source frame count is inconsistent or too short")
    pts = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
        "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(video),
    ], timeout=15))["frames"]
    if len(pts) != len(images):
        raise ValueError("Frame timestamps do not match decoded source frames")

    elapsed = elapsed_timestamps(video, pts)
    home = np.median(motion[-8:, 20:-18], axis=0).astype(np.int16)
    distance = settling_difference(motion, home, BOX)
    glyph = settings_glyph(images)
    settled_glyph = np.median(glyph[-8:], axis=0) > 0.5
    settled_overlap = float(dice(settled_glyph, reference, (0, 1)))
    overlap = dice(glyph, settled_glyph[None, ...], (1, 2))
    similarity = dice(glyph[:-1], glyph[1:], (1, 2))

    rgb = np.stack([np.median(crop_array(images, box), axis=(1, 2)) for box in PLATE.values()], axis=1)
    luma = rgb @ LUMA
    chroma = np.stack((rgb[..., 0] - luma, rgb[..., 2] - luma), axis=-1)
    step_luma = np.max(np.abs(np.diff(luma, axis=0)), axis=1)
    step_chroma = np.max(np.linalg.norm(np.diff(chroma, axis=0), axis=-1), axis=1)

    controls = [crop_array(images, box).astype(np.int16) for box in CONTROL]
    control_diff = [np.abs(np.diff(box, axis=0)) for box in controls]
    control_mean = np.max([delta.mean(axis=(1, 2, 3)) for delta in control_diff], axis=0)
    control_changed = np.max([(delta.max(axis=-1) > 16).mean(axis=(1, 2)) for delta in control_diff], axis=0)

    settled_scene = (distance[:-1] <= 0.005) & (distance[1:] <= 0.005)
    possible = (
        (overlap[:-1] >= 0.84)
        & (overlap[1:] >= 0.84)
        & ((similarity >= 0.93) | (settled_scene & (similarity >= 0.84)))
        & (control_mean <= 2.0)
        & (control_changed <= 0.03)
        & (np.maximum.accumulate(distance)[:-1] >= 0.16)
    )
    if settled_overlap < 0.80:
        possible[:] = False

    records = []
    for i in range(len(images)):
        records.append({
            "frame": i,
            "pts_seconds": float(pts[i]["best_effort_timestamp_time"]),
            "screenrecord_elapsed_ms": None if elapsed is None else round(float(elapsed[i]), 3),
            "outside_difference_to_home": round(float(distance[i]), 5),
            "wallpaper_control_mean_delta": round(float(control_mean[i - 1]), 5) if i else None,
            "wallpaper_control_changed_fraction": round(float(control_changed[i - 1]), 5) if i else None,
            "glyph_overlap_with_home": round(float(overlap[i]), 4),
            "glyph_overlap_with_previous": round(float(similarity[i - 1]), 4) if i else None,
            "luma_delta": round(float(step_luma[i - 1]), 2) if i else None,
            "chroma_delta": round(float(step_chroma[i - 1]), 2) if i else None,
            "handoff_pair": bool(i and possible[i - 1]),
        })
    candidates = np.flatnonzero(possible)
    score = np.maximum(step_luma / LUMA_LIMIT, step_chroma / CHROMA_LIMIT)
    flagged_all = [[int(i), int(i + 1)] for i in candidates if score[i] >= 1.0]
    return {
        "video": str(video),
        "settled_reference_overlap": round(settled_overlap, 4),
        "eligible_pairs": int(len(candidates)),
        "flagged_all_pairs": flagged_all,
        "max_luma_all": None if not len(candidates) else round(float(step_luma[candidates].max()), 2),
        "max_chroma_all": None if not len(candidates) else round(float(step_chroma[candidates].max()), 2),
        "frames": records,
    }, images


def edge_crossing(values, offset, falling=False):
    if falling:
        values = -values
    indices = np.flatnonzero((values[:-1] <= 0) & (values[1:] > 0))
    if not len(indices):
        return None
    slopes = values[indices + 1] - values[indices]
    index = int(indices[np.argmax(slopes)])
    return round(float(offset + index - values[index] / (values[index + 1] - values[index])), 2)


def wall_edges(image):
    img = image.astype(np.float32)
    left = np.mean(img[840:899, 439:443], axis=1)
    bottom = np.mean(img[899:905, 450:508], axis=0)
    exterior_v = np.mean(img[851:915, 411:425], axis=1)
    exterior_h = np.mean(img[802:810, 445:521], axis=0)
    return {
        "plate_x": edge_crossing(bottom[:, 1] - bottom[:, 2], 450),
        "plate_y": edge_crossing(left[:, 0] - left[:, 2], 840, falling=True),
        "outside_x": edge_crossing(exterior_h[:, 1] - exterior_h[:, 0], 445),
        "outside_y": edge_crossing(exterior_v[:, 0] - exterior_v[:, 2], 851, falling=True),
    }


def save_pair_evidence(directory, video, crops, pair, prefix):
    i, j = pair
    full = decode_crops(video, [i, j], (0, 0, VIDEO_WIDTH, VIDEO_HEIGHT))
    for frame_idx, image in zip((i, j), full, strict=True):
        Image.fromarray(image).save(directory / f"{prefix}-{frame_idx:03}.png")
    delta = np.abs(crops[j].astype(np.int16) - crops[i])
    Image.fromarray(np.minimum(255, delta * 4).astype(np.uint8)).resize(
        (delta.shape[1] * 4, delta.shape[0] * 4), Image.Resampling.NEAREST,
    ).save(directory / f"{prefix}-diff-{i:03}-{j:03}-4x.png")
    first, last = wall_edges(full[0]), wall_edges(full[1])
    change = {
        key: round(last[key] - val, 2) if val is not None and last[key] is not None else None
        for key, val in first.items()
    }
    return {"pair": [i, j], "before": first, "after": last, "change": change}


def marker(emulator, tag, event):
    text = emulator.shell(
        "sh", "-c",
        f'read t rest < /proc/uptime; log -t HG_REPRO "{tag} {event} uptime=$t"; echo "$t"',
    )
    return float(text.strip())


def wait_for_event(log, pattern, timeout=2.5):
    until = time.monotonic() + timeout
    with log.open(errors="replace") as source:
        while time.monotonic() < until:
            for line in source:
                if pattern in line:
                    return line.rstrip()
            time.sleep(0.004)
    raise RuntimeError(f"Missing launcher event: {pattern}")


def timed_record(emulator, directory, label, action, record_seconds):
    remote = f"/data/local/tmp/hg-handoff-{os.getpid()}-{directory.name}.mp4"
    clock = {}
    tags = ("HyperGlass:V", "flutter:V", "HG_REPRO:V", "AndroidRuntime:E")
    log_file = directory / "transition.log"
    with log_file.open("wb") as output:
        collector = subprocess.Popen(
            emulator.adb + ["logcat", "-T", "1", "-v", "threadtime", "-v", "monotonic", "-v", "usec", "-s", *tags],
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
        )
        recorder = None
        try:
            clock["start_uptime"] = marker(emulator, label, "START")
            cmd = emulator.root(
                "screenrecord", "--verbose", "--display-id", emulator.display, "--size",
                f"{VIDEO_WIDTH}x{VIDEO_HEIGHT}", "--bit-rate", 16000000, "--time-limit", record_seconds, remote,
            )
            recorder = subprocess.Popen(
                emulator.adb + ["shell", cmd],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            time.sleep(0.65)
            action(clock, log_file)
            console, _ = recorder.communicate(timeout=record_seconds + 10)
            (directory / "screenrecord.log").write_bytes(console)
            if recorder.returncode:
                raise RuntimeError(f"screenrecord exited {recorder.returncode}")
            (directory / "return.mp4").write_bytes(
                emulator.run("exec-out", emulator.root("cat", remote), timeout=18),
            )
            clock["end_uptime"] = marker(emulator, label, "END")
            write_json(directory / "clock.json", clock)
            if collector.poll() is not None:
                raise RuntimeError(f"logcat exited {collector.returncode}")
        finally:
            if recorder and recorder.poll() is None:
                recorder.kill()
                recorder.wait(timeout=5)
            try:
                emulator.shell("rm", "-f", remote)
            finally:
                time.sleep(0.1)
                collector.terminate()
                collector.wait(timeout=5)


def parse_events(log_path):
    events = {}
    patterns = {
        "hide": "onAnimStart: hideIcon called",
        "placeholder": "[AppIconLaunchWrapper] placeholderBuilder branch=normal",
        "open_end": "Signal sent: SceneAnimationSignalType.openingRemoteAnimationClose",
        "back_interrupt": "reuseWindowTransition type=WindowReuseType.backInterrupt",
        "open_interrupted": "AnimType.OPEN_FROM_HOME interrupted=true",
        "pop": "Driver startPop.",
        "create_draw": "FloatingIconLayer createAndDraw all ",
        "sf_committed": "onReleaseCommitted: SF confirmed",
        "close_end_signal": "Signal sent: SceneAnimationSignalType.closingRemoteAnimationClose",
        "close_completed": "AnimType.CLOSE_TO_HOME interrupted=false",
        "settings_close": "uniqueTag=com.android.settings/",
        "enter_overview": "enterOverviewState",
        "exit_overview": "exitOverviewState",
    }
    for line in log_path.read_text(errors="replace").splitlines():
        stamp = re.match(r"^\s*(\d+\.\d+)\s+", line)
        if not stamp:
            continue
        ms = float(stamp[1]) * 1000.0
        for key, pat in patterns.items():
            if pat in line:
                events.setdefault(key, []).append({"ms": ms, "text": line.strip()})
    return events


def evaluate_sf_window(result, sf_ms_list):
    """Evaluate jumps strictly inside the SF release confirmation window [-40, +120] ms."""
    aligned_pairs = []
    for release_ms in sf_ms_list:
        for row in result["frames"][1:]:
            t = row["screenrecord_elapsed_ms"]
            if row["handoff_pair"] and t is not None and release_ms - 40.0 <= t <= release_ms + 120.0:
                aligned_pairs.append(row["frame"])
    aligned_set = set(aligned_pairs)
    window_rows = [row for row in result["frames"] if row["frame"] in aligned_set]
    flagged_in_window = [
        [row["frame"] - 1, row["frame"]]
        for row in window_rows
        if row["luma_delta"] >= LUMA_LIMIT or row["chroma_delta"] >= CHROMA_LIMIT
    ]
    max_luma = max((row["luma_delta"] for row in window_rows), default=None)
    max_chroma = max((row["chroma_delta"] for row in window_rows), default=None)
    peak_row = max(
        window_rows,
        key=lambda row: max(row["luma_delta"] / LUMA_LIMIT, row["chroma_delta"] / CHROMA_LIMIT),
        default=None,
    )
    return {
        "aligned_count": len(window_rows),
        "aligned_frames": sorted(aligned_set),
        "peak_frame": None if peak_row is None else peak_row["frame"],
        "window_jumps": flagged_in_window,
        "window_max_luma": max_luma,
        "window_max_chroma": max_chroma,
    }


def latest_return_capture_and_latency(events, after_ms):
    pops = [e["ms"] for e in events.get("pop", []) if e["ms"] >= after_ms]
    if not pops:
        return None, None
    pop_ms = pops[-1]
    draws = [e for e in events.get("create_draw", []) if e["ms"] >= pop_ms]
    if not draws:
        return None, None
    first_draw = draws[0]
    size = first_draw["text"].split("createAndDraw all ", 1)[1].split()[0]
    return size, round(first_draw["ms"] - pop_ms, 3)


def launch_settings(emulator):
    emulator.shell("am", "start", "--display", 0, "-a", "android.settings.SETTINGS", "-p", SETTINGS)


def press_settings(emulator):
    x, y = TAP[1:]
    emulator.shell(
        "sh", "-c",
        f"input touchscreen -d 0 motionevent DOWN {x} {y}; sleep 0.09; "
        f"input touchscreen -d 0 motionevent UP {x} {y}",
    )


def build_quadrants_wallpaper(path, size):
    colors = Image.new("RGB", size)
    draw = ImageDraw.Draw(colors)
    boxes = ((0, 0, 971, 1767), (972, 0, 1119, 1767), (0, 1768, 971, 2435), (972, 1768, 1119, 2435))
    quadrants = ((207, 57, 46), (54, 182, 85), (48, 107, 192), (216, 181, 58))
    for box, color in zip(boxes, quadrants, strict=True):
        draw.rectangle(box, fill=color)
    colors.save(path)
    return path


def wait_settings_icon(emulator, reference, out_dir, name="missing-target.png"):
    emulator.wait_page(1, seconds=8)
    until = time.monotonic() + 10.0
    while True:
        shot = emulator.screenshot()
        glyph = settings_glyph(np.asarray(shot.crop(BOX))[None, ...])[0]
        if dice(glyph, reference, (0, 1)) >= 0.80:
            return
        if time.monotonic() >= until:
            shot.save(out_dir / name)
            raise RuntimeError("Settings glyph unavailable on page 1")
        time.sleep(0.25)


def run_single_attempt(emulator, directory, reference, case_name):
    directory.mkdir(parents=True, exist_ok=True)
    record_seconds = 13 if case_name == "folder-boundary" else 8

    def action(clock, log):
        if case_name == "folder-boundary":
            emulator.touch(*TAP)
            emulator.wait_top(SETTINGS, seconds=4)
            emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_HOME")
            emulator.wait_top(HOME, seconds=4)
            time.sleep(0.5)
            folder = PROFILE["scenes"]["folder"]
            emulator.touch(*folder["enter"][0])
            time.sleep(0.30)
            emulator.touch("tap", *folder["tap"])
            emulator.wait_top(folder["package"], seconds=4)
            emulator.touch(*SWIPE)
            emulator.wait_top(HOME, seconds=3)
            time.sleep(0.44)
            emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_BACK")
            time.sleep(0.25)
            launch_settings(emulator)
            emulator.wait_top(SETTINGS, seconds=4)
            clock["final_return_ms"] = marker(emulator, case_name, "FINAL_SWIPE") * 1000.0
            emulator.touch(*SWIPE)
        elif case_name == "unpaired-external":
            emulator.touch("tap", 690, 1460)
            emulator.wait_top(CALCULATOR, seconds=4)
            launch_settings(emulator)
            emulator.wait_top(SETTINGS, seconds=4)
            time.sleep(0.18)
            clock["final_return_ms"] = marker(emulator, case_name, "FINAL_SWIPE") * 1000.0
            emulator.touch(*SWIPE)
        elif case_name == "page-return":
            x0, y0, x1, y1, duration = PROFILE["next_page"]
            emulator.touch("swipe", x0, y0, x1, y1, duration)
            time.sleep(0.26)
            emulator.touch("swipe", x1, y1, x0, y0, duration)
            time.sleep(0.08)
            emulator.touch(*TAP)
            return_at = time.monotonic() + 0.35
            emulator.wait_top(SETTINGS, seconds=3)
            clock["settings_resumed_ms"] = marker(emulator, case_name, "SETTINGS_RESUMED") * 1000.0
            time.sleep(max(0.0, return_at - time.monotonic()))
            clock["final_return_ms"] = marker(emulator, case_name, "FINAL_SWIPE") * 1000.0
            emulator.touch(*SWIPE)
        elif case_name in ("home-key-tap", "home-key-press", "home-key-unpaired"):
            if case_name == "home-key-tap":
                emulator.touch(*TAP)
            elif case_name == "home-key-press":
                press_settings(emulator)
            else:
                launch_settings(emulator)
            emulator.wait_top(SETTINGS, seconds=4)
            wait_for_event(log, "Signal sent: SceneAnimationSignalType.openingRemoteAnimationClose")
            time.sleep(0.85)
            clock["final_return_ms"] = marker(emulator, case_name, "KEYCODE_HOME") * 1000.0
            emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_HOME")
        elif case_name in ("mid-open-gesture", "mid-open-home-key"):
            press_settings(emulator)
            wait_for_event(log, "onAnimStart: hideIcon called")
            wait_for_event(log, "[AppIconLaunchWrapper] placeholderBuilder branch=normal")
            time.sleep(0.06)
            clock["final_return_ms"] = marker(emulator, case_name, "MID_OPEN_RETURN") * 1000.0
            if case_name == "mid-open-gesture":
                emulator.touch(*SWIPE)
            else:
                emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_HOME")
        else:
            raise ValueError(f"Unsupported single-attempt case {case_name}")
        emulator.wait_top(HOME, seconds=4)

    timed_record(emulator, directory, case_name, action, record_seconds)
    video = directory / "return.mp4"
    probe_res, crops = probe_video(video, reference)
    events = parse_events(directory / "transition.log")
    clock = json.loads((directory / "clock.json").read_text())
    final_ms = clock["final_return_ms"]

    sf_after = [e["ms"] for e in events.get("sf_committed", []) if e["ms"] >= final_ms]
    window = evaluate_sf_window(probe_res, sf_after[-1:] if case_name == "folder-boundary" else sf_after)
    capture_size, draw_latency_ms = latest_return_capture_and_latency(events, final_ms)

    reasons = []
    if probe_res["settled_reference_overlap"] < 0.80:
        reasons.append(f"settled_overlap={probe_res['settled_reference_overlap']}")
    if not sf_after:
        reasons.append("missing sf_committed after final return")
    if window["aligned_count"] == 0:
        reasons.append("no aligned handoff frames near sf_committed")
    if window["window_jumps"]:
        reasons.append(f"jump in SF handoff window: {window['window_jumps']}")
    if capture_size != "171x171":
        reasons.append(f"unexpected return capture_size={capture_size}")

    release_edges = None
    if sf_after:
        release_ms = sf_after[-1]
        pre_frames = [
            f["frame"] for f in probe_res["frames"]
            if f["screenrecord_elapsed_ms"] is not None
            and release_ms - 90.0 <= f["screenrecord_elapsed_ms"] < release_ms
            and f["glyph_overlap_with_home"] >= 0.84
        ]
        if not pre_frames:
            reasons.append("missing pre-release plate frame [-90, 0) ms")
        else:
            release_edges = save_pair_evidence(
                directory, video, crops, [pre_frames[-1], len(probe_res["frames"]) - 2], "release-edges",
            )
            for axis in ("plate_x", "plate_y"):
                before_val = release_edges["before"].get(axis)
                after_val = release_edges["after"].get(axis)
                delta = release_edges["change"].get(axis)
                if before_val is None or after_val is None or delta is None or abs(delta) > 3.0:
                    reasons.append(
                        f"pre-release plate edge mismatch ({axis}: before={before_val}, after={after_val}, delta={delta})"
                    )

    if case_name == "page-return":
        close_ok = any(
            "AnimType.CLOSE_TO_HOME" in e["text"] and "interrupted=false" in e["text"]
            and e["ms"] >= final_ms and e["ms"] <= clock["end_uptime"] * 1000.0 + 30.0
            for e in events.get("settings_close", [])
        )
        if not close_ok:
            reasons.append("missing Settings CLOSE_TO_HOME evidence")
    elif case_name in ("home-key-tap", "home-key-press", "home-key-unpaired"):
        if not any(e["ms"] >= final_ms for e in events.get("close_end_signal", [])):
            reasons.append("missing closingRemoteAnimationClose signal")
    elif case_name in ("mid-open-gesture", "mid-open-home-key"):
        hides = events.get("hide", [])
        placeholders = events.get("placeholder", [])
        interrupted = events.get("open_interrupted", [])
        open_ends = events.get("open_end", [])
        midflight = bool(
            hides and placeholders and interrupted
            and any(hides[0]["ms"] <= p["ms"] < interrupted[0]["ms"] for p in placeholders)
            and not any(oe["ms"] < interrupted[0]["ms"] for oe in open_ends)
        )
        if not midflight:
            reasons.append("did not hit mid-flight OPEN_FROM_HOME interrupted=true before opening end")
        if case_name == "mid-open-home-key" and not events.get("back_interrupt"):
            reasons.append("missing WindowReuseType.backInterrupt")
        if not events.get("close_completed"):
            reasons.append("missing CLOSE_TO_HOME completion")

    summary = {
        "case": case_name,
        "pass": not reasons,
        "reasons": reasons,
        "capture_size": capture_size,
        "start_pop_to_draw_ms": draw_latency_ms,
        **window,
        "max_luma_all": probe_res["max_luma_all"],
        "max_chroma_all": probe_res["max_chroma_all"],
        "flagged_all_pairs": probe_res["flagged_all_pairs"],
        "settled_reference_overlap": probe_res["settled_reference_overlap"],
    }
    if release_edges is not None:
        summary["release_edges"] = release_edges
    if window["peak_frame"] is not None:
        peak_f = window["peak_frame"]
        summary["handoff_edges"] = save_pair_evidence(directory, video, crops, [peak_f - 1, peak_f], "handoff")
    write_json(directory / "frames.json", probe_res)
    write_json(directory / "events.json", events)
    write_json(directory / "result.json", summary)
    return summary, probe_res, events, crops


def run_wallpaper_case(emulator, directory, reference, base_wallpaper, case_name):
    """Test wallpaper refresh acceptance after mid-open interrupt or Recents return."""
    directory.mkdir(parents=True, exist_ok=True)
    mode = "mid-open" if case_name == "wallpaper-after-mid-open" else "recents"

    # Trigger precondition sequence on the current desktop.
    if mode == "mid-open":
        pre = retry_case(emulator, directory / "precondition", reference, "mid-open-home-key")
        if not pre["pass"]:
            return {
                "case": case_name,
                "pass": False,
                "reasons": [f"precondition mid-open-home-key failed: {pre['reasons']}"],
            }
        dy, dx = 10, 32
    else:
        pre_dir = directory / "precondition"
        pre_dir.mkdir(parents=True, exist_ok=True)

        def recents_action(_clock, log):
            emulator.touch(*TAP)
            emulator.wait_top(SETTINGS, seconds=4)
            wait_for_event(log, "Signal sent: SceneAnimationSignalType.openingRemoteAnimationClose")
            emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_APP_SWITCH")
            time.sleep(0.65)
            emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_HOME")
            emulator.wait_top(HOME, seconds=4)

        timed_record(emulator, pre_dir, case_name + "-recents", recents_action, 8)
        pre_events = parse_events(pre_dir / "transition.log")
        if not pre_events.get("exit_overview"):
            return {
                "case": case_name,
                "pass": False,
                "reasons": ["precondition recents return did not emit exitOverviewState"],
            }
        dy, dx = -10, -32

    source = np.array(Image.open(base_wallpaper).convert("RGB"))
    shifted = source[
        np.clip(np.arange(source.shape[0]) - dy, 0, source.shape[0] - 1)[:, None],
        np.clip(np.arange(source.shape[1]) - dx, 0, source.shape[1] - 1)[None, :],
    ]
    fixture = directory / "shifted-wallpaper.png"
    Image.fromarray(shifted).save(fixture)
    backup = directory / "restore-wallpaper.png"

    with emulator.test_wallpaper(fixture, backup):
        time.sleep(0.6)
        wait_settings_icon(emulator, reference, directory)
        summary, probe_res, events, crops = run_single_attempt(
            emulator, directory / "unpaired-return", reference, "home-key-unpaired",
        )
        reasons = list(summary["reasons"])
        plate_edges = None
        sf_list = events.get("sf_committed", [])
        if sf_list:
            release_ms = sf_list[-1]["ms"]
            pre_frames = [
                f["frame"] for f in probe_res["frames"]
                if f["screenrecord_elapsed_ms"] is not None
                and release_ms - 90.0 <= f["screenrecord_elapsed_ms"] < release_ms
                and f["glyph_overlap_with_home"] >= 0.84
            ]
            settled_frame = len(probe_res["frames"]) - 2
            if not pre_frames:
                reasons.append("missing pre-release plate frame [-90, 0) ms")
            else:
                plate_edges = save_pair_evidence(
                    directory / "unpaired-return",
                    directory / "unpaired-return/return.mp4",
                    crops,
                    [pre_frames[-1], settled_frame],
                    "wallpaper-check",
                )
                for axis in ("plate_x", "plate_y"):
                    delta = plate_edges["change"].get(axis)
                    if delta is None or abs(delta) > 3.0:
                        reasons.append(f"wallpaper not updated before release ({axis} delta={delta})")
        result = {
            "case": case_name,
            "pass": not reasons,
            "reasons": reasons,
            "wallpaper_sha256": list(emulator.wallpaper_hashes()),
            "plate_edges": plate_edges,
            "unpaired_return": summary,
        }
        write_json(directory / "result.json", result)
        return result


def retry_case(emulator, base_dir, reference, case_name, max_attempts=3):
    last = None
    for attempt in range(1, max_attempts + 1):
        wait_settings_icon(emulator, reference, base_dir)
        attempt_dir = base_dir / f"attempt-{attempt:02}"
        try:
            last, _, _, _ = run_single_attempt(emulator, attempt_dir, reference, case_name)
        except Exception as error:
            last = {"case": case_name, "pass": False, "reasons": [str(error)]}
            emulator.start_home()
            emulator.wait_top(HOME)
        if last["pass"]:
            last["attempts"] = attempt
            return last
        emulator.start_home()
        emulator.wait_top(HOME)
    last["attempts"] = max_attempts
    return last


def ensure_runtime_ready(emulator, output):
    if not emulator.check_runtime(output, seconds=2):
        return
    emulator.shell("am", "force-stop", HOME)
    emulator.start_home()
    emulator.wait_top(HOME)
    if missing := emulator.check_runtime(output, seconds=20):
        raise RuntimeError(f"HyperGlass runtime activation missing: {missing}")


def candidate_apk():
    apks = [p for p in (ROOT / "dist").glob("HyperGlass-LSPosed-*.apk") if p.is_file()]
    if not apks:
        raise RuntimeError(f"No built release APK found in {ROOT / 'dist'}")
    return max(apks, key=lambda p: p.stat().st_mtime)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default=SERIAL)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apk", type=Path, default=None)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    apk_path = args.apk or candidate_apk()
    emulator = HandoffEmulator(args.serial, PROFILE["display_size"])
    avd = emulator.run("emu", "avd", "name").decode().splitlines()[0]
    if avd != "Xiaomi_18_Pro":
        raise RuntimeError(f"Expected Xiaomi_18_Pro, observed {avd}")

    initial_scales = emulator.animation_scales()
    if set(initial_scales.values()) != {"1.0"}:
        raise RuntimeError(f"Expected animation scales 1.0, observed {initial_scales}")
    initial_wallpaper = emulator.wallpaper_hashes()
    installed_sha = emulator.module_hash()
    expected_sha = sha256(apk_path.read_bytes()).hexdigest()
    if installed_sha != expected_sha:
        raise RuntimeError(f"Installed APK {installed_sha} differs from {expected_sha}")

    emulator.start_home()
    emulator.wait_top(HOME)
    emulator.shell("wm", "dismiss-keyguard")
    ensure_runtime_ready(emulator, args.output)
    launcher_pid = emulator.launcher_pid()
    reference = baseline_glyph()

    base_wallpaper_path = args.output / "original-wallpaper_orig.png"
    base_wallpaper_path.write_bytes(
        emulator.run("exec-out", emulator.root("cat", "/data/system/users/0/wallpaper_orig")),
    )
    original_shot = emulator.screenshot()
    original_shot.save(args.output / "initial-home.png")
    rgb = np.asarray(original_shot)
    probes = rgb[[770, 770, 1000, 1000], [410, 530, 410, 530]]
    quadrants = np.array([[207, 57, 46], [54, 182, 85], [48, 107, 192], [216, 181, 58]])

    results = []
    try:
        wallpaper_ctx = (
            emulator.test_wallpaper(
                build_quadrants_wallpaper(args.output / "quadrants.png", PROFILE["display_size"]),
                args.output / "original-wallpaper-backup.png",
            )
            if np.max(np.abs(probes.astype(int) - quadrants)) > 30
            else nullcontext()
        )
        with ExitStack() as stack:
            stack.enter_context(wallpaper_ctx)
            for name in args.cases:
                case_dir = args.output / name
                if name.startswith("wallpaper-after-"):
                    if np.max(np.abs(probes.astype(int) - quadrants)) > 30:
                        wp_source = args.output / "quadrants.png"
                    else:
                        wp_source = base_wallpaper_path
                    res = run_wallpaper_case(emulator, case_dir, reference, wp_source, name)
                else:
                    res = retry_case(emulator, case_dir, reference, name)
                results.append(res)
                print(json.dumps({
                    "case": res["case"],
                    "pass": res["pass"],
                    "capture_size": res.get("capture_size"),
                    "window_max_luma": res.get("window_max_luma"),
                    "window_max_chroma": res.get("window_max_chroma"),
                    "start_pop_to_draw_ms": res.get("start_pop_to_draw_ms"),
                    "reasons": res.get("reasons", []),
                }, ensure_ascii=False), flush=True)
                if emulator.launcher_pid() != launcher_pid:
                    raise RuntimeError(f"Launcher PID changed during {name}")
    finally:
        if emulator.animation_scales() != initial_scales:
            with emulator.slowed_animations(initial_scales.values(), initial_scales):
                pass
        if emulator.wallpaper_hashes() != initial_wallpaper:
            if base_wallpaper_path.exists():
                with emulator.test_wallpaper(base_wallpaper_path, args.output / "restore-final-tmp.png"):
                    pass
        final_state = {
            "apk_sha256": emulator.module_hash(),
            "animation_scales": emulator.animation_scales(),
            "wallpaper_sha256": list(emulator.wallpaper_hashes()),
            "launcher_pid": emulator.launcher_pid(),
            "total": len(results),
            "passed": sum(1 for r in results if r["pass"]),
            "pass": len(results) == len(args.cases) and all(r["pass"] for r in results),
            "results": results,
        }
        write_json(args.output / "summary.json", final_state)
        assert final_state["animation_scales"] == initial_scales
        assert tuple(final_state["wallpaper_sha256"]) == tuple(initial_wallpaper)

    if not final_state["pass"]:
        failed = [r["case"] for r in results if not r["pass"]]
        raise SystemExit(f"Handoff regression failed: {failed}")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda s, _f: (_ for _ in ()).throw(KeyboardInterrupt(f"Signal {s}")))
    main()
