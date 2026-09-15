---
title: Documentation
description: Install QtALH, monitor your first alarm, and find the configuration and command-line reference.
---

<div class="doc-eyebrow">EPICS ALARM HANDLER · QT 5 / QT 6</div>

# QtALH documentation

<p class="doc-intro">Monitor EPICS alarms, build a facility configuration, and understand what happens when an alarm changes.</p>

<div class="doc-paths">
  <a class="doc-path" href="./get-started/first-alarm.html"><span class="path-number">01 / LEARN</span><strong>Your first alarm →</strong><span>Run a local IOC and follow an alarm from normal to acknowledged.</span></a>
  <a class="doc-path" href="./operate/alarms.html"><span class="path-number">02 / OPERATE</span><strong>Work with alarms →</strong><span>Navigate groups, acknowledge alarms, silence audio, and read logs.</span></a>
  <a class="doc-path" href="./configure/editor.html"><span class="path-number">03 / CONFIGURE</span><strong>Build your configuration →</strong><span>Organize channels, edit properties, and set up masks and logging.</span></a>
  <a class="doc-path" href="./reference/command-line.html"><span class="path-number">04 / LOOK UP</span><strong>Find an option or directive →</strong><span>Exact syntax, defaults, platform support, and configuration rules.</span></a>
</div>

## Start with a configuration

After [building QtALH](./get-started/install), run these commands from the repository root:

```sh
bin/Linux-x86_64/qtalh --validate examples/minimal.alhConfig
bin/Linux-x86_64/qtalh -c examples/minimal.alhConfig
```

The example contains two placeholder PVs. [Open your first configuration](./get-started/first-run) explains how to replace them and start monitoring. Linux, macOS, and Windows builds are supported.

<div class="doc-note">
<strong>Choosing a mode?</strong> Local mode keeps acknowledgements in your runtime. Global mode uses IOC acknowledgement fields. Passive mode prevents CA writes. <a href="./understand/alarm-lifecycle.html">Compare the modes →</a>
</div>

## Choose your appearance

Keep the default Motif appearance, or start with `-style fusion` for modern
controls. [Appearance & font size](./operate/appearance) shows both looks and
explains the font-size shortcuts.

![QtALH main window with Fusion styling](/images/fusion-main.png)

## Working on QtALH

Read the [architecture](./develop/architecture), run the [test suites](./develop/testing), or inspect the [compatibility inventory](./understand/compatibility). Historical measurements and implementation investigations live in [Engineering history](./history/).
