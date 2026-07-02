# future/ — Braid bitmap typography work-in-progress

This folder holds the typography work until core is ready to receive it.
Everything here is self-contained and can be dropped into `core/` / `md/`
when the other instance finishes.

## Contents

| File | Purpose |
|---|---|
| `typography.md` | Design doc: API, font choice, implementation plan. |
| `bdf_to_braid_font.py` | Converter from BDF to a C++ embedded font header. |
| `cherry-13-r.bdf` | Source BDF for the default Cherry 13-r font. |
| `LICENSE-cherry-13-r` | Cherry's 0BSD license. |
| `braid_font_cherry13.h` | Generated C++ header with the 128×208 R8 atlas and glyph table. |

## Regenerating the font header

```bash
cd /path/to/braid
python3 future/bdf_to_braid_font.py future/cherry-13-r.bdf future/braid_font_cherry13.h --name Cherry13
```

The script will pick up `LICENSE-cherry-13-r` automatically.

## Verifying the atlas

You can render the generated atlas to a PGM file to inspect it:

```bash
python3 - <<'PY'
import pathlib
data = pathlib.Path('future/braid_font_cherry13.h').read_text()
# naive parse of the atlas array
start = data.index('kAtlas[')
start = data.index('{', start) + 1
end = data.index('};', start)
vals = [int(x, 16) for x in data[start:end].replace(',', ' ').split() if x.startswith('0x')]
w, h = 128, 208
with open('/tmp/cherry13_atlas.pgm', 'wb') as f:
    f.write(f'P5 {w} {h} 255\n'.encode())
    f.write(bytes(vals))
PY
open /tmp/cherry13_atlas.pgm
```

## Next steps (for when core is free)

1. Move/copy `future/typography.md` to `md/typography.md`.
2. Move `future/braid_font_cherry13.h` into `core/` (or a new `core/braid_text.h`).
3. Wire `SketchApp::text()` / `textSize()` into the batching pipeline as described in the design doc.
4. Keep `future/bdf_to_braid_font.py` as a tool for adding more built-in fonts later.
