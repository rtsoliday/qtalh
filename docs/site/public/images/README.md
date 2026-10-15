# Screenshot provenance

The modern screenshots were regenerated on 2026-10-15 with macOS, Qt 5.15.19,
and the `offscreen` platform to restore the missing referenced assets.
`main-window.png` retains its 2026-10-14 Linux/Qt 5.15.9 capture.
They show repository-owned synthetic test data; no
production PVs were connected. The modern captures use the default appearance
and its smaller default font. Native window decorations, fonts, colors, and
display scale can vary by platform.

| Retained image | Source under the build's `test-artifacts/` directory |
| --- | --- |
| `main-window.png` | `classic-main.png` from `test_ui runtimeModels` with `QTALH_TEST_STYLE=motif` |
| `modern-main.png` | `modern-main.png` from `test_ui runtimeModels` (default appearance) |
| `modern-properties.png` | `dialogs/qt-editor-channel-properties.png` |
| `modern-force-pv.png` | `dialogs/qt-channel-force.png` |
| `modern-history.png` | `dialogs/qt-history.png` |
| `modern-runtime.png` | `dialogs/qt-runtime.png` |
| `modern-mask.png` | `dialogs/qt-mask.png` |

Regenerate the captures from the repository root:

```sh
make -C qtalh QT_VERSION=5 test-ui test-ui-motif test-gallery
```

Copy the files above from the selected object directory into this directory.
The main-window test uses the BOOSTER hierarchy; the dialog gallery uses
QTALH_REFERENCE with the synthetic Vacuum pressure channel. The history gallery
widens the dialog to show all columns.

Keep PNGs in version control. Markdown pages use `/images/...`; the site build
copies them to `docs/html/images/`. Run `make docs` after replacing screenshots
so the generated HTML and retained image assets stay in sync.
