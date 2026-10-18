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

There must be one root. Group names must not repeat an ancestor or use the reserved name `NULL`. See the [configuration reference](/reference/configuration) for parent lookup and nesting rules.

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

::: info Saving changes the textual representation
Save expands includes, canonicalizes masks, places channels before child groups, and discards comments and original formatting. Keep the original source configuration if those details matter to your editing workflow.
:::

Continue with [masks and automatic forcing](/configure/masks) or [logging setup](/configure/logging).
