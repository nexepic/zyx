"""Linux benchmark resources; a fresh launcher isolates oracle-parent RSS."""
from pathlib import Path
import json
import os
import signal
import subprocess
import tempfile


def run(command, stdout, stderr, env, limits, timeout=1200):
    launcher = Path(__file__).with_name('resource-launcher')
    with tempfile.TemporaryDirectory(prefix='zyx-worker-resources-') as temporary:
        receipt = Path(temporary) / 'usage.json'
        args = [str(launcher), str(receipt), str(limits['address_space_bytes']),
                str(limits['file_bytes']), *command]
        process = subprocess.Popen(args, stdout=stdout, stderr=stderr, env=env,
                                   start_new_session=True)
        timed_out = False
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        if receipt.exists():
            result = json.loads(receipt.read_text())
        else:
            result = {'returncode': process.returncode, 'resources_unavailable': True}
        result.update(timed_out=timed_out, launcher_returncode=process.returncode,
                      scope='whole worker including open/copy/queries/close; '
                      'wait4 from fresh launcher, not oracle controller; '
                      'OS blocks are not attributed query I/O')
        return result
