"""Capture and compare the launcher material scenes on a rooted emulator."""

import argparse
from contextlib import contextmanager, nullcontext
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
from uuid import uuid4

import numpy as np
from PIL import Image


HERE = Path(__file__).resolve().parent
HOME = "com.miui.home"
MODULE = "dev.hyperglass.lsposed"
DISPLAYS = r"Display (\d+) \(HWC display 0\)"
WORKSPACE_PAGE = r"\[LayoutInfo\].*currentPageIndex=(\d+)"
SCALES = ("window_animation_scale", "transition_animation_scale", "animator_duration_scale")
VIDEO_WIDTH, VIDEO_HEIGHT = 560, 1218
MOTION_WIDTH, MOTION_HEIGHT = 140, 304
FPS = 60
RECORD_SECONDS = 6
MOTION_ATTEMPTS = 5
SCENE_SETTLE_SECONDS = 3
OPEN_TAP_HOLD_MS = 120
STYLE_FEATURE = 1
SNAPSHOT_FEATURE = 2
PIXEL_CHANGE_THRESHOLD = 16
SOURCE_TIMING = ("-fps_mode", "passthrough", "-enc_time_base", "demux")
WALLPAPER_DIR = "/data/system/users/0"


class DeviceLostError(RuntimeError):
    pass


class Emulator:
    def __init__(self, serial, size):
        if not re.fullmatch(r"emulator-\d+", serial):
            raise ValueError("Visual capture requires an emulator serial")
        self.serial = serial
        self.adb = ["adb", "-s", serial]
        self.size = tuple(size)
        self.page_observation = None
        self.ensure_online()
        until = time.monotonic() + 60
        while self.run("shell", "getprop", "sys.boot_completed").strip() != b"1":
            if time.monotonic() >= until:
                raise RuntimeError(f"{serial} did not finish booting")
            time.sleep(1)
        if self.shell("id", "-u").strip() != "0":
            raise RuntimeError(f"{serial} requires su access")
        displays = self.shell("dumpsys", "SurfaceFlinger", "--display-id")
        primary = re.findall(DISPLAYS, displays)
        if len(primary) != 1:
            raise RuntimeError(f"Cannot identify display 0 on {serial}")
        self.display = primary[0]

    def run(self, *args, timeout=12):
        try:
            return subprocess.check_output(self.adb + list(args), timeout=timeout, stdin=subprocess.DEVNULL)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            self.ensure_online()
            raise

    def ensure_online(self):
        try:
            result = subprocess.run(self.adb + ["get-state"], stdin=subprocess.DEVNULL,
                                    capture_output=True, timeout=5)
        except subprocess.TimeoutExpired as error:
            raise DeviceLostError(f"device lost: {self.serial} did not answer adb") from error
        if result.returncode or result.stdout.strip() != b"device":
            raise DeviceLostError(f"device lost: {self.serial} is offline or absent")

    @staticmethod
    def root(*args):
        return shlex.join(("su", "-c", shlex.join(map(str, args))))

    def shell(self, *args, timeout=12):
        return self.run("shell", self.root(*args), timeout=timeout).decode()

    def screenshot(self):
        data = self.run("exec-out", self.root("screencap", "-p", "-d", self.display))
        image = Image.open(BytesIO(data)).convert("RGB")
        if image.size != self.size:
            raise RuntimeError(f"Expected display size {self.size}, got {image.size}")
        return image.resize((VIDEO_WIDTH, VIDEO_HEIGHT), Image.Resampling.BOX)

    def touch(self, *args):
        self.shell("input", "touchscreen", "-d", 0, *args)

    def start_home(self):
        self.shell("am", "start", "-W", "--display", 0, "-a", "android.intent.action.MAIN",
                   "-c", "android.intent.category.HOME", "-n", f"{HOME}/.launcher.Launcher")

    def wait_top(self, package, seconds=10):
        until = time.monotonic() + seconds
        actual = "none"
        while time.monotonic() < until:
            activities = self.shell("dumpsys", "activity", "activities")
            display = re.search(r"(?ms)^Display #0\b(.*?)(?=^Display #\d+\b|\Z)", activities)
            if display:
                resumed = re.search(r"topResumedActivity=[^\n]* ([\w.]+)/", display[1])
                actual = resumed[1] if resumed else "none"
                if actual == package:
                    return
            time.sleep(0.25)
        raise RuntimeError(f"scene precondition: expected {package} on display 0, observed {actual}")

    def launcher_pid(self):
        try:
            return self.shell("pidof", "-s", HOME).strip()
        except subprocess.CalledProcessError:
            return ""

    def wait_page(self, expected, seconds=6):
        until = time.monotonic() + seconds
        actual = None
        while time.monotonic() < until:
            pid = self.launcher_pid()
            if pid.isdecimal():
                logs = self.run("logcat", "-d", "--pid", pid, "-s", "flutter:I", "-e", WORKSPACE_PAGE).decode()
                pages = re.findall(WORKSPACE_PAGE, logs)
                if pages:
                    self.page_observation = pid, int(pages[-1]) + 1
                # The bounded log buffer can discard a settled page's last
                # event while the launcher remains on that page.
                if self.page_observation and self.page_observation[0] == pid:
                    actual = self.page_observation[1]
                    if actual == expected:
                        return
            time.sleep(0.25)
        raise RuntimeError(f"scene precondition: expected workspace page {expected}, observed {actual or 'unknown'}")

    def move_pages(self, count, swipe):
        for page in range(2, count + 2):
            time.sleep(0.7)
            self.touch("swipe", *swipe)
            self.wait_page(page)

    def check_runtime(self, output, seconds=30, *, snapshot_disabled=False):
        until = time.monotonic() + seconds
        logs = ""
        resolved = (
            r"RESOLVED image=libhyper_os_flutter\.so features=2\b",
            r"RESOLVED image=librust_maml_sdk\.so features=8\b",
        )
        material = (
            r"Native material capture ready: independent GPU backdrop, live foreground",
            r"Backdrop stream hooks ready: per-stream startup buffer filtering",
        )
        if snapshot_disabled:
            required = (*resolved, r"RESOLVED image=libapp\.so features=\d+\b")
        else:
            required = (*resolved, r"RESOLVED image=libapp\.so features=247\b",
                        r"APPLIED pid=\d+ features=255\b", *material)

        def missing_markers():
            missing = [line for line in required if not re.search(line, logs)]
            if snapshot_disabled:
                applied = re.findall(r"APPLIED pid=\d+ features=(\d+)\b", logs)
                features = int(applied[-1]) if applied else 0
                if (features & (STYLE_FEATURE | SNAPSHOT_FEATURE)) != STYLE_FEATURE:
                    missing.append("launcher style applied with the snapshot feature bit clear")
                if any(re.search(line, logs) for line in material):
                    missing.append("native material and backdrop hooks disabled")
            return missing

        while time.monotonic() < until:
            pid = self.launcher_pid()
            if pid.isdecimal():
                logs = self.run("logcat", "-d", "--pid", pid, "-s", "HyperGlass").decode()
                if not missing_markers():
                    break
            time.sleep(0.5)
        (output / "hyperglass.log").write_text(logs)
        return missing_markers()

    def animation_scales(self):
        return {key: self.shell("settings", "get", "global", key).strip() for key in SCALES}

    @contextmanager
    def test_wallpaper(self, fixture, backup):
        if not backup.exists():
            state = self.shell("dumpsys", "wallpaper")
            system, separator, lock = state.partition("System wallpaper state:")[2].partition("Lock wallpaper state:")
            lock = lock.partition("Fallback wallpaper state:")[0].strip()
            component = re.search(r"(?m)^mImageWallpaper=(\S+)$", state)
            if not (component and separator and f"mWallpaperComponent={component[1]}" in system
                    and "mWhich=3:" in system and lock in ("", "(null entry)")):
                raise RuntimeError("Scene requires a static wallpaper shared by home and lock screen")
            original = self.run("exec-out", self.root("cat", f"{WALLPAPER_DIR}/wallpaper_orig"))
            digest = sha256(original).hexdigest()
            with Image.open(BytesIO(original)) as image:
                if image.size != self.size or self.wallpaper_hashes() != (digest, digest):
                    raise RuntimeError(f"Scene requires an uncropped wallpaper sized {self.size}")
            temporary = backup.with_suffix(".tmp")
            temporary.write_bytes(original)
            temporary.replace(backup)

        with TemporaryDirectory(dir=backup.parent) as directory:
            setter = build_wallpaper_setter(Path(directory))
            run_id = sha256(str(backup.resolve()).encode()).hexdigest()[:20]
            remote = f"/data/local/tmp/hg-visual-wallpaper-{run_id}"
            self.shell("rm", "-rf", remote)
            self.run("shell", "mkdir", "-m", "755", remote)
            try:
                self.run("push", str(setter), f"{remote}/setter.jar")
                self.shell("chmod", "444", f"{remote}/setter.jar")
                try:
                    self.set_wallpaper(fixture, remote)
                    yield
                finally:
                    self.set_wallpaper(backup, remote)
                    backup.unlink()
            finally:
                self.shell("rm", "-rf", remote)

    def wallpaper_hashes(self):
        paths = (f"{WALLPAPER_DIR}/wallpaper_orig", f"{WALLPAPER_DIR}/wallpaper")
        return tuple(line.split()[0] for line in self.shell("sha256sum", *paths).splitlines())

    def set_wallpaper(self, image, remote):
        expected = (sha256(image.read_bytes()).hexdigest(),) * 2
        if self.wallpaper_hashes() != expected:
            self.run("push", str(image), f"{remote}/image.png")
            self.shell("chmod", "644", f"{remote}/image.png")
            self.shell("su", "1000", "-c",
                       f"CLASSPATH={remote}/setter.jar app_process /system/bin SetWallpaper {remote}/image.png")
        until = time.monotonic() + 10
        while time.monotonic() < until:
            if self.wallpaper_hashes() == expected:
                # HyperOS renders through its own ImageWallpaper service.
                self.shell("am", "startservice", "--user", "0", "-a", "android.service.wallpaper.WallPaperControllerService",
                           "-p", "com.miui.miwallpaper", "--es", "command", "update_wallpaper",
                           "--es", "calling_package", "android", "--ei", "which_wallpaper", "3")
                time.sleep(1)  # Let the wallpaper surface deliver the new buffer to the launcher.
                return
            time.sleep(0.25)
        raise RuntimeError(f"Wallpaper readback differs from {image.name}")

    def module_hash(self):
        package_paths = self.shell("pm", "path", MODULE).splitlines()
        if not package_paths or not package_paths[0].startswith("package:"):
            raise RuntimeError(f"Cannot find the installed {MODULE} APK")
        apk = package_paths[0].removeprefix("package:")
        return self.shell("sha256sum", apk).split()[0]

    @contextmanager
    def slowed_animations(self, scales, saved):
        try:
            for key, scale in zip(SCALES, scales, strict=True):
                self.shell("settings", "put", "global", key, scale)
            yield
        finally:
            for key, value in saved.items():
                if value == "null":
                    self.shell("settings", "delete", "global", key)
                else:
                    self.shell("settings", "put", "global", key, value)

    def record(self, video, action, lead_seconds=1.2):
        remote = f"/data/local/tmp/hg-visual-{uuid4().hex}.mp4"
        command = self.root(
            "screenrecord", "--display-id", self.display, "--size", f"{VIDEO_WIDTH}x{VIDEO_HEIGHT}",
            "--bit-rate", 16000000, "--time-limit", RECORD_SECONDS, remote,
        )
        recorder = subprocess.Popen(self.adb + ["shell", command], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            time.sleep(lead_seconds)
            action()
            output, _ = recorder.communicate(timeout=RECORD_SECONDS + 5)
            if recorder.returncode:
                self.ensure_online()
                raise RuntimeError(f"screenrecord failed: {output.decode(errors='replace').strip()}")
            video.write_bytes(self.run("exec-out", self.root("cat", remote), timeout=18))
        finally:
            if recorder.poll() is None:
                recorder.kill()
                recorder.wait(timeout=5)
            self.shell("rm", "-f", remote)


def decode(video, *args):
    return subprocess.check_output(
        ["ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-i", str(video), *args],
        timeout=30,
    )


def build_wallpaper_setter(directory):
    sys.path.insert(0, str(HERE.parents[1]))
    from tools.build_env import ANDROID_JAR, BUILD_TOOLS

    setter = directory / "setter.jar"
    subprocess.run(["javac", "--release", "17", "-classpath", str(ANDROID_JAR), "-d", str(directory),
                    str(HERE / "SetWallpaper.java")], timeout=30, check=True)
    subprocess.run([str(BUILD_TOOLS / "d8"), "--min-api", "35", "--lib", str(ANDROID_JAR),
                    "--output", str(setter), str(directory / "SetWallpaper.class")], timeout=30, check=True)
    return setter


def motion(video, returning, timing_filter, output_timing):
    raw = decode(video, "-vf", f"{timing_filter}scale={MOTION_WIDTH}:{MOTION_HEIGHT}:flags=area",
                 *output_timing, "-an", "-pix_fmt", "rgb24", "-f", "rawvideo", "-")
    frames = np.frombuffer(raw, dtype=np.uint8).reshape(-1, MOTION_HEIGHT, MOTION_WIDTH, 3)
    if len(frames) < FPS:
        raise RuntimeError("Recording is too short to measure motion")
    # Exclude the clock, notifications and navigation bar.
    comparison = frames[:, 20:-18]
    start = np.median(comparison[:8], axis=0).astype(np.int16)
    end = np.median(comparison[-8:], axis=0).astype(np.int16)
    home, app = (end, start) if returning else (start, end)
    changed = np.max(np.abs(app - home), axis=2) >= 32
    if np.mean(changed) < 0.15:
        raise RuntimeError("Start and end of recording have no distinct app surface")
    moving = comparison[:, changed].astype(np.int16)
    closer_to_app = np.max(np.abs(moving - app[changed]), axis=2) < np.max(np.abs(moving - home[changed]), axis=2)
    return frames, np.mean(closer_to_app, axis=1), home


def transition(video, returning, spec, reference):
    # Keep source frame indices on the short launch so resampling cannot
    # discard the only recorded pose that matches its baseline.
    timing_filter = f"fps={FPS}," if returning else ""
    output_timing = () if returning else SOURCE_TIMING
    frames, phases, home = motion(video, returning, timing_filter, output_timing)
    target_phase = spec["phase"]
    phase_distance = np.abs(phases - target_phase)
    distance = phase_distance
    settling_range = spec.get("home_difference")
    if settling_range:
        # Near landing the app surface can be gone before the launcher
        # unblurs and the flying layer reaches its final position.
        delta = np.abs(frames[:, 20:-18].astype(np.int16) - home)
        home_difference = np.mean(np.any(delta > PIXEL_CHANGE_THRESHOLD, axis=3), axis=(1, 2))
        lower, upper = settling_range
        in_flight = (home_difference >= lower) & (home_difference <= upper)
        distance = np.abs(home_difference - (lower + upper) / 2)
    else:
        in_flight = phases > 0.004
    if probe := spec.get("foreground"):
        x0, y0, x1, y1 = motion_box(probe["box"])
        pixels = frames[:, y0:y1, x0:x1]
        fraction = np.mean(np.min(pixels, axis=3) >= probe["min_rgb"], axis=(1, 2))
        lower, upper = probe["coverage"]
        if not settling_range:
            settled = np.median(fraction[-8:] if returning else fraction[:8])
            # A small app surface may still be visible in the probe after its
            # full-screen phase falls below the motion floor.
            in_flight |= fraction > settled + 0.05
        in_flight &= (fraction >= lower) & (fraction <= upper)
    candidates = np.flatnonzero(in_flight & (phases < 0.90) & (phase_distance <= 0.08))
    if not len(candidates):
        raise RuntimeError(f"Recording contains no in-flight frame near phase {target_phase:.3f}")
    if reference is None:
        rank = distance[candidates]
    else:
        # Locate the corresponding pose among the in-flight frames, then
        # compare the selected full-resolution crop against the baseline.
        x0, y0, x1, y1 = motion_box(spec["box"])
        images = frames[candidates, y0:y1, x0:x1].astype(np.int16)
        expected = np.asarray(reference.resize((x1 - x0, y1 - y0), Image.Resampling.BOX), dtype=np.int16)
        rank = np.mean(np.abs(images - expected), axis=(1, 2, 3))
    frame = int(candidates[np.argmin(rank)])
    png = decode(video, "-vf", f"{timing_filter}select=eq(n\\,{frame})", *output_timing,
                 "-frames:v", "1", "-c:v", "png", "-f", "image2pipe", "-")
    details = {"frame": frame, "phase": round(float(phases[frame]), 4)}
    if settling_range:
        details["home_difference"] = round(float(home_difference[frame]), 4)
    return Image.open(BytesIO(png)).convert("RGB"), details


def stripe_harmonics(images, period):
    luma = images @ np.array([0.2126, 0.7152, 0.0722])
    # The column median rejects foreground marks. Detrending rejects the
    # card tint, and combining phase components allows the edge to move.
    profile = np.median(luma, axis=-2)
    x = np.linspace(-1, 1, profile.shape[-1])
    angle = np.arange(len(x)) * (2 * np.pi / period)
    design = np.column_stack((np.ones(len(x)), x, x * x, np.cos(angle), np.sin(angle),
                              np.cos(2 * angle), np.sin(2 * angle)))
    coefficients = profile @ np.linalg.pinv(design).T
    return np.hypot(coefficients[..., 3], coefficients[..., 4]), np.hypot(coefficients[..., 5], coefficients[..., 6])


def decode_crops(video, frames, box):
    x0, y0, x1, y1 = box
    width, height = x1 - x0, y1 - y0
    select = "+".join(f"eq(n\\,{frame})" for frame in frames)
    raw = decode(video, "-vf", f"select={select},format=rgb24,crop={width}:{height}:{x0}:{y0}",
                 *SOURCE_TIMING, "-an", "-pix_fmt", "rgb24", "-f", "rawvideo", "-")
    return np.frombuffer(raw, dtype=np.uint8).reshape(len(frames), height, width, 3)


def check_blur(video, returning, spec, settled, output, name):
    _, phases, _ = motion(video, returning, "", SOURCE_TIMING)
    low, high = spec["phase_range"]
    frames = np.flatnonzero((phases >= low) & (phases <= high))
    limits = spec["limits"]
    if len(frames) < limits["min_frames"]:
        raise RuntimeError(f"Found {len(frames)} source frames in phase range [{low}, {high}], "
                           f"need {limits['min_frames']}")

    samples = decode_crops(video, frames, spec["box"])
    # The stripe wallpaper is nearly neutral. During the app crossfade,
    # a frame can match the global phase even though the app covers this crop.
    chroma = np.median(np.ptp(samples, axis=-1), axis=(1, 2))
    visible = chroma <= spec["max_median_chroma"]
    frames, samples = frames[visible], samples[visible]
    if len(frames) < limits["min_frames"]:
        raise RuntimeError(f"Only {len(frames)} unobscured frames in phase range [{low}, {high}], "
                           f"need {limits['min_frames']}")
    reference = settled.crop(spec["box"])
    fundamental, harmonic = stripe_harmonics(np.asarray(reference), spec["period"])
    if fundamental < limits["min_settled_fundamental"] or harmonic < limits["min_settled_harmonic"]:
        raise RuntimeError(f"Settled crop lacks the wallpaper pattern ({fundamental:.2f}, {harmonic:.2f})")
    settled_ratio = harmonic / fundamental
    fundamental, harmonic = stripe_harmonics(samples, spec["period"])
    ratios = harmonic / np.maximum(fundamental, 1e-6)
    deltas = np.abs(ratios / settled_ratio - 1)
    worst = int(np.argmax(deltas))
    Image.fromarray(samples[worst]).save(output / f"{name}.png")
    reference.save(output / f"{name}-settled.png")
    return {
        "pass": bool(deltas[worst] <= limits["max_relative_delta"]),
        "metric": spec["metric"],
        "settled_ratio": round(float(settled_ratio), 3),
        "in_flight_median_ratio": round(float(np.median(ratios)), 3),
        "max_relative_delta": round(float(deltas[worst]), 3),
        "max_relative_delta_limit": limits["max_relative_delta"],
        "worst_frame": int(frames[worst]),
        "frames": [{"frame": int(frame), "phase": round(float(phases[frame]), 4), "ratio": round(float(ratio), 3)}
                   for frame, ratio in zip(frames, ratios, strict=True)],
    }


def centered_disk(height, width, radius):
    y, x = np.ogrid[:height, :width]
    return (x - width / 2) ** 2 + (y - height / 2) ** 2 <= radius ** 2


def icon_luma(images, radius):
    height, width = images.shape[-3:-1]
    disk = centered_disk(height, width, radius)
    luma = images[..., disk, :] @ np.array([0.2126, 0.7152, 0.0722])
    dark, median, light = np.quantile(luma, [0.03, 0.5, 0.97], axis=-1)
    return median, (light - median) - (median - dark)


def settling_difference(images, home, box):
    # Select the landing pose using the rest of the desktop. The tested
    # icon cannot admit or exclude a frame by changing its own appearance.
    moved = np.any(np.abs(images[:, 20:-18].astype(np.int16) - home) > PIXEL_CHANGE_THRESHOLD, axis=3)
    x0, y0, x1, y1 = motion_box(box)
    moved[:, max(0, y0 - 20):max(0, y1 - 20), x0:x1] = False
    return np.mean(moved, axis=(1, 2))


def check_icon_polarity(video, returning, spec, settled, output, name):
    images, phases, home = motion(video, returning, "", SOURCE_TIMING)
    difference = settling_difference(images, home, spec["box"])
    lower, upper = spec["home_difference"]
    frames = np.flatnonzero((phases <= spec["max_app_phase"]) & (difference >= lower) & (difference <= upper))
    limits = spec["limits"]
    if len(frames) < limits["min_frames"]:
        raise RuntimeError(f"Only {len(frames)} source frames near icon landing, need {limits['min_frames']}")

    reference = settled.crop(spec["box"])
    settled_luma, settled_polarity = icon_luma(np.asarray(reference), spec["radius"])
    if settled_polarity < limits["min_polarity"]:
        raise RuntimeError(f"Settled icon lacks light marks on a dark face (polarity {settled_polarity:.1f})")
    crops = decode_crops(video, frames, spec["box"])
    lumas, polarities = icon_luma(crops, spec["radius"])
    deltas = np.abs(lumas - settled_luma)
    worst = int(np.argmax(deltas))
    Image.fromarray(crops[worst]).save(output / f"{name}.png")
    reference.save(output / f"{name}-settled.png")
    return {
        "pass": bool(deltas[worst] <= limits["max_luma_delta"] and np.min(polarities) >= limits["min_polarity"]),
        "metric": spec["metric"],
        "settled_luma": round(float(settled_luma), 2),
        "settled_polarity": round(float(settled_polarity), 2),
        "in_flight_median_luma": round(float(np.median(lumas)), 2),
        "min_in_flight_polarity": round(float(np.min(polarities)), 2),
        "max_luma_delta": round(float(deltas[worst]), 2),
        "max_luma_delta_limit": limits["max_luma_delta"],
        "min_polarity_limit": limits["min_polarity"],
        "worst_luma_frame": int(frames[worst]),
        "frames": [
            {"frame": int(frame), "phase": round(float(phases[frame]), 4),
             "home_difference": round(float(difference[frame]), 4),
             "luma": round(float(luma), 2), "polarity": round(float(polarity), 2)}
            for frame, luma, polarity in zip(frames, lumas, polarities, strict=True)
        ],
    }


def foreground_bounds(mask):
    y, x = np.nonzero(mask)
    if not x.size:
        return None
    return np.quantile(np.column_stack((x, y)), [0.01, 0.99], axis=0)


def within_one_pixel(mask):
    padding = ((0, 0),) * (mask.ndim - 2) + ((1, 1), (1, 1))
    neighborhoods = np.lib.stride_tricks.sliding_window_view(np.pad(mask, padding), (3, 3), axis=(-2, -1))
    return neighborhoods.any(axis=(-2, -1))


def check_icon_landing(video, returning, spec, settled, output, name):
    images, phases, home = motion(video, returning, "", SOURCE_TIMING)
    box, limits = spec["box"], spec["limits"]
    difference = settling_difference(images, home, box)
    late = (phases <= spec["max_app_phase"]) & (difference <= spec["late_difference"])
    not_late = np.flatnonzero(~late)
    if not not_late.size:
        raise RuntimeError("Recording never leaves the landing window")
    first = int(not_late[-1]) + 1
    stationary = np.flatnonzero(difference[first:] <= spec["settled_difference"])
    if not stationary.size:
        raise RuntimeError("Recording never reaches the settled panel")
    panel_frame = first + int(stationary[0])
    end = panel_frame + spec["follow_frames"]
    if panel_frame - first < limits["min_frames"] or end + 8 > len(images):
        raise RuntimeError("Recording lacks source frames on both sides of icon landing")

    frames = np.arange(panel_frame - limits["min_frames"], end)
    recorded = decode_crops(video, [*frames, *range(len(images) - 8, len(images))], box)
    tail = np.median(recorded[len(frames):], axis=0).astype(np.uint8)
    _, mean, _ = pixel_diff(Image.fromarray(tail), settled.crop(box))
    if mean > 8:
        raise RuntimeError(f"Settled recording does not match the panel screenshot (mean RGB {mean:.2f})")
    # Compare to the first settled icon in the same landing window. The
    # live panel can refresh its backdrop again later in the recording.
    crops = recorded[:len(frames)]
    reference_count = limits["min_frames"]
    reference = np.median(crops[-reference_count:], axis=0).astype(np.uint8)

    probe = spec["glyph"]
    left, top, right, bottom = probe["box"]
    glyphs = np.min(crops[:, top:bottom, left:right], axis=-1) >= probe["min_rgb"]
    glyph = np.min(reference[top:bottom, left:right], axis=-1) >= probe["min_rgb"]
    if radius := probe.get("exclude_center_radius"):
        # The hands can legitimately move between snapshot and live drawing.
        # Match the fixed tick marks to within one recorded pixel.
        perimeter = ~centered_disk(*glyph.shape, radius)
        glyphs &= perimeter
        glyph &= perimeter
        matches = np.count_nonzero(glyphs & within_one_pixel(glyph), axis=(1, 2))
        matches += np.count_nonzero(within_one_pixel(glyphs) & glyph, axis=(1, 2))
    else:
        matches = 2 * np.count_nonzero(glyphs & glyph, axis=(1, 2))
    if (rest_bounds := foreground_bounds(glyph)) is None:
        raise RuntimeError("No icon foreground at the settled target")
    overlap = matches / (np.count_nonzero(glyphs, axis=(1, 2)) + np.count_nonzero(glyph))
    centers, sizes = [], []
    for mask in glyphs:
        bounds = foreground_bounds(mask)
        delta = None if bounds is None else bounds - rest_bounds
        centers.append(None if delta is None else round(float(np.max(np.abs(np.mean(delta, axis=0)))), 2))
        sizes.append(None if delta is None else round(float(np.max(np.abs(delta[1] - delta[0]))), 2))

    plate = np.stack([
        np.median(crops[:, top:bottom, left:right], axis=(1, 2))
        for left, top, right, bottom in spec["plate"]
    ], axis=1)
    delta = plate - np.median(plate[-reference_count:], axis=0)
    luma = delta @ np.array([0.2126, 0.7152, 0.0722])
    chroma = np.stack((delta[..., 0] - luma, delta[..., 2] - luma), axis=-1)
    luma_delta = np.max(np.abs(luma), axis=1)
    chroma_delta = np.max(np.linalg.norm(chroma, axis=-1), axis=1)
    max_center = max((x for x in centers if x is not None), default=None)
    max_size = max((x for x in sizes if x is not None), default=None)
    # Record the largest adjacent source-frame change in any plate sample.
    steps = np.maximum(
        np.max(np.abs(np.diff(luma, axis=0)), axis=1) / limits["max_luma_delta"],
        np.max(np.linalg.norm(np.diff(chroma, axis=0), axis=-1), axis=1) / limits["max_chroma_delta"],
    )
    step = int(np.argmax(steps))
    Image.fromarray(crops[step]).save(output / f"{name}-step-before.png")
    Image.fromarray(crops[step + 1]).save(output / f"{name}-step-after.png")
    Image.fromarray(reference).save(output / f"{name}-settled.png")
    result = {
        "pass": bool(np.max(luma_delta) <= limits["max_luma_delta"]
                     and np.max(chroma_delta) <= limits["max_chroma_delta"]
                     and max_center is not None and max_center <= limits["max_center_delta"]
                     and max_size is not None and max_size <= limits["max_size_delta"]
                     and np.min(overlap) >= limits["min_glyph_overlap"]),
        "metric": spec["metric"],
        "panel_settled_frame": panel_frame,
        "landing_reference_frames": [int(frame) for frame in frames[-reference_count:]],
        "largest_plate_step": [int(frames[step]), int(frames[step + 1])],
        "max_luma_delta": round(float(np.max(luma_delta)), 2),
        "max_chroma_delta": round(float(np.max(chroma_delta)), 2),
        "max_center_delta": max_center,
        "max_size_delta": max_size,
        "min_glyph_overlap": round(float(np.min(overlap)), 3),
        "frames": [
            {"frame": int(frame), "home_difference": round(float(difference[frame]), 4),
             "luma_delta": round(float(luma_delta[i]), 2), "chroma_delta": round(float(chroma_delta[i]), 2),
             "center_delta": centers[i], "size_delta": sizes[i], "glyph_overlap": round(float(overlap[i]), 3)}
            for i, frame in enumerate(frames)
        ],
    }
    if polarity := spec.get("polarity"):
        result["polarity"] = check_icon_polarity(video, returning, polarity, settled, output, f"{name}-polarity")
        result["pass"] = result["pass"] and result["polarity"]["pass"]
    return result


def motion_box(box):
    left, top, right, bottom = box
    return (round(left * MOTION_WIDTH / VIDEO_WIDTH), round(top * MOTION_HEIGHT / VIDEO_HEIGHT),
            round(right * MOTION_WIDTH / VIDEO_WIDTH), round(bottom * MOTION_HEIGHT / VIDEO_HEIGHT))


def crop(image, box, ignore=()):
    result = image.crop(box)
    for rect in ignore:
        result.paste((0, 0, 0), rect)
    return result


def pixel_diff(current, reference):
    difference = np.abs(np.asarray(current, dtype=np.int16) - np.asarray(reference, dtype=np.int16))
    mean = float(np.mean(difference))
    changed = float(np.mean(np.max(difference, axis=2) > PIXEL_CHANGE_THRESHOLD) * 100)
    return difference, mean, changed


def within_limits(mean, changed, limits):
    return mean <= limits["max_mean"] and changed <= limits["max_changed_pct"]


def capture_scene(emulator, profile, output, work, name, scene, baseline):
    results = {}

    def prepare(capture_entry=False):
        emulator.start_home()
        emulator.shell("input", "-d", 0, "keyevent", "KEYCODE_HOME")
        emulator.wait_top(HOME)
        emulator.wait_page(1)
        emulator.move_pages(scene["page"] - 1, profile["next_page"])
        time.sleep(2)
        emulator.wait_page(scene["page"])
        if capture_entry and scene.get("before_enter"):
            image = emulator.screenshot()
            for label, spec in scene["before_enter"].items():
                save(f"{name}-{label}", image, spec)
        for touch in scene.get("enter", ()):
            emulator.touch(*touch)
            time.sleep(SCENE_SETTLE_SECONDS)

    def save(key, image, spec, **details):
        path = output / f"{key}.png"
        crop(image, spec["box"], spec.get("ignore", ())).save(path, optimize=True)
        print(f"CAPTURE {key}", details, flush=True)
        results[key] = details

    if scene.get("warmup"):
        prepare()
        emulator.touch("tap", *scene["tap"])
        emulator.wait_top(scene["package"])
        time.sleep(SCENE_SETTLE_SECONDS)
        emulator.touch("swipe", *profile["home_gesture"])
        emulator.wait_top(HOME)
        time.sleep(SCENE_SETTLE_SECONDS)

    prepare(capture_entry=True)
    if "package" not in scene:
        widgets = emulator.shell("dumpsys", "appwidget").partition("\nWidgets:")[2].partition("\nHosts:")[0]
        if scene["provider"] not in widgets:
            raise RuntimeError(f"scene precondition: AppWidget {scene['provider']} is not bound")
        save(name, emulator.screenshot(), scene)
        return results
    rest = emulator.screenshot()
    rest_spec = scene.get("rest")
    if rest_spec:
        save(f"{name}-rest", rest, rest_spec)
    for label, spec in scene.get("details", {}).items():
        save(f"{name}-{label}", rest, spec)
    errors = {}
    best = {}
    accepted = set()
    transitions = (("open", False), ("return", True))
    capture_count = sum(action in scene for action, _ in transitions)
    for attempt in range(1, MOTION_ATTEMPTS + 1):
        if attempt > 1:
            prepare()
            if "metric" in scene.get("open", {}):
                rest = emulator.screenshot()
        for action, returning in transitions:
            key = f"{name}-{action}"
            foreground = HOME if returning else scene["package"]
            touch = ("swipe", *profile["home_gesture"]) if returning else ("tap", *scene["tap"])
            spec = scene.get(action)
            if spec is None or key in accepted:
                emulator.touch(*touch)
                emulator.wait_top(foreground)
                time.sleep(2.5)
                continue
            clip = work / f"{key}-{attempt}.mp4"
            # Shift the gesture relative to the recorder after a missed pose.
            lead_seconds = 1.2 + (attempt - 1) * 0.045
            if not returning:
                # The pressed state gives the recorder motion before the
                # release starts the first launch frame.
                x, y = scene["tap"]
                hold_ms = OPEN_TAP_HOLD_MS + (attempt - 1) * spec.get("tap_hold_step_ms", 0)
                touch = ("swipe", x, y, x, y, hold_ms)
            emulator.record(clip, lambda: emulator.touch(*touch), lead_seconds)
            emulator.wait_top(foreground)
            if "metric" in spec:
                metric = {
                    "stripe_harmonic_ratio": check_blur,
                    "icon_luma_polarity": check_icon_polarity,
                    "icon_landing": check_icon_landing,
                }.get(spec["metric"])
                if metric is None:
                    raise ValueError(f"Unknown motion metric: {spec['metric']}")
                settled = rest
                if returning:
                    emulator.wait_page(scene["page"])
                    settled = emulator.screenshot()
                try:
                    result = metric(clip, returning, spec, settled, output, key)
                except RuntimeError as error:
                    errors[key] = str(error)
                    continue
                result["attempt"] = attempt
                results[key] = result
                accepted.add(key)  # Retry a missed phase, but keep a measured regression.
                errors.pop(key, None)
                summary = {field: value for field, value in result.items() if field not in ("frames", "polarity")}
                print("PASS" if result["pass"] else "FAIL", key, summary, flush=True)
                continue
            reference = None
            if baseline:
                with Image.open(baseline / f"{key}.png") as stored:
                    reference = stored.convert("RGB")
            try:
                image, details = transition(clip, returning, spec, reference)
                score = 0
                if reference is not None:
                    _, mean, changed = pixel_diff(crop(image, spec["box"], spec.get("ignore", ())), reference)
                    limits = spec["limits"]
                    score = max(mean / limits["max_mean"], changed / limits["max_changed_pct"])
                if key not in best or score < best[key]:
                    best[key] = score
                    save(key, image, spec, attempt=attempt, **details)
                if score <= 1:
                    accepted.add(key)
                errors.pop(key, None)
            except RuntimeError as error:
                if key not in best:
                    errors[key] = str(error)
        if len(accepted) == capture_count:
            break

    if errors:
        raise RuntimeError("\n".join(f"{key}: {error}" for key, error in errors.items()))
    if rest_spec is None:
        return results
    emulator.wait_page(scene["page"])
    returned = emulator.screenshot()
    spec = rest_spec
    _, mean, changed = pixel_diff(
        crop(returned, spec["box"], spec.get("ignore", ())), crop(rest, spec["box"], spec.get("ignore", ())),
    )
    if mean > 25 or changed > 60:
        raise RuntimeError(
            f"scene precondition: return did not settle on the initial layout "
            f"(mean_rgb={mean:.3f}, changed_pct={changed:.3f})"
        )
    save(f"{name}-returned", returned, spec)
    return results


def compare(name, output, baseline, limits):
    reference = Image.open(baseline / f"{name}.png").convert("RGB")
    current = Image.open(output / f"{name}.png").convert("RGB")
    if reference.size != current.size:
        return {"pass": False, "reason": f"Crop dimensions changed from {reference.size} to {current.size}"}
    difference, mean, changed = pixel_diff(current, reference)
    Image.fromarray(np.minimum(difference * 4, 255).astype(np.uint8)).save(output / f"{name}-diff.png")
    return {
        "pass": within_limits(mean, changed, limits),
        "mean_rgb": round(mean, 3),
        "changed_pct": round(changed, 3),
        **limits,
    }


def write_report(output, report):
    temporary = output / ".report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    temporary.replace(output / "report.json")


def publish_baseline(baseline, output, report):
    baseline.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=baseline.parent) as directory:
        staging = Path(directory)
        images = {name: result for name, result in report["scenes"].items() if "metric" not in result}
        for name in images:
            path = output / f"{name}.png"
            (staging / path.name).write_bytes(path.read_bytes())
        manifest = {"scenes": images, "issues": report["issues"], "launcher": report["launcher"]}
        (staging / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n")
        staging.rename(baseline)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="emulator-5580")
    parser.add_argument("--profile", type=Path, default=HERE / "scenes.json")
    parser.add_argument("--baseline", type=Path, default=HERE / "baseline")
    parser.add_argument("--output", type=Path, required=True, help="Directory for current images, logs and report")
    parser.add_argument("--record", action="store_true", help="Record a new baseline after all runtime and scene checks pass")
    parser.add_argument("--expect-snapshot-disabled", action="store_true",
                        help="Verify the snapshot group is disabled before replaying a negative control")
    parser.add_argument("--keep-recordings", action="store_true", help="Save transition clips in the output directory")
    parser.add_argument("--resume", action="store_true", help="Resume an incomplete run in the same output directory")
    args = parser.parse_args()
    if not re.fullmatch(r"emulator-\d+", args.serial):
        parser.error("Visual capture requires an emulator serial")
    if args.record and args.expect_snapshot_disabled:
        parser.error("A negative control can only replay an existing baseline")
    if args.record and args.baseline.exists():
        parser.error(f"Baseline already exists: {args.baseline}")

    profile_data = args.profile.read_bytes()
    profile = json.loads(profile_data)
    manifest = args.baseline / "capture.json"
    baseline_data = manifest.read_bytes() if not args.record and manifest.is_file() else b""
    run = {
        "serial": args.serial,
        "record": args.record,
        "keep_recordings": args.keep_recordings,
        "profile_sha256": sha256(profile_data).hexdigest(),
        "baseline": str(args.baseline.resolve()),
        "baseline_manifest_sha256": sha256(baseline_data).hexdigest() if baseline_data else None,
    }
    wallpapers = {scene["wallpaper"] for scene in profile["scenes"].values() if "wallpaper" in scene}
    if wallpapers:
        run["wallpaper_sha256"] = {name: sha256((HERE / name).read_bytes()).hexdigest() for name in sorted(wallpapers)}
    if args.expect_snapshot_disabled:
        run["expect_snapshot_disabled"] = True
    if args.resume:
        try:
            report = json.loads((args.output / "report.json").read_text())
        except (OSError, ValueError) as error:
            parser.error(f"Cannot resume capture: {error}")
        if report.get("run") != run or report.get("status") == "complete":
            parser.error("--resume requires an incomplete run with the same serial, profile, baseline and flags")
    else:
        if args.output.exists():
            parser.error(f"Output directory already exists: {args.output}")
        args.output.mkdir(parents=True)
        report = {"run": run, "completed_scenes": [], "scenes": {}}

    pending = [name for name in profile["scenes"] if name not in report["completed_scenes"]]
    report.update(status="in_progress", next_scene=next(iter(pending), None), issues=[])
    write_report(args.output, report)
    try:
        emulator = Emulator(args.serial, profile["display_size"])
        package = emulator.shell("dumpsys", "package", HOME)
        version = re.search(r"versionName=(\S+)", package)
        if not version:
            raise RuntimeError(f"Cannot find the installed {HOME} version")
        launcher = version[1]
        module = emulator.module_hash()
        if report.get("launcher", launcher) != launcher or report.get("module_apk_sha256", module) != module:
            raise RuntimeError("Resume requires the same launcher and HyperGlass APK")
        report.update(launcher=launcher, module_apk_sha256=module)
        if baseline_data:
            expected = json.loads(baseline_data).get("launcher")
            if expected and expected != launcher:
                raise RuntimeError(f"Baseline launcher is {expected}, installed launcher is {launcher}")

        emulator.start_home()
        emulator.wait_top(HOME, seconds=30)
        missing = emulator.check_runtime(args.output, snapshot_disabled=args.expect_snapshot_disabled)
        if missing:
            raise RuntimeError("\n".join(f"Runtime missing {line}" for line in missing))
        if "original_animation_scales" not in report:
            report["original_animation_scales"] = emulator.animation_scales()
            write_report(args.output, report)

        if args.keep_recordings:
            path = args.output / "recordings"
            path.mkdir(exist_ok=True)
            recordings = nullcontext(path)
        else:
            recordings = TemporaryDirectory(dir=args.output)
        with recordings as work, emulator.slowed_animations(profile["animation_scales"], report["original_animation_scales"]):
            baseline = None if args.record else args.baseline
            for index, name in enumerate(pending):
                print(f"SCENE {name}", flush=True)
                scene = profile["scenes"][name]
                wallpaper = scene.get("wallpaper")
                background = (emulator.test_wallpaper(HERE / wallpaper, args.output / f"{name}-original-wallpaper.png")
                              if wallpaper else nullcontext())
                with background:
                    results = capture_scene(emulator, profile, args.output, Path(work), name, scene, baseline)
                if baseline:
                    for key, result in results.items():
                        if "metric" in result:
                            continue
                        label = key.removeprefix(f"{name}-")
                        spec = (scene.get("before_enter", {}).get(label) or scene.get("details", {}).get(label)
                                or scene.get(label, scene.get("rest", scene)))
                        result.update(compare(key, args.output, baseline, spec["limits"]))
                        print("PASS" if result["pass"] else "FAIL", key, result, flush=True)
                report["scenes"].update(results)
                report["completed_scenes"].append(name)
                report["next_scene"] = pending[index + 1] if index + 1 < len(pending) else None
                write_report(args.output, report)

        report["status"] = "complete"
        if args.record and all(result.get("pass", True) for result in report["scenes"].values()):
            publish_baseline(args.baseline, args.output, report)
    except DeviceLostError as error:
        report["status"] = "device_lost"
        target = report["next_scene"] or "finalization"
        report["issues"].append(f"{error}. Restart the AVD and use --resume to continue at {target}")
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["issues"].append("Capture interrupted. Use --resume to continue")
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        report["status"] = "incomplete"
        target = report["next_scene"]
        report["issues"].append(f"{target}: {error}" if target else str(error))
    finally:
        write_report(args.output, report)
    for issue in report["issues"]:
        print(f"FAIL {issue}", flush=True)
    return int(bool(report["issues"]) or any(not scene.get("pass", args.record) for scene in report["scenes"].values()))


def terminate(_signum, _frame):
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, terminate)
    raise SystemExit(main())
