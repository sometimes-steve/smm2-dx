#!/usr/bin/env python3
"""Controller button icons for the menu (runtime/ui/pad_icons.c): picks the icons the menu uses from the three icon packs
by Zacksly (CC BY 3.0, https://zacksly.itch.io: Switch, Xbox Series and PS5 "Button Icons and Controls", "Buttons Full Solid",
white) and writes recomp/runtime/ui/icons/pad/<family>_<name>.svg plus recomp/runtime/ui/pad_icons.inc.

The packs' SVGs colour their shapes with CSS classes (<style>.cls-1{fill:#fff;}</style>), which the menu's SVG renderer
(nanosvg) ignores, so each class is written into its shapes as a style attribute. Nothing else is changed.

    python recomp/tools/gen_pad_icons.py IMAGES_DIR

IMAGES_DIR holds "Switch Button Icons and Controls" (a folder) and "Xbox Series Button Icons and Controls.zip" /
"PS5 Button Icons and Controls.zip" (or the folders they unpack to).
"""
import re
import sys
import zipfile
from pathlib import Path

RECOMP = Path(__file__).resolve().parent.parent
OUT = RECOMP / 'runtime' / 'ui' / 'icons' / 'pad'
INC = RECOMP / 'runtime' / 'ui' / 'pad_icons.inc'

# SDL's gamepad buttons, as runtime/input_bind.h's PADB values (runtime/hle/bindings.c k_pad_ini order)
HOSTS = ["south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
         "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
         "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger"]

# family: (pack, source file stem for each icon key, host button -> icon key, menu label -> icon key)
FAMILIES = [
    ('nin', 'Switch Button Icons and Controls', {
        'A': 'A', 'B': 'B', 'X': 'X', 'Y': 'Y', 'L': 'L', 'R': 'R', 'ZL': 'ZL', 'ZR': 'ZR', 'Plus': 'Plus', 'Minus': 'Minus',
        'Home': 'Home', 'Capture': 'Capture', 'LStick': 'Left Stick Click', 'RStick': 'Right Stick Click',
        'DUp': 'Pro D-Pad Up', 'DDown': 'Pro D-Pad Down', 'DLeft': 'Pro D-Pad Left', 'DRight': 'Pro D-Pad Right'},
     {'south': 'B', 'east': 'A', 'west': 'Y', 'north': 'X', 'back': 'Minus', 'guide': 'Home', 'start': 'Plus',
      'lstick': 'LStick', 'rstick': 'RStick', 'lshoulder': 'L', 'rshoulder': 'R', 'dpad_up': 'DUp', 'dpad_down': 'DDown',
      'dpad_left': 'DLeft', 'dpad_right': 'DRight', 'misc1': 'Capture', 'ltrigger': 'ZL', 'rtrigger': 'ZR'},
     {'A': 'A', 'B': 'B', 'X': 'X', 'Y': 'Y', 'L': 'L', 'R': 'R', 'ZL': 'ZL', 'ZR': 'ZR', '+': 'Plus', '-': 'Minus'}),
    ('xb', 'Xbox Series Button Icons and Controls', {
        'A': 'A', 'B': 'B', 'X': 'X', 'Y': 'Y', 'LB': 'Left Bumper', 'RB': 'Right Bumper', 'LT': 'Left Trigger',
        'RT': 'Right Trigger', 'Menu': 'Menu', 'View': 'View', 'Home': 'Home', 'Share': 'Share',
        'LStick': 'Left Stick Click', 'RStick': 'Right Stick Click',
        'DUp': 'D-Pad Up', 'DDown': 'D-Pad Down', 'DLeft': 'D-Pad Left', 'DRight': 'D-Pad Right'},
     {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y', 'back': 'View', 'guide': 'Home', 'start': 'Menu',
      'lstick': 'LStick', 'rstick': 'RStick', 'lshoulder': 'LB', 'rshoulder': 'RB', 'dpad_up': 'DUp', 'dpad_down': 'DDown',
      'dpad_left': 'DLeft', 'dpad_right': 'DRight', 'misc1': 'Share', 'ltrigger': 'LT', 'rtrigger': 'RT'},
     {'A': 'A', 'B': 'B', 'X': 'X', 'Y': 'Y', 'LB': 'LB', 'RB': 'RB', 'LT': 'LT', 'RT': 'RT', 'Menu': 'Menu', 'View': 'View'}),
    ('ps', 'PS5 Button Icons and Controls', {
        'Cross': 'Cross', 'Circle': 'Circle', 'Square': 'Square', 'Triangle': 'Triangle', 'L1': 'L1', 'R1': 'R1',
        'L2': 'L2', 'R2': 'R2', 'Options': 'Options', 'Create': 'Create', 'Home': 'Home', 'Touchpad': 'Touch Pad Press',
        'LStick': 'Left Stick Click', 'RStick': 'Right Stick Click',
        'DUp': 'D-Pad Up', 'DDown': 'D-Pad Down', 'DLeft': 'D-Pad Left', 'DRight': 'D-Pad Right'},
     {'south': 'Cross', 'east': 'Circle', 'west': 'Square', 'north': 'Triangle', 'back': 'Create', 'guide': 'Home',
      'start': 'Options', 'lstick': 'LStick', 'rstick': 'RStick', 'lshoulder': 'L1', 'rshoulder': 'R1', 'dpad_up': 'DUp',
      'dpad_down': 'DDown', 'dpad_left': 'DLeft', 'dpad_right': 'DRight', 'touchpad': 'Touchpad', 'ltrigger': 'L2',
      'rtrigger': 'R2'},
     {'Cross': 'Cross', 'Circle': 'Circle', 'Square': 'Square', 'Triangle': 'Triangle', 'L1': 'L1', 'R1': 'R1', 'L2': 'L2',
      'R2': 'R2', 'Options': 'Options', 'Create': 'Create'}),
]
SUB = 'Buttons Full Solid/White/SVG'


def inline_styles(svg: str) -> str:
    """The <style> block's .cls-N rules written into the shapes that use them (nanosvg has no CSS)."""
    rules = {}
    for sels, body in re.findall(r'((?:\.cls-\d+,?)+)\{([^}]*)\}', svg):  # (".cls-1,.cls-2{...}": several classes at once)
        for c in re.findall(r'cls-\d+', sels):
            rules[c] = rules.get(c, '') + body
    svg = re.sub(r'<defs>\s*<style>.*?</style>\s*</defs>', '', svg, flags=re.S)
    svg = re.sub(r'class="(cls-\d+)"', lambda m: 'style="%s"' % rules.get(m.group(1), ''), svg)
    svg = re.sub(r'\s(id|data-name)="[^"]*"', '', svg)
    return svg.strip() + '\n'


def read_pack(images: Path, pack: str):
    d = images / pack
    if d.is_dir():
        base = d / SUB
        return lambda stem: (base / (stem + '.svg')).read_text(encoding='utf-8')
    z = zipfile.ZipFile(images / (pack + '.zip'))
    return lambda stem: z.read('%s/%s/%s.svg' % (pack, SUB, stem)).decode('utf-8')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    images = Path(sys.argv[1])
    OUT.mkdir(parents=True, exist_ok=True)
    inc = ['// Generated by recomp/tools/gen_pad_icons.py: do not edit.',
           '// Button icons by Zacksly (CC BY 3.0, https://zacksly.itch.io), see ui/icons/pad/CREDITS.txt.', '']
    for fam, pack, icons, hosts, labels in FAMILIES:
        read = read_pack(images, pack)
        for key, stem in icons.items():
            svg = inline_styles(read(stem))
            (OUT / ('%s_%s.svg' % (fam, key))).write_text(svg, encoding='utf-8', newline='\n')
            inc += ['static const char svg_%s_%s[] = {' % (fam, key), '#embed "icons/pad/%s_%s.svg"' % (fam, key), '    , 0};']
        inc.append('static const PadSvg k_%s_icons[] = {' % fam)
        inc += ['    {"%s", svg_%s_%s},' % (k, fam, k) for k in icons]
        inc.append('};')
        inc.append('static const char *const k_%s_hosts[%d] = {' % (fam, len(HOSTS)))
        inc += ['    [%d] = "%s",  // %s' % (i, hosts[h], h) for i, h in enumerate(HOSTS) if h in hosts]
        inc.append('};')
        inc.append('static const PadAlias k_%s_labels[] = {' % fam)
        inc += ['    {"%s", "%s"},' % (a, k) for a, k in labels.items()]
        inc += ['};', '']
    INC.write_text('\n'.join(inc), encoding='utf-8', newline='\n')
    (OUT / 'CREDITS.txt').write_text(
        'Button icons by Zacksly (CC BY 3.0, https://creativecommons.org/licenses/by/3.0/, https://zacksly.itch.io):\n'
        '  "Switch Button Icons and Controls", "Xbox Series Button Icons and Controls", "PS5 Button Icons and Controls"\n'
        '  ("Buttons Full Solid", white).\n'
        'Changed from the originals: only the ones the menu uses are kept, and the CSS classes in each SVG (which the\n'
        "menu's SVG renderer does not read) are written into the shapes as style attributes. The shapes are unchanged.\n"
        'Generated by recomp/tools/gen_pad_icons.py.\n', encoding='utf-8', newline='\n')
    print('wrote %s and %s' % (OUT, INC))


if __name__ == '__main__':
    main()
