# Your first alarm

Run a small IOC on your own computer, raise an alarm, and acknowledge it in QtALH. You will see the difference between a current alarm and an alarm that still needs acknowledgement.

**Before you begin:** [build QtALH](/get-started/install) and EPICS Base. You need a desktop session and the Base tools `softIoc` and `caput`. The walkthrough below uses a Linux/macOS shell, two terminals, and the repository's <a href="../downloads/examples/tutorial.db" download>tutorial database</a> and <a href="../downloads/examples/tutorial.alhConfig" download>configuration</a>.

## 1. Set up a local connection

Open two terminals at the repository root. Run the following in **both** terminals. Replace the Base path and host architecture to match your installation; on macOS, for example, the host architecture may be `darwin-aarch64`.

```sh
export PATH=/path/to/base/bin/linux-x86_64:$PATH
export EPICS_CA_AUTO_ADDR_LIST=NO
export EPICS_CA_ADDR_LIST=127.0.0.1:5068
export EPICS_CA_SERVER_PORT=5068
export EPICS_CA_REPEATER_PORT=5069
export EPICS_CAS_INTF_ADDR_LIST=127.0.0.1
export EPICS_CAS_BEACON_ADDR_LIST=127.0.0.1:5069
```

These settings keep this exercise on loopback and use a separate pair of CA ports. If those ports are already in use, choose another unused pair and update both terminals consistently. The variables apply to the current terminals only.

## 2. Start the IOC

In the first terminal:

```sh
softIoc -d examples/tutorial.db
```

**Expected result:** the IOC starts and presents an `epics>` prompt. Leave it running. The pressure record starts at zero, with no active alarm.

## 3. Open QtALH

In the second terminal:

```sh
bin/Linux-x86_64/qtalh --validate examples/tutorial.alhConfig
bin/Linux-x86_64/qtalh -D -s -mainwindow examples/tutorial.alhConfig &
```

On Apple Silicon, substitute `bin/Darwin-arm64/qtalh`. Validation should report **1 channel and 2 groups**. In the main window, select **Vacuum** and find **Vacuum pressure**. After connecting, it should be normal.

This exercise uses default local acknowledgement, disables file logging (`-D`), and silences audio (`-s`). It leaves acknowledgement enabled. If the row stays ERROR, check that the IOC is running and both terminals have the same CA settings.

To use modern controls and layouts, add `-style fusion` to the launch command.
The default remains the familiar Motif appearance; `-style motif` selects it
explicitly. Both appearances provide the same alarm actions.

![QtALH with Fusion styling](/images/fusion-main.png)

## 4. Raise and clear the alarm

Run each command in the second terminal and observe the row before continuing:

```sh
caput qtalh_demo:pressure 7
```

The channel enters **MINOR**, with a yellow alarm indicator. Its parent group reflects the alarm.

```sh
caput qtalh_demo:pressure 12
```

The channel enters **MAJOR**, with a red alarm indicator.

```sh
caput qtalh_demo:pressure 0
```

The active severity returns to **NO_ALARM**. With the default mask, the highest unacknowledged transient remains until you acknowledge it.

## 5. Acknowledge the channel

Select **Vacuum pressure**, then use **Action → Acknowledge Alarm** or **Ctrl+A**. The outstanding acknowledgement clears. Acknowledgement records that an operator has seen an alarm; it does not change the pressure PV or correct the underlying condition.

Hover over the channel's **G** button to preview its guidance without opening a dialog. Open the guidance using **G** or **Ctrl+G**. The instructions are stored in the configuration's `$GUIDANCE` block.

## 6. Finish the exercise

Close the compact QtALH window and confirm exit. In the IOC terminal, enter `exit` at the `epics>` prompt. Close the two terminals to discard their temporary environment settings.

Next, [create your own configuration](/configure/editor) or [compare local, global, and passive modes](/understand/alarm-lifecycle).
