"""Resolver cases shared by the host and Android test runners.

The reference launcher must resolve every feature group. Older launchers are
reported for compatibility tracking only and never fail a run.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

H = Path(__file__).resolve().parent
REFS = Path(os.environ.get('HYPERGLASS_REFERENCE_DIR', H.parent / 'fixtures/reference'))
APK_LIB = '!/lib/arm64-v8a/libapp.so'
REFERENCE = REFS / 'launcher-801030308'
ENGINE = REFS / 'launcher-801027722/libhyper_os_flutter.so'
MAML = REFS / 'launcher-801027722/librust_maml_sdk.so'
FULL = 247
# Fixture mode -> feature groups it must disable on the reference launcher.
APP_FAILURES = {
    'core-layout': 2, 'scene-view-layout': 2, 'maml-repaint-layout': 2, 'sf-return-layout': 2,
    'material-scene-layout': 2, 'detached-capture-identity': 2, 'workspace-scroll-layout': 2,
    'widget-layout': 4 | 16 | 64 | 128, 'ambiguous': 2, 'badge-layout': 32, 'icon-layout': 1,
}
ENGINE_FAILURES = ('engine-layout', 'frame-info-layout', 'glass-cache-layout')
COMPATIBILITY = [
    ('9239', REFS / 'launcher-801029239/MiuiHome.apk', APK_LIB),
    ('9226', REFS / 'launcher-801029226/MiuiHome.apk', APK_LIB),
    ('7722', REFS / 'launcher-801027722/libapp.so', ''),
]


def run(*arguments, **options):
    return subprocess.run(list(map(str, arguments)), check=True, **options)


def required_cases(out):
    """Generate fixtures in `out`; return (name, file, image index, ready, suffix)."""
    app = REFERENCE / 'libapp.so'
    run(sys.executable, H / 'adaptive/make_future_fixture.py', app, out / 'future.so')
    for mode in APP_FAILURES:
        run(sys.executable, H / 'adaptive/make_failure_fixture.py', app, mode, out / (mode + '.so'))
    for mode in ENGINE_FAILURES:
        run(sys.executable, H / 'adaptive/make_failure_fixture.py', ENGINE, mode, out / (mode + '.so'))
    cases = [('reference', app, 0, FULL, ''), ('reference-apk', REFERENCE / 'MiuiHome.apk', 0, FULL, APK_LIB),
             ('engine', ENGINE, 1, 2, ''), ('maml', MAML, 2, 8, ''), ('future', out / 'future.so', 0, FULL, '')]
    cases += [(mode, out / (mode + '.so'), 0, FULL & ~groups, '') for mode, groups in APP_FAILURES.items()]
    cases += [(mode, out / (mode + '.so'), 1, 0, '') for mode in ENGINE_FAILURES]
    return cases


def check_plan(name, plan):
    if name in APP_FAILURES:
        assert all(not (group & APP_FAILURES[name]) for _, _, group in plan['patches']), name
    if name in ENGINE_FAILURES:
        assert plan['patches'] == [], name


def report_compatibility(resolve):
    """`resolve(file, suffix)` runs the resolver and returns its stdout."""
    for name, file, suffix in COMPATIBILITY:
        if not file.exists():
            print(f'compatibility {name}: fixture missing')
            continue
        ready = re.search(r'ready=(\d+)', resolve(file, suffix))
        print(f'compatibility {name}: ready={ready.group(1) if ready else "?"} (reference {FULL})')


def runtime_plan_arguments(plans):
    return [plans / 'reference.json', REFERENCE / 'libapp.so', plans / 'future.json', plans / 'future.so']
