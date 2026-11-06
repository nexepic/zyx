"""One-shot LLDB quota trace; inspect symbols, never target data values."""
import json
import os
from pathlib import Path
import lldb

quota_stop = False


def on_quota(frame, _location, _internal):
    global quota_stop
    quota_stop = True
    print('CI_QUOTA_BACKTRACE begin maximum_frames=32', flush=True)
    thread = frame.GetThread()
    for index in range(32):
        current = thread.GetFrameAtIndex(index)
        if not current.IsValid():
            break
        line = current.GetLineEntry()
        function = current.GetFunctionName() or current.GetSymbol().GetName() or '<unknown>'
        print('CI_QUOTA_FRAME ' + json.dumps({
            'index': index,
            'function': function[:512],
            'symbol_truncated': len(function) > 512,
            'module': (current.GetModule().GetFileSpec().GetFilename() or '')[:128],
            'file': (line.GetFileSpec().GetFilename() or '')[:128],
            'line': line.GetLine(),
        }), flush=True)
    return False


def __lldb_init_module(debugger, _internal):
    target = debugger.GetSelectedTarget()
    breakpoint = target.BreakpointCreateByRegex('MaterializedByteBudget::diagnosticRejection')
    print(f'CI_QUOTA_BREAKPOINT locations={breakpoint.GetNumLocations()}', flush=True)
    if not breakpoint.GetNumLocations():
        raise RuntimeError('quota diagnostic symbol unavailable')
    breakpoint.SetOneShot(True)
    breakpoint.SetAutoContinue(True)
    breakpoint.SetScriptCallbackFunction(__name__ + '.on_quota')


def report_exit(debugger):
    process = debugger.GetSelectedTarget().GetProcess()
    exited = process.IsValid() and process.GetState() == lldb.eStateExited
    record = {'exited': exited, 'exit_code': process.GetExitStatus() if exited else None,
              'quota_stop': quota_stop}
    if process.IsValid() and not exited:
        # This target was launched by our diagnostic, never attached to a PID.
        error = process.Kill()
        record['cleanup_succeeded'] = error.Success()
    Path(os.environ['ZYX_CI_QUOTA_STATUS_PATH']).write_text(json.dumps(record) + '\n')
    print('CI_QUOTA_PROCESS ' + json.dumps(record), flush=True)
