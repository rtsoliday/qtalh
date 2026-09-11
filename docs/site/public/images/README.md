# Screenshot provenance

These screenshots were refreshed on 2026-09-11 with Linux, Qt 5.15.3, and the
`offscreen` platform. They show repository-owned synthetic test data; no
production PVs were connected. The Fusion captures include the smaller default
font and the right-aligned Alarm sound captions. Native window decorations,
fonts, colors, and display scale can vary by platform.

| Retained image | Source under the build's `test-artifacts/` directory |
| --- | --- |
| `main-window.png` | `qtalh-main.png` from `test_ui runtimeModels` in Motif mode |
| `fusion-main.png` | `fusion-main.png` from `test_ui runtimeModels` in Fusion mode |
| `fusion-properties.png` | `dialogs-fusion/qt-editor-channel-properties.png` |
| `fusion-force-pv.png` | `dialogs-fusion/qt-channel-force.png` |
| `fusion-history.png` | `dialogs-fusion/qt-history.png` |
| `fusion-runtime.png` | `dialogs-fusion/qt-runtime.png` |

Regenerate the captures from the repository root:

```sh
make -C qtalh QT_VERSION=5 test-ui test-ui-fusion test-gallery
```

Copy the files above from the selected object directory into this directory.
The main-window test uses the BOOSTER hierarchy; the dialog gallery uses
QTALH_REFERENCE with the synthetic Vacuum pressure channel. The history gallery
widens the dialog to show all columns. The Force PV capture intentionally shows
the scrollable form with its action row visible.

Keep PNGs in version control. Markdown pages use `/images/...`; the site build
copies them to `docs/html/images/`. Run `make docs` after replacing screenshots
so the generated HTML and retained image assets stay in sync.
