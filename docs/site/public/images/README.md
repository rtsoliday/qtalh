# Screenshot provenance

`main-window.png` is the repository-owned `qtalh-main.png` fixture produced by
`qtalh/tests/test_ui.cc` on Linux/Qt 5. It shows synthetic test data, not a live
facility. Regenerate by running `make -C qtalh QT_VERSION=5 test-ui`, then copy
the image from the selected object's `test-artifacts/` directory.

`fusion-main.png` is the Fusion version of the same synthetic UI fixture.
`fusion-properties.png` is the editor Properties capture from the Qt-only
`test_visual dialogGallery` case. Both were generated on Linux with Qt 5.15.3;
no production PVs were connected. Regenerate with `test-ui-fusion test-gallery`,
then copy `fusion-main.png` and `dialogs-fusion/qt-editor-properties.png` from
`test-artifacts/` to these names. Native window decorations vary by platform.
