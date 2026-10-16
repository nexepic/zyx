import os
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from runner.adapters import kuzu as adapter

class CapabilityProbeResourceTest(unittest.TestCase):
    def test_probe_honors_mapping_pool_and_thread_limits_before_classifying_support(self):
        calls=[]
        class Database:
            def __init__(self,path,**kwargs): calls.append(kwargs)
            def close(self):pass
        class Connection:
            def __init__(self,db,**kwargs):self.kwargs=kwargs
            def execute(self,query):
                if query.startswith("CREATE INDEX"):
                    raise RuntimeError("Parser exception: CREATE INDEX is unsupported")
                return SimpleNamespace(has_next=lambda:False,close=lambda:None)
            def close(self):pass
        module=SimpleNamespace(Database=Database,Connection=Connection)
        with patch.dict(sys.modules,{"kuzu":module}),patch.dict(os.environ,{"KUZU_BENCH_BUFFER_POOL_BYTES":str(64*1024**2),"KUZU_BENCH_MAX_DB_BYTES":str(2*1024**3)}),patch.object(adapter,"_SECONDARY_PROPERTY_INDEX_SUPPORT",None):
            self.assertFalse(adapter.supports_secondary_property_indexes())
            self.assertFalse(adapter.supports_secondary_property_indexes())
        self.assertEqual(calls,[{"buffer_pool_size":64*1024**2,"max_db_size":2*1024**3,"max_num_threads":1}])
