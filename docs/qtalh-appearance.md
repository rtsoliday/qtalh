# Motif appearance comparison

The Qt presentation was compared on the local X display using
`/usr/local/iocapps/opsys/asdops/alh/Rad_Mon.alhConfig` with these commands:

```sh
./bin/Linux-x86_64/alh -m 0 \
  -p /usr/share/sounds/freedesktop/stereo/bell.oga \
  -l /home/oxygen/SOLIDAY/github/alh/junklogs \
  /usr/local/iocapps/opsys/asdops/alh/Rad_Mon.alhConfig

./bin/Linux-x86_64/qtalh -m 0 \
  -p /usr/share/sounds/freedesktop/stereo/bell.oga \
  -l /home/oxygen/SOLIDAY/github/alh/qtjunklogs \
  /usr/local/iocapps/opsys/asdops/alh/Rad_Mon.alhConfig
```

The compact runtime window now matches Motif's 220×35 default size, single
facility-name button, aggregated mask suffix and blue-gray normal background.
It no longer adds a severity text line or separate Silence/Exit buttons.
Silence remains available in the main window. Closing the compact window or
choosing File Exit/Close asks “Exit Alarm Handler?” with OK and Cancel. Closing
only the main runtime display continues to hide it without stopping monitoring.

The main window retains the 1000×600 default and now opens only the first tree
level. Rows use 24-pixel spacing, with branch lines before acknowledgement
controls, natural-width names, and consecutive optional arrow/G/P controls.
Names are not elided into fixed columns. Motif-style bevels replace rounded row
buttons. The pane divider, width slider, menu height and footer placement follow
the reference display. Button labels are shifted down two pixels to center them
in their bevels, and the four silence/beep lines occupy equally spaced rows. Normal rows omit alarm summaries; disabled and cancelled
channels omit active severity indicators. Timed NoAck is shown as H in the mask.

Rendering and hit testing share the same row geometry. The existing item model
continues to own selection data and alarm updates. Single-clicking a group name
in the right pane selects it; double-clicking opens its contents. The arrow
expands or collapses that group's branch in the left pane.

Regression coverage in `tests/test_ui.cc` checks runtime size and labels, long
names, row hit targets, default expansion, leaf-group arrows, channel selection,
normal annotations and pane resizing. `make -C qtalh test-ui test-visual` passes;
the visual test also compares alarm logs against Motif using an isolated IOC.
Valgrind reports no memory errors or lost allocations in the checked runtime,
row-interaction, dialog and repeated-window tests.
The production configuration was used for viewing and navigation only; alarm
transitions for automated testing remain confined to the test IOC.

Remaining appearance differences include platform font rasterization and window-manager decorations. The subsequent auxiliary-window pass is described below. Printing still uses the Qt system print dialog.

Alarm files now use QMediaPlayer instead of QSoundEffect, allowing the supplied
Ogg/Vorbis `bell.oga` to decode. Qt 5 uses its installed GStreamer media backend;
Qt 6 uses QMediaPlayer with QAudioOutput. The UI test decodes and plays the system
bell twice with output muted (skipped if the freedesktop sound theme is absent).
The radiation-monitor launch produces no sound-decoding error. Audible output
through the operator's speakers still needs a listening check. See the
[Qt audio overview](https://doc.qt.io/qt-6.10/audiooverview.html) for media-format
support and backend requirements.


## Auxiliary-window comparison (2026-09-10)

The auxiliary windows were compared with the running Motif ALH on a private
Xvfb display, using a temporary configuration and no production PV connections.
The common Qt style now uses square bevels, Motif-like checkbox and diamond
radio indicators, compact control heights, blue-gray read-only fields, and
monospace dialog labels. Action rows follow ALH's order and use Dismiss for
modeless windows. File selection uses ALH's Filter, Directories, Files, and
Selection layout with OK, Filter, Cancel, and Help controls.

The following windows received layout changes:

- Group/channel Properties, including configuration editing: one vertically
  arranged form with force PV/CALC fields, masks, count filtering, commands,
  and guidance. Qt-specific editable ACKPV and heartbeat fields remain available.
- Modify Mask Settings: separate Add/Cancel, Enable/Disable, Ack/NoAck,
  AckT/NoAckT, and Log/NoLog action rows, each with Reset.
- Force Mask: mask summary, checkbox panel, and Apply/Reset/Dismiss/Help.
- Force Process Variable: narrow stacked form with mask toggles, force/reset
  values, and CALC A–F inputs. Cancel restores the current applied settings;
  Dismiss closes the form.
- Beep Severity: compact MINOR/MAJOR/INVALID/ERROR radio selection.
- Current Alarm History: compact gray text window with column headings.
- Guidance, configuration/log viewers, and broadcast/reload/stop-logging
  message entry: compact text layouts and ALH-style action rows.
- Open, Save As, report, and log-file selection: shared Motif-style file chooser.

The main and compact runtime windows retain their earlier layout work. Setup
filter and beep choices now appear as submenus like ALH. Generic confirmation,
About, and insert-name prompts share the same palette, fonts, and control style.
The print dialog retains Qt's printer controls; its structure is platform
specific. Help topics still open the external browser. Pixel-identical rendering
is not expected across font libraries, Qt versions, and window managers.

`test_visual dialogGallery` captures both runtime and editor windows plus their
auxiliary dialogs in `qtalh/O.<OS>-<ARCH>-qt<version>/test-artifacts/dialogs/`.
The full `test-visual` target also runs the loopback-only IOC comparison and
writes the Motif/Qt main-window captures and alarm transition trace.
The gallery is an inspection artifact, not a pixel-diff assertion. UI regression
checks exercise file filtering/selection/cancellation, property editing and
undo, group mask operations, and Force PV Apply/Cancel behavior.


The subsequent workflow pass adds the From/To/With historical log browsers,
Apply/Cancel/Dismiss editor Properties controls, and the Disabled forcePVs
footer count. Open selection dialogs retain their window geometry while following
selection changes. Their captures are included in `test_visual dialogGallery`.
