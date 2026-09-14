#!/usr/bin/env python3
"""Compare ALH/QtALH executables with a private loopback IOC (Linux).

CSV goes to stdout. --output retains logs, identities, and validation evidence.
Use --xvfb for a private X11 display and separate X-server CPU measurements.
100% CPU means one core; startup, the IOC, and monitor processes are excluded.
"""
import argparse
from contextlib import nullcontext
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

MODES = ('idle', 'updates', 'logged-updates')
FIELDS = ('mode', 'trial', 'version', 'channels', 'wall_seconds', 'cpu_seconds',
          'cpu_percent', 'xvfb_cpu_seconds', 'xvfb_cpu_percent')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def cpu_seconds(process):
    require(process.poll() is None, f'Process {process.pid} exited during the benchmark')
    fields = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def witness_values(path):
    values = []
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2:
            try:
                values.append(float(fields[1]))
            except ValueError:
                pass  # camonitor's connection messages are not values.
    return values


def check_log(path, prefix, channels):
    records = path.read_text().splitlines()
    states = [set() for _ in range(channels)]
    pattern = re.compile(re.escape(prefix) + r'pv(\d+)\b')
    for record in records:
        match = pattern.search(record)
        if match and int(match[1]) < channels:
            for severity in ('MAJOR', 'NO_ALARM'):
                if severity in record:
                    states[int(match[1])].add(severity)
    missing = [i for i, observed in enumerate(states) if observed != {'MAJOR', 'NO_ALARM'}]
    require(not missing, f'Log lacks both states for channels {missing[:20]}')
    return {'log_channels_both_states': channels, 'log_lines': len(records)}


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--labels', nargs=2, default=['before', 'after'], metavar=('BEFORE', 'AFTER'))
    parser.add_argument('--epics-bin', type=Path, default=Path('/usr/local/oag/base/bin/linux-x86_64'))
    parser.add_argument('--channels', type=int, default=1000)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--logged-seconds', type=int, help='Override duration for logged updates')
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--max-records', type=int, default=2000,
                        help='Circular-log capacity; at least twice channels for validation')
    parser.add_argument('--modes', nargs='+', choices=MODES, default=MODES)
    parser.add_argument('--output', type=Path, help='New directory for retained run evidence')
    parser.add_argument('--xvfb', action='store_true', help='Start a private 1600x1000x24 display')
    parser.add_argument('--cpus', nargs=4, type=int, metavar=('APP', 'IOC', 'XVFB', 'MONITOR'),
                        help='Pin processes to four distinct physical cores; omit to inherit affinity')
    args = parser.parse_args()
    if min(args.channels, args.seconds, args.trials, args.max_records) < 1:
        parser.error('counts and durations must be positive')
    if args.logged_seconds is not None and args.logged_seconds < 1:
        parser.error('--logged-seconds must be positive')
    if 'logged-updates' in args.modes and args.max_records < 2 * args.channels:
        parser.error('--max-records must be at least twice --channels to verify both logged states')
    if len(set(args.labels)) != 2 or any(not re.fullmatch(r'[A-Za-z0-9_-]+', s) for s in args.labels):
        parser.error('--labels must be distinct names containing only letters, digits, hyphens, or underscores')
    if len(set(args.modes)) != len(args.modes):
        parser.error('--modes must not contain duplicates')
    if sys.platform != 'linux':
        parser.error('this benchmark requires Linux /proc and process affinity support')
    args.before, args.after, args.epics_bin = args.before.resolve(), args.after.resolve(), args.epics_bin.resolve()
    for path in [args.before, args.after] + [args.epics_bin / name for name in ('softIoc', 'caRepeater', 'caget', 'caput', 'camonitor')]:
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f'Executable not found: {path}')
    commands = ['stdbuf'] + (['Xvfb', 'xwininfo'] if args.xvfb else []) + (['taskset'] if args.cpus else [])
    for command in commands:
        if not shutil.which(command):
            parser.error(f'Required command not found: {command}')
    if args.cpus:
        allowed = os.sched_getaffinity(0)
        if any(cpu not in allowed for cpu in args.cpus):
            parser.error('--cpus contains a CPU outside the current allowed affinity')
        topology = []
        for cpu in args.cpus:
            directory = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
            topology.append(tuple((directory / name).read_text().strip()
                                  for name in ('physical_package_id', 'core_id')))
        if len(set(topology)) != 4:
            parser.error('--cpus must select four distinct physical cores, not sibling hardware threads')
    if args.output:
        args.output = args.output.resolve()
        if args.output.exists():
            parser.error('--output must name a new directory')
    return args


def run(args, root):
    prefix = f'qtalh_cpu_{os.getpid()}:'
    port = 26000 + os.getpid() % 10000
    env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'config'), XDG_DATA_HOME=str(root / 'data'),
               QTALH_TEST_STYLE='', EPICS_CA_AUTO_ADDR_LIST='NO',
               EPICS_CA_ADDR_LIST=f'127.0.0.1:{port}', EPICS_CA_SERVER_PORT=str(port),
               EPICS_CA_REPEATER_PORT=str(port + 1), EPICS_CAS_INTF_ADDR_LIST='127.0.0.1',
               EPICS_CAS_BEACON_ADDR_LIST=f'127.0.0.1:{port + 1}')
    binaries = list(zip(args.labels, [args.before, args.after]))
    manifest = {'channels': args.channels, 'seconds': args.seconds, 'logged_seconds': args.logged_seconds,
                'warmup': 3, 'trials': args.trials, 'max_records': args.max_records,
                'modes': list(args.modes), 'cpus': args.cpus, 'prefix': prefix, 'xvfb': args.xvfb,
                'executables': {label: {'path': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
                                for label, binary in binaries}, 'commands': [], 'processes': []}
    owned = []

    def launch(command, log, role, **kwargs):
        cpu_index = {'app': 0, 'ioc': 1, 'xvfb': 2, 'monitor': 3}[role]
        command = [str(part) for part in command]
        if args.cpus:
            command = ['taskset', '-c', str(args.cpus[cpu_index])] + command
        with log.open('w') as output:
            process = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT, **kwargs)
        owned.append(process)
        manifest['processes'].append({'role': role, 'pid': process.pid})
        manifest['commands'].append(command)
        return process

    xserver = None
    try:
        if args.xvfb:
            read_fd, write_fd = os.pipe()
            try:
                xserver = launch(['Xvfb', '-displayfd', write_fd, '-screen', '0', '1600x1000x24', '-nolisten', 'tcp'],
                                 root / 'xvfb.log', 'xvfb', pass_fds=[write_fd])
            finally:
                os.close(write_fd)
            with os.fdopen(read_fd) as pipe:
                require(select.select([pipe], [], [], 10)[0], 'Xvfb startup timed out')
                display = pipe.readline().strip()
            require(display.isdigit(), 'Xvfb failed to provide a display; see xvfb.log')
            env.update(DISPLAY=':' + display, QT_QPA_PLATFORM='xcb')
        launch([args.epics_bin / 'caRepeater'], root / 'repeater.log', 'monitor')
        config = f'GROUP NULL benchmark\n$SEVRPV {prefix}rootSeverity\n'
        for i in range(args.channels):
            if i % 100 == 0:
                config += f'GROUP benchmark group{i // 100}\n'
            config += f'CHANNEL group{i // 100} {prefix}pv{i}\n'
        config_path = root / 'benchmark.alhConfig'
        config_path.write_text(config)
        with (root / 'trials.csv').open('w') as output, (root / 'validation.jsonl').open('w') as validation:
            writers = [csv.DictWriter(stream, fieldnames=FIELDS) for stream in (output, sys.stdout)]
            for writer in writers:
                writer.writeheader()
            for mode in args.modes:
                seconds = args.logged_seconds if mode == 'logged-updates' and args.logged_seconds is not None else args.seconds
                database = f'record(ao, "{prefix}rootSeverity") {{ field(VAL, "-1") }}\n'
                scan, calc = ('Passive', '0') if mode == 'idle' else ('.5 second', '10-VAL')
                for i in range(args.channels):
                    database += (f'record(calc, "{prefix}pv{i}") {{ field(PINI, "YES") field(CALC, "{calc}") '
                                 f'field(HIHI, "5") field(HHSV, "MAJOR") field(SCAN, "{scan}") }}\n')
                db_path = root / f'{mode}.db'
                db_path.write_text(database)
                ioc = launch([args.epics_bin / 'softIoc', '-d', db_path], root / f'{mode}-ioc.log', 'ioc', stdin=subprocess.PIPE)
                time.sleep(1)
                require(ioc.poll() is None, f'IOC failed to start; see {mode}-ioc.log')
                subprocess.run([str(args.epics_bin / 'caget'), '-w', '5', prefix + f'pv{args.channels - 1}'],
                               env=env, capture_output=True, check=True, timeout=10)
                for trial in range(1, args.trials + 1):
                    for label, binary in binaries[::1 if trial % 2 else -1]:
                        directory = root / f'{mode}-{trial}-{label}'
                        directory.mkdir()
                        subprocess.run([str(args.epics_bin / 'caput'), '-w', '5', prefix + 'rootSeverity', '-1'],
                                       env=env, capture_output=True, check=True, timeout=10)
                        monitor = launch(['stdbuf', '-oL', args.epics_bin / 'camonitor', '-t', 'n', prefix + 'rootSeverity'],
                                         directory / 'witness.log', 'monitor')
                        command = [binary, '-s', '-global', '-mainwindow', '-noerrorpopup', '-l', directory,
                                   '-a', 'alarm.log', '-o', 'operation.log', '-m', args.max_records]
                        if mode != 'logged-updates':
                            command.append('-D')
                        app = launch(command + [config_path], directory / 'app.log', 'app', cwd=directory)
                        time.sleep(3)
                        require(app.poll() is None, f'{label} exited during warmup; see {directory / "app.log"}')
                        if args.cpus:
                            require(os.sched_getaffinity(app.pid) == {args.cpus[0]}, 'Application affinity changed')
                        observed = witness_values(directory / 'witness.log')
                        require(observed and observed[-1] in ([0] if mode == 'idle' else [0, 2]),
                                f'{label} did not reach connected root severity: {observed}')
                        offset = len(observed)
                        start_xcpu = cpu_seconds(xserver) if xserver else None
                        start_cpu, start_wall = cpu_seconds(app), time.monotonic()
                        time.sleep(seconds)
                        cpu, wall = cpu_seconds(app) - start_cpu, time.monotonic() - start_wall
                        xcpu = cpu_seconds(xserver) - start_xcpu if xserver else None
                        measured = witness_values(directory / 'witness.log')[offset:]
                        require(ioc.poll() is None and monitor.poll() is None, 'IOC or monitor exited during measurement')
                        if xserver:
                            tree = subprocess.run(['xwininfo', '-root', '-tree'], env=env, text=True,
                                                  capture_output=True, check=True, timeout=10)
                            (directory / 'windows.txt').write_text(tree.stdout)
                        stop(app)
                        stop(monitor)
                        require(all(value in [0, 2] for value in measured), f'Invalid measured severity: {measured}')
                        require(all(value == 0 for value in measured) if mode == 'idle' else 0 in measured and 2 in measured,
                                f'Missing expected root states: {measured}')
                        evidence = dict(mode=mode, trial=trial, version=label, passed=True,
                                        root_values_during_measurement=measured, root_value_changes=len(measured))
                        if mode == 'logged-updates':
                            evidence.update(check_log(directory / 'alarm.log', prefix, args.channels))
                        else:
                            require(not (directory / 'alarm.log').exists() and not (directory / 'operation.log').exists(),
                                    'Disabled logging created output files')
                        row = dict(mode=mode, trial=trial, version=label, channels=args.channels,
                                   wall_seconds=f'{wall:.6f}', cpu_seconds=f'{cpu:.6f}', cpu_percent=f'{100 * cpu / wall:.4f}',
                                   xvfb_cpu_seconds=f'{xcpu:.6f}' if xcpu is not None else '',
                                   xvfb_cpu_percent=f'{100 * xcpu / wall:.4f}' if xcpu is not None else '')
                        for writer in writers:
                            writer.writerow(row)
                        output.flush()
                        sys.stdout.flush()
                        validation.write(json.dumps(evidence) + '\n')
                        validation.flush()
                        print(f'{mode} {trial} {label}: validated', file=sys.stderr, flush=True)
                stop(ioc)
    finally:
        for process in reversed(owned):
            stop(process)
        manifest['environment'] = {key: value for key, value in env.items()
                                   if key.startswith(('EPICS_', 'QT_', 'QTALH_', 'XDG_')) or key == 'DISPLAY'}
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def interrupted(signum, frame):
    raise SystemExit(128 + signum)


def main():
    args = arguments()
    signal.signal(signal.SIGTERM, interrupted)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.output) if args.output else tempfile.TemporaryDirectory(prefix='qtalh-cpu-')
    with context as directory:
        run(args, Path(directory).resolve())


if __name__ == '__main__':
    main()
