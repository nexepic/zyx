import sys,runpy
sys.path.insert(0,'/workspace/zyx/benchmarks/graphdb-comparison')
from runner.baseline import BASELINE_WORKLOADS
BASELINE_WORKLOADS['indexed']=['point_lookup_indexed']
runpy.run_path('/tmp/zyx-final-c741-window.py',run_name='__main__')
