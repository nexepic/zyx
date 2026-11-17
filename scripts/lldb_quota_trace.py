"""One-shot LLDB quota trace; inspect symbols, never target data values."""
import json
import os
from pathlib import Path
import lldb

quota_stop = False
quota_reports = 0
stack_limit = 1


def on_quota(frame, _location, _internal):
    global quota_stop, quota_reports
    if quota_reports >= stack_limit:
        return False
    quota_stop = True
    quota_reports += 1
    print('CI_QUOTA_BACKTRACE begin maximum_frames=32', flush=True)
    thread = frame.GetThread()
    symbol_only = os.environ.get('ZYX_CI_QUOTA_SYMBOL_ONLY') == '1'
    if symbol_only:
        # PDB symbol lookup itself can crash LLDB (Build 247), even without
        # GetFunctionName. Flush every raw PC before any symbol resolution so
        # a later debugger failure does not discard the rest of the stack.
        for index in range(32):
            current = thread.GetFrameAtIndex(index)
            if not current.IsValid():
                break
            print('CI_QUOTA_PC ' + json.dumps({'index': index, 'pc': current.GetPC()}), flush=True)
    for index in range(32):
        current = thread.GetFrameAtIndex(index)
        if not current.IsValid():
            break
        # Windows LLDB crashed completing C++ PDB types through GetFunctionName
        # in Build 244. Public symbols and PCs avoid function/type expansion.
        line = None if symbol_only else current.GetLineEntry()
        function = (current.GetSymbol().GetName() if symbol_only else current.GetFunctionName()) or '<unknown>'
        print('CI_QUOTA_FRAME ' + json.dumps({
            'index': index,
            'function': function[:512],
            'symbol_truncated': len(function) > 512,
            'module': (current.GetModule().GetFileSpec().GetFilename() or '')[:128],
            'file': (line.GetFileSpec().GetFilename() or '')[:128] if line else '',
            'line': line.GetLine() if line else 0,
            'symbol_only': symbol_only,
            'pc': current.GetPC() if symbol_only else None,
        }), flush=True)
    if stack_limit > 1 and quota_reports == stack_limit:
        _location.GetBreakpoint().SetEnabled(False)
        print(f'CI_QUOTA_BACKTRACE_LIMIT reports={quota_reports}', flush=True)
    return False


def __lldb_init_module(debugger, _internal):
    global stack_limit
    stack_limit = max(1, min(3, int(os.environ.get('ZYX_CI_QUOTA_STACK_LIMIT', '1'))))
    target = debugger.GetSelectedTarget()
    breakpoint = target.BreakpointCreateByRegex('MaterializedByteBudget::diagnosticRejection')
    print(f'CI_QUOTA_BREAKPOINT locations={breakpoint.GetNumLocations()}', flush=True)
    if not breakpoint.GetNumLocations():
        raise RuntimeError('quota diagnostic symbol unavailable')
    breakpoint.SetOneShot(stack_limit == 1)
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
