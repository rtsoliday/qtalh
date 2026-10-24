# Create & edit a configuration

Build a hierarchy of groups and EPICS channels using the configuration editor. You need the PV names and the intended alarm policy for your facility.

## Start from the example

From the repository root:

```sh
bin/Linux-x86_64/qtalh -c examples/minimal.alhConfig
```

The editor uses the modern appearance by default; add `-style motif` for the classic single-column Properties form.

Use **Save As** to save your own file. To start an empty document instead, run `qtalh -c`.

## Add and name the hierarchy

1. Select a parent group.
2. Use the **Insert** menu to add a group or channel.
3. In Properties, set the channel's actual EPICS PV name. Use an alias for a readable display label.
4. Repeat for the remaining channels and subgroups.

**Insert → Group…** and **Insert → Channel…** place the new node right after
the selection. With a channel or subgroup selected in a group's contents, it
follows that node. With a group selected in the tree, it becomes the group's
first entry. The new node is then selected, so nodes inserted one after another
keep their order; a new group becomes the group shown, ready for its channels.
Pasted nodes and **Insert → Configuration File…** are still added at the end
of the destination group.

There must be one root, and group names must not use the reserved name `NULL`.
A group may repeat an ancestor's name, with parent names resolved to the nearest
matching ancestor. Editor changes and saves that would reload the hierarchy
differently are rejected with the group to rename. See the
[configuration reference](/reference/configuration) for parent lookup and nesting rules.

## Edit properties and guidance

Select a node and open Properties. **Apply** validates and applies the fields while leaving the dialog open. **Cancel** restores the currently applied values; **Close** (modern) or **Dismiss** (Motif mode) closes the window. Selection dialogs follow your selected node. In a runtime, **View → Properties Window** shows the same fields read-only, with only Close or Dismiss.

In the modern appearance, Properties organizes the fields into **General**, **Force PV**,
**Commands**, and **Guidance** tabs of aligned forms. General contains identity, alias,
masks, count filtering (*n alarms within s seconds*), beep severity (with completion), and PV settings. Apply, Cancel, and Close stay
below the tabs. When the selected node changes, the dialog preserves the selected
tab and scroll position; unfinished edits are protected from background refreshes.

![Modern channel Properties with the General tab selected](/images/modern-properties.png)

This screenshot uses the synthetic channel from the repository gallery.

Add operator guidance explaining what to check when the alarm occurs. Inline `$GUIDANCE` text appears inside QtALH; a configured URL or file opens externally. Related `$COMMAND` actions should use commands and paths appropriate to the runtime host.

Operators can hover over **G** to read guidance and **P** to preview related commands in either appearance.
Clicking **P** runs the related-process command in the configuration tool too, as in ALH; severity and status commands never run there because the editor connects to no PVs.

Undo uses **Alt+Backspace**, and redo uses **Ctrl+Y**. The editor also supports cut/copy/paste and copying between editor windows.

Copying a group or channel also carries the notification destinations its
`$NOTIFY` directives need. Paste reuses matching destination definitions and
adds missing ones to the destination configuration's root. It refuses a paste
if the same destination name has a different definition there. Clear and Cut
refuse to remove a destination still used elsewhere in the configuration.

Undo and redo share a limit of 100 configuration snapshots and a 32 MiB memory
budget. Older snapshots are discarded as needed. A configuration too large for
one snapshot clears the undo/redo history.

## Reports, printing, and inserting files

- **Insert → Configuration File…** reads another configuration and adds its
  root group, with everything below it, under the selected group (or the
  selected channel's group).
- **File → Report…** writes an indented text outline of the groups and
  channels, with each channel's mask, to an `.alhReport` file.
  **File → Print…** prints the same outline; the runtime's File menu also has
  **Print…**.
- **File → Close All** closes the open editor windows. Each window asks before
  discarding unsaved changes.

## Validate and activate

Save the file, then check it without connecting to any IOC:

```sh
bin/Linux-x86_64/qtalh --validate /path/to/facility.alhConfig
```

**Expected result:** a successful exit and the channel/group counts. This verifies syntax, includes, and supported expressions; it does not check PV availability or write permissions.

Use **File → Activate ALH** to open an independent runtime copy of the current edits. An unnamed document must first be saved to establish a filename. Runtime mask changes do not edit the source document; a later runtime reload reads the named disk configuration.

## Save comments and INCLUDE files

Save keeps the file as you wrote it: comments, blank lines, spacing and line
order stay, and each INCLUDE line stays an INCLUDE line. Only the lines you
changed are rewritten, in the standard form legacy ALH writes, which drops a
comment at the end of a changed line. A deleted group or channel takes the
comments directly above it. Directives you add follow the existing directives of
their group or channel; inserted groups and channels go where described above.
Directives with no effect are not kept: a value replaced by a later one of the
same kind, and later `$SEVRPV` or `$HEARTBEATPV` lines that ALH ignores. In an
INCLUDE file that other configurations also read, such a line, for example a
`$BEEPSEVERITY` that this configuration overrides, can take effect there; it is
removed when Save rewrites that file. If any file of the configuration is read
as ISO-8859-1, every file Save writes uses ISO-8859-1 where it can represent the
text.

Groups and channels you add or paste into a group read from an INCLUDE file,
or insert right after a node read from one,
belong to that file. When an edit changes an included file, **Save** and
**Save As** list every included file they will rewrite, in place at its own
path, and ask before writing them, because other
configurations that include a file read the change too. Included files you did
not change are never rewritten. Removing everything an included file declared
removes its INCLUDE line and leaves the file as it is.

**Save As** keeps the INCLUDE lines: they are read from the configuration
directory (`-f`, otherwise `ALARMHANDLER` or the working directory), not from the
saved file's directory. If they would name other files from the new file, Save
As instead saves one file that contains the included files, and the document
then refers to that file only. Pasted nodes and
**Insert → Configuration File…** add their text, including that of their
INCLUDE files, to the file of the destination group.

Before writing, QtALH reads the saved text back and refuses to save if it would
load as a different configuration; nothing is written then. Each file is first
written to a temporary copy, and no file is replaced unless every copy could be
written, so a read-only directory or a full disk leaves all files unchanged. If
replacing a file still fails, the message lists the INCLUDE files already saved.
This also applies when different INCLUDE paths reach the same file through a
symbolic link: their copies must agree before Save can write that file.

In a runtime, **View → Configuration File** shows the running configuration as
read-only text with search, with the comments and blank lines of its source
files. Each INCLUDE line is followed by its file, indented three spaces per
level as in ALH. Runtime changes such as **Beep Severity** and **ALH Beep
Severity**, and unsaved editor changes in a runtime activated from the editor,
appear in place. The view shows what was read when the configuration was loaded;
a later change to a file on disk appears after a reload.

Continue with [masks and automatic forcing](/configure/masks) or [logging setup](/configure/logging).
