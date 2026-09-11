#!/usr/bin/env python3
"""Compare complete QtALH binaries against a private loopback soft IOC (Linux).

Requires a Qt platform (e.g. QT_QPA_PLATFORM=offscreen or DISPLAY on private Xvfb).
CPU percent is the application's process CPU time / wall time, with 100% = one core.
The IOC, X server, and startup are excluded. No production PVs are accessed.
"""
import argparse
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def cpu_seconds(process):
    if process.poll() is not None:
        raise RuntimeError("QtALH exited during measurement")
    fields = Path('/proc/{}/stat'.format(process.pid)).read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--epics-bin', type=Path, default=Path('/usr/local/oag/base/bin/linux-x86_64'))
    parser.add_argument('--channels', type=int, default=1000)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--modes', nargs='+', choices=('idle', 'updates', 'logged-updates'),
                        default=('idle', 'updates', 'logged-updates'))
    args = parser.parse_args()
    if min(args.channels, args.seconds, args.trials) < 1:
        parser.error('counts and durations must be positive')
    binaries = [('before', str(args.before.resolve())), ('after', str(args.after.resolve()))]
    port = 26000 + os.getpid() % 10000
    prefix = 'qtalh_cpu_{}:'.format(os.getpid())
    env = dict(os.environ, EPICS_CA_AUTO_ADDR_LIST='NO',
               EPICS_CA_ADDR_LIST='127.0.0.1:{}'.format(port),
               EPICS_CA_SERVER_PORT=str(port), EPICS_CA_REPEATER_PORT=str(port + 1),
               EPICS_CAS_INTF_ADDR_LIST='127.0.0.1',
               EPICS_CAS_BEACON_ADDR_LIST='127.0.0.1:{}'.format(port + 1))
    writer = csv.writer(sys.stdout)
    writer.writerow(['mode', 'trial', 'version', 'channels', 'wall_seconds', 'cpu_seconds', 'cpu_percent'])
    with tempfile.TemporaryDirectory(prefix='qtalh-cpu-') as directory:
        root = Path(directory)
        repeater = ioc = app = None
        try:
            with (root / 'repeater.log').open('w') as log:
                repeater = subprocess.Popen([str(args.epics_bin / 'caRepeater')], env=env,
                                            stdout=log, stderr=subprocess.STDOUT)
            config = 'GROUP NULL benchmark\n'
            for i in range(args.channels):
                if i % 100 == 0:
                    config += 'GROUP benchmark group{}\n'.format(i // 100)
                config += 'CHANNEL group{} {}pv{}\n'.format(i // 100, prefix, i)
            (root / 'benchmark.alhConfig').write_text(config)
            for mode in args.modes:
                database = ''
                for i in range(args.channels):
                    database += ('record(calc, "' + prefix + 'pv' + str(i) + '") {\n'
                                 ' field(CALC, "10-VAL")\n field(HIHI, "5")\n'
                                 ' field(HHSV, "MAJOR")\n field(SCAN, "' +
                                 ('Passive' if mode == 'idle' else '.5 second') + '")\n}\n')
                (root / 'benchmark.db').write_text(database)
                with (root / 'ioc.log').open('w') as log:
                    ioc = subprocess.Popen([str(args.epics_bin / 'softIoc'), '-d',
                                            str(root / 'benchmark.db')], env=env, stdin=subprocess.PIPE,
                                           stdout=log, stderr=subprocess.STDOUT)
                time.sleep(1)
                if ioc.poll() is not None:
                    raise RuntimeError((root / 'ioc.log').read_text())
                subprocess.run([str(args.epics_bin / 'caget'), '-w', '5', prefix + 'pv' +
                                str(args.channels - 1)], env=env, stdout=subprocess.PIPE, check=True)
                for trial in range(args.trials):
                    # Alternate order to reduce systematic temperature/load bias.
                    for version, binary in binaries[::1 if trial % 2 == 0 else -1]:
                        run_dir = root / '{}-{}-{}'.format(mode, trial, version)
                        run_dir.mkdir()
                        command = [binary, '-s', '-mainwindow', '-noerrorpopup', '-l', str(run_dir),
                                   '-a', 'alarm.log', '-o', 'operation.log']
                        if mode != 'logged-updates':
                            command.append('-D')
                        command.append(str(root / 'benchmark.alhConfig'))
                        with (root / 'app.log').open('w') as log:
                            app = subprocess.Popen(command, cwd=str(run_dir), env=env,
                                                   stdout=log, stderr=subprocess.STDOUT)
                        time.sleep(3)
                        start_cpu, start_wall = cpu_seconds(app), time.monotonic()
                        time.sleep(args.seconds)
                        cpu, wall = cpu_seconds(app) - start_cpu, time.monotonic() - start_wall
                        writer.writerow([mode, trial, version, args.channels,
                                         '{:.6f}'.format(wall), '{:.6f}'.format(cpu),
                                         '{:.4f}'.format(100 * cpu / wall)])
                        sys.stdout.flush()
                        stop(app)
                        if mode == 'logged-updates':
                            alarm = (run_dir / 'alarm.log').read_text()
                            if 'MAJOR' not in alarm or 'NO_ALARM' not in alarm:
                                raise RuntimeError('Expected both alarm transitions in the log')
                stop(ioc)
        finally:
            stop(app)
            stop(ioc)
            stop(repeater)


if __name__ == '__main__':
    main()
