# Environment variables

QtALH reads its own path/font settings and uses EPICS Base (Channel Access and pvAccess) and Qt for connection and display settings. Explicit command-line path/font options take precedence over the corresponding QtALH defaults.

| Variable | Purpose |
| --- | --- |
| `ALARMHANDLER` | Configuration directory and default log directory; defaults to `.`. All relative INCLUDE paths use this directory unless `-f` overrides it. |
| `ALHMAINFONT` | Runtime button font, overridden by `-fn` or `-font`. XLFD fonts are approximated. |
| `USE_SHELVING` | `1` shows [timed shelving](/operate/shelving): **Action → Shelve Alarms…** and **Unshelve Alarms**, **View → Shelved Alarms…** and the right-click **Shelve this channel…** and **Shelve this group…** commands. Any other value, or none, hides them. |
| `QTALH_PHOEBUS_MENU` | Any value other than empty or `0` shows **File → Import Phoebus XML…** and **Export Phoebus XML…** (see [Phoebus XML](/configure/phoebus)). `--to-phoebus` and `--from-phoebus` work without it. |
| `EPICS_CA_AUTO_ADDR_LIST` | EPICS Base automatic address discovery; set to `NO` in the isolated tutorial. |
| `EPICS_CA_ADDR_LIST` | EPICS Base CA server search addresses. |
| `EPICS_CA_SERVER_PORT` | CA server port used by the client/IOC. |
| `EPICS_CA_REPEATER_PORT` | CA repeater port. |
| `EPICS_CAS_INTF_ADDR_LIST` | IOC server interfaces; the tutorial uses loopback only. |
| `EPICS_CAS_BEACON_ADDR_LIST` | IOC beacon destinations. |
| `EPICS_PVA_ADDR_LIST` | pvAccess server search addresses for `pva://` channels. |
| `EPICS_PVA_AUTO_ADDR_LIST` | pvAccess automatic address discovery. |
| `EPICS_PVA_BROADCAST_PORT` | pvAccess search (UDP) port. |
| `DISPLAY` | X11 display, also selectable with `-display`. |
| `USER` | Sender name in the `FROM: User=` line of `-B` broadcasts (**Action → Send Message…**, **Stop Alarm Logging…**, and **Reload Facility…**). |
| `QT_QPA_PLATFORM` | Qt platform backend, such as `xcb` or `offscreen`. |
| `QT_PLUGIN_PATH` | Additional Qt plugin directories, including a split Multimedia SDK. |
| `COMSPEC` | Windows command shell; defaults to `cmd.exe`. |

Other standard `EPICS_CA_*` and `EPICS_PVA_*` variables are handled by EPICS Base. Names without the `pva://` prefix use Channel Access; see [pvAccess channels](/reference/configuration#pvaccess-channels). Build-time variables such as `EPICS_BASE`, `EPICS_HOST_ARCH`, `QT_VERSION`, and `QT_DIR` are covered in [Install & build](/get-started/install).

## Path resolution

| Input | Relative to |
| --- | --- |
| Configuration filename | `-f`, otherwise `ALARMHANDLER`, otherwise working directory. |
| INCLUDE filename | The same configured directory, including nested includes. |
| `-a` / `-o` log filename | `-l`, otherwise the configuration directory. |
| `-p` audio filename | Process working directory. |
| Explicit `-Lfile` basename | Process working directory. |
| Guidance filename | Document directory. |

Absolute paths bypass directory joining. Configuration filenames and `-a`/`-o`
log filenames beginning with `./` or `../` also resolve from the process working
directory, bypassing `-f`/`ALARMHANDLER` or `-l` respectively. Relative INCLUDE
filenames still use the configured directory. The C++ `loadConfig` API has a separate default: without an explicit configuration directory, includes resolve against the top-level file's directory.
