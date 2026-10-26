"""macOS (finding 091): build/host64-macos/Downhill Revived.app from build/host64-macos/dhd64.

The bundle holds the program, its icon and a dhd.ini template; no game data. From a .app the
program keeps dhd.ini, saves/ and dhd.log in ~/Library/Application Support/Downhill Revived
(launcher.c app_folder), copying the template there on the first start. The game then asks for
the player's disc image once (host.c host_disc_ready, dialogs_apple.m) and fills in the player
defaults (host.c player_defaults) where the template doesn't say. The template asks for 3x: the
native renderer at 1344 lines fills a 1440-line monitor.

usage: python port/tools/package_mac.py   (after build_host64_macos.py)"""
import os
import plistlib
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'build', 'host64-macos')
APP = os.path.join(OUT, 'Downhill Revived.app')

TEMPLATE = """; Downhill Revived for Mac: the PC settings (the OPTIONS > PC SETTINGS page changes them too).
; The game adds the disc image and the other player defaults on the first start.
renderer = native
scale = 3
window = borderless
widescreen = 1
memory_card = 1
"""


def version():
    for line in open(os.path.join(ROOT, 'port', 'include', 'version.h'), encoding='utf-8'):
        if line.startswith('#define DHD_VERSION '):
            return line.split('"')[1]
    sys.exit('no DHD_VERSION in port/include/version.h')


def icon(resources):
    """AppIcon.icns from port/res/icon.png (512 px), through an iconset."""
    work = os.path.join(OUT, 'AppIcon.iconset')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    source = os.path.join(ROOT, 'port', 'res', 'icon.png')
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            pixels = min(size * scale, 512)
            name = 'icon_%dx%d%s.png' % (size, size, '@2x' if scale == 2 else '')
            subprocess.run(['sips', '-z', str(pixels), str(pixels), source, '--out', os.path.join(work, name)],
                           check=True, capture_output=True)
    subprocess.run(['iconutil', '-c', 'icns', work, '-o', os.path.join(resources, 'AppIcon.icns')], check=True)
    shutil.rmtree(work)


def main():
    exe = os.path.join(OUT, 'dhd64')
    if not os.path.exists(exe):
        sys.exit('no build/host64-macos/dhd64: run port/tools/build_host64_macos.py first')
    shutil.rmtree(APP, ignore_errors=True)
    macos, resources = os.path.join(APP, 'Contents', 'MacOS'), os.path.join(APP, 'Contents', 'Resources')
    os.makedirs(macos)
    os.makedirs(resources)
    shutil.copy2(exe, os.path.join(macos, 'dhd64'))
    icon(resources)
    with open(os.path.join(resources, 'dhd.ini'), 'w', newline='\n') as f:
        f.write(TEMPLATE)
    v = version()
    info = {
        'CFBundleName': 'Downhill Revived',
        'CFBundleDisplayName': 'Downhill Revived',
        'CFBundleIdentifier': 'local.downhill-revived',
        'CFBundleExecutable': 'dhd64',
        'CFBundleIconFile': 'AppIcon',
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': v,
        'CFBundleVersion': v,
        'LSMinimumSystemVersion': '13.0',
        'LSArchitecturePriority': ['arm64'],
        'LSApplicationCategoryType': 'public.app-category.racing-games',  # a game: macOS Game Mode
        'NSHighResolutionCapable': True,
        'GCSupportsControllerUserInteraction': True,
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
    }
    with open(os.path.join(APP, 'Contents', 'Info.plist'), 'wb') as f:
        plistlib.dump(info, f)
    # an ad-hoc signature over the whole bundle (Apple Silicon runs only signed code)
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', APP], check=True)
    print(APP)


if __name__ == '__main__':
    main()
