# Appearance & font size

QtALH keeps its Motif-influenced appearance by default. Choose a Qt Widgets style
at startup if you prefer modern controls. The same choice applies to the editor,
its dialogs, and runtime windows activated from that editor.

## Choose Fusion or keep Motif

Fusion is a portable choice for Qt 5.15 and Qt 6 on Linux, macOS, and Windows.
From the repository root, launch the runtime or editor with:

```sh
bin/Linux-x86_64/qtalh -style fusion -mainwindow examples/minimal.alhConfig
bin/Linux-x86_64/qtalh -c -style fusion examples/minimal.alhConfig
```

The runtime connects to the configured PVs; the editor does not start monitoring.
On macOS or Windows, substitute your platform's executable path.

`-style=Fusion` also works. Style names are case-insensitive; the last occurrence
wins. Other styles depend on the Qt installation. An unknown name reports the
available styles before connecting to PVs. Omit `-style`, or use `-style motif`,
to restore the original appearance. Environment style settings alone do not
select the redesigned interface.

### Fusion

![Fusion main window with the alarm tree, contents pane, Status section, and Alarm sound controls](/images/fusion-main.png)

The panes and alarm actions retain their behavior. Styled mode adds standard
menus and controls, a labeled pane-width slider, and an **Alarm legend** that
starts collapsed. Expand the legend to read the mask and alarm-count notation.
The **Alarm sound** rows share a right-aligned text edge in every style.

### Default Motif appearance

![Default Motif-influenced main window using the same synthetic alarm fixture](/images/main-window.png)

Both screenshots use repository test data rather than a live facility. Alarm
colors, severity letters, mask meaning, blinking, and acknowledgement behavior
are preserved across appearances. Typography and window decorations vary by
platform.

## Adjust the font while running

These shortcuts are available with any explicitly selected style other than Motif:

| Action | Linux / Windows | macOS |
| --- | --- | --- |
| Smaller by one point | Ctrl+− | Command+− |
| Larger by one point | Ctrl++ or Ctrl+= | Command++ or Command+= |
| Restore startup sizes | Ctrl+0 | Command+0 |

The default is one point smaller than the desktop's font size, with a minimum of
6 points. For example, an 11-point desktop UI font starts at 10 points in QtALH.
The main UI can be adjusted from 6 to 72 points. Alarm rows and their click targets
resize with the font, and open dialogs and new windows share the adjustment.
Log/configuration text keeps its fixed-width family. Bare `+` and `-` still expand
and collapse the alarm tree.

`-fn`, `-font`, and `ALHMAINFONT` still select only the compact runtime button's
font. An explicit font starts at its requested size; the shortcuts adjust it
relative to that size, and reset restores it.

Style and font adjustments last for this process only. No appearance preference
is saved. Colors come from the selected style and desktop palette; there is no
separate light/dark option.

## Dialogs and compact runtime

The compact window keeps its facility button and mask summary. Click it to open
the main window; it grows with its content and font size.

![Fusion compact runtime facility button](/images/fusion-runtime.png)

[Properties](/configure/editor#edit-properties-and-guidance) uses four tabs:
General, Force PV, Commands, and Guidance. Styled forms use **Close** for the
dismiss action. Apply and Cancel retain their existing meanings. File selection
uses Qt's styled file dialog; printing uses the platform's print dialog.

For guidance and command previews, see [G and P buttons](/operate/alarms#open-guidance-or-a-related-process).
