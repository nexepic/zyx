"""Public entry tests run when the experimental extension and kernel are installed."""
import os
import signal
import subprocess
import sys
from pathlib import Path

import pytest
import zyxdb

pytest.importorskip('zyxdb._wal_plan')
pytest.importorskip('zyx_development_wal.adapter')

def db_at(p):
    return zyxdb.Database(str(p),backend='experimental-wal')

def read(db,q,**kw):
    r=db.execute(q,**kw)
    assert r.is_success,r.error
    return [row.values() for row in r]

def test_snapshot_and_stream(tmp_path):
    p=tmp_path/'wal';a=db_at(p)
    ids=a.create_nodes('Item',[{'id':1,'val':3},{'id':2,'grp':None}])
    edge=a.create_edge(*ids,'Link',{'w':4})
    a.create_edges('Link',[(ids[0],ids[1],{'w':6}),(ids[0],ids[0],{'w':2})])
    assert edge>0
    b=db_at(p);old=a.begin_read_only_transaction()
    q='MATCH (arbitrary:Item) RETURN arbitrary.id AS key, arbitrary.grp AS groupValue'
    r=old.execute(q);assert r.fetchone()['key']==1
    t=b.begin_transaction();token=t.experimental_commit_token
    read(t,'UNWIND $values AS v CREATE (:Item {id:v, val:7})',values=[3,4])
    assert read(t,'MATCH (x:Item {id:$k}) RETURN x.val AS value',k=4)==[[7]]
    t.commit()
    assert b.experimental_transaction_outcome(token) is not None
    assert [row.values() for row in r]==[[2,None]]
    assert read(old,q)==[[1,None],[2,None]]
    old.rollback()
    assert sorted(read(b,'MATCH (a:Item {id:$key})-[r:Link]->(b:Item) RETURN b.id AS k,r.w AS w',key=1))==[[1,2],[2,4],[2,6]]
    assert sorted(read(b,'MATCH (a:Item {id:$key})<-[r:Link]-(b:Item) RETURN b.id AS k,r.w AS w',key=2))==[[1,4],[1,6]]
    assert sorted(read(b,'MATCH (a:Item {id:$key})-[r:Link]-(b:Item) RETURN b.id AS k,r.w AS w',key=1))==[[1,2],[2,4],[2,6]]
    assert read(b,'MATCH (whatever:Item) WHERE whatever.val=$x RETURN whatever.id AS id LIMIT 1',x=7)==[[3]]
    assert not b.execute('MATCH (n:Item) RETURN n.id AS key ORDER BY key').is_success
    assert read(b,'MATCH (n:Item {id:$k}) RETURN n.val AS v',k=None)==[]
    assert not b.execute('MATCH (n:Item {id:$missing}) RETURN n.val AS v').is_success
    b.close();a.close();a=db_at(p)
    assert read(a,'MATCH (n:Item {id:$k}) RETURN n.val AS v',k=4)==[[7]];a.close()

def test_public_uncertain(tmp_path,monkeypatch):
    from zyx_development_wal.durable import IO,Uncertain
    p=tmp_path/'wal';a=db_at(p);t=a.begin_transaction();token=t.experimental_commit_token
    read(t,'CREATE (:Item {id:9})')
    original=IO.event
    def lost(self,label):
        if label=='commit.before_reply':raise OSError('lost acknowledgement')
        return original(self,label)
    monkeypatch.setattr(IO,'event',lost)
    with pytest.raises(Uncertain):t.commit()
    assert a.experimental_transaction_outcome(token)==1
    a.close()
    a=db_at(p);assert read(a,'MATCH (n:Item) RETURN n.id AS k')==[[9]];a.close()

def test_public_kill_and_missing_wal(tmp_path):
    p=tmp_path/'wal'
    code="""import zyxdb,sys,signal
db=zyxdb.Database(sys.argv[1],backend='experimental-wal')
tx=db.begin_transaction()
assert tx.execute('CREATE (:Item {id:17, val:31})').is_success
token=tx.experimental_commit_token
tx.commit()
print(token,flush=True)
signal.pause()
"""
    proc=subprocess.Popen([sys.executable,'-c',code,str(p)],stdout=subprocess.PIPE,text=True)
    try:
        token=proc.stdout.readline().strip();assert len(token)==32
    finally:proc.kill();proc.wait()
    require=p/'wal';require.rename(p/'saved')
    from zyx_development_wal.durable import Corrupt
    with pytest.raises(Corrupt):db_at(p)
    (p/'saved').rename(require)
    db=db_at(p);assert db.experimental_transaction_outcome(token)==1
    assert read(db,'MATCH (k:Item {id:$i}) RETURN k.val AS v',i=17)==[[31]]
    db.close()

def test_public_update_and_detach(tmp_path):
    db=db_at(tmp_path/'db')
    t=db.begin_transaction()
    read(t,'UNWIND $ids AS i CREATE (:Item {id:i,val:i,grp:0})',ids=list(range(300)))
    t.commit()
    a=db.create_node('Item',{'id':-1});b=db.create_node('Item',{'id':-2})
    db.create_edges('Link',[(a,b,{'w':4}),(a,b,{'w':9})])
    peer=db_at(tmp_path/'db');old=peer.begin_read_only_transaction()
    t=db.begin_transaction()
    read(t,'MATCH (x:Item {id:$key}) SET x.val=$v',key=-1,v=17)
    q='MATCH (x:Item {id:$key}) RETURN x.val AS v'
    assert read(t,q,key=-1)==[[17]]
    t.rollback();assert read(db,q,key=-1)==[[None]]
    read(db,'MATCH (x:Item) SET x.grp=x.val')
    read(db,'MATCH (x:Item {id:$key}) SET x.val=NULL',key=7)
    assert read(db,'MATCH (x:Item {id:7}) RETURN x.val AS v,x.grp AS g')==[[None,7]]
    read(db,'MATCH (all:Item) DETACH DELETE all')
    assert read(db,q,key=-1)==[] and read(old,q,key=-1)==[[None]]
    assert len(read(old,'MATCH (a:Item)-[r:Link]->(b:Item) RETURN r.w AS w'))==2
    old.rollback();peer.close()
    nid=db.create_node('Item',{'id':-1})
    assert nid>b
    db.close()

def test_stream_uses_payload_of_key_cursor(tmp_path,monkeypatch):
    from zyx_development_wal.durable import Transaction as KernelTransaction
    db=db_at(tmp_path/'db')
    db.create_nodes('Item',[{'id':i,'val':i} for i in range(16)])
    def duplicate_lookup(self,key):
        raise AssertionError('full scan performed another key lookup')
    monkeypatch.setattr(KernelTransaction,'get',duplicate_lookup)
    assert read(db,'MATCH (all:Item) RETURN all.val AS v LIMIT 3')==[[0],[1],[2]]
    db.close()

@pytest.mark.parametrize('backend',['native','experimental-wal'])
def test_public_graph_ids_survive_reopen(tmp_path,backend):
    path=str(tmp_path/'db')
    db=zyxdb.Database(path,backend=backend)
    a,b=db.create_nodes('Item',[{'id':120},{'id':88}])
    e1,e2=db.create_edges('Link',[(a,b,{'w':9}),(a,b,{'w':9})])
    q='MATCH (start:Item)-[rel:Link]->(end:Item) RETURN id(start) AS a,ID(rel) AS eid,id(end) AS b'
    expected=[[a,e1,b],[a,e2,b]]
    assert sorted(read(db,q))==expected
    db.close()
    db=zyxdb.Database(path,backend=backend)
    assert sorted(read(db,q))==expected
    read(db,'MATCH (n:Item {id:88}) DETACH DELETE n')
    replacement=db.create_node('Item',{'id':88})
    assert replacement!=b and read(db,q)==[]
    db.close()

@pytest.mark.parametrize('backend',['native','experimental-wal'])
def test_nested_equality_keeps_types_and_null(tmp_path,backend):
    db=zyxdb.Database(str(tmp_path/'db'),backend=backend)
    values=read(db,'RETURN (1=1)=1 AS mixed,(1=1)=(2=2) AS both,NULL=NULL AS missing')
    assert values==[[False,True,None]]
    db.close()

@pytest.mark.parametrize('backend', ['native', 'experimental-wal'])
def test_typed_scan_through_commit_and_reopen(tmp_path, backend):
    path = str(tmp_path / 'typed')
    db = zyxdb.Database(path, backend=backend)
    values = [False, 0, -0.0, 0.0, True, 1, 1.0, -2.5]
    with db.begin_transaction() as tx:
        for i, value in enumerate(values):
            read(tx, 'CREATE (:Item {id:$k,val:$v})', k=i, v=value)
        read(tx, 'CREATE (:Item {id:8})')
        read(tx, 'CREATE (:Item {id:9,val:NULL})')
    db.close()
    db = zyxdb.Database(path, backend=backend)
    lookup = 'MATCH (n:Item) WHERE n.val=$value RETURN n.id AS k,n.val AS v'
    for value in values + [None]:
        rows = sorted(read(db, lookup, value=value))
        expected = [[i,v] for i,v in enumerate(values) if type(v) is type(value) and v == value]
        assert rows == expected
        assert all(type(r[1]) is type(value) for r in rows)
        assert sorted(read(db, 'MATCH (n:Item {val:$value}) RETURN n.id AS k', value=value)) == [[i] for i,_ in expected]
    assert read(db, 'RETURN true AS yes,2.5 AS d') == [[True, 2.5]]
    assert len(read(db,'MATCH (n:Item) RETURN n.grp AS missing LIMIT 3')) == 3
    db.close()


def test_shared_checkpoint_from_public_entry(tmp_path):
    p = tmp_path / 'shared'
    db = db_at(p)
    peer = db_at(p)
    db.create_node('Item', {'id':1, 'val':True})
    old = peer.begin_read_only_transaction()
    query = 'MATCH (n:Item) RETURN n.val AS v'
    assert read(old,query) == [[True]]
    db.save()
    for i in range(12):
        read(db,'MATCH (n:Item {id:1}) SET n.val=$v',v=i+0.5)
        db.save()
        assert read(old,query) == [[True]]
    old.rollback()
    assert read(peer,query) == [[11.5]]
    db.close()
    assert (p/'wal').exists()
    peer.close()
    assert list(p.iterdir()) == [p/'main']


def test_typed_stream_matches_operator_fallback(tmp_path, monkeypatch):
    from zyx_development_wal import operators
    db = db_at(tmp_path/'oracle')
    db.create_nodes('Item', [{'id': i, 'val': v} for i,v in enumerate([True,1,1.0,-0.0,0.0,None])])
    tx = db.begin_transaction()
    read(tx,'MATCH (n:Item {id:0}) SET n.val=$v',v=False)
    queries = [
        ('MATCH (n:Item) WHERE $v=n.val RETURN id(n) AS eid,n.val AS v LIMIT 2',{'v':0.0}),
        ('MATCH (n:Item {val:$v}) RETURN n.val AS v,n.grp AS g',{'v':None}),
        ('MATCH (n:Item) RETURN n.val AS v,n.id AS k',{}),
        ('MATCH (n:Item) WHERE n.val=$v RETURN n.id AS k',{'v':False}),
        ('MATCH (n:Item) WHERE n.val=$v RETURN n.id AS k',{'v':1.0}),
    ]
    direct = [read(tx,q,**params) for q,params in queries]
    monkeypatch.setattr(operators,'projected_scan',lambda *_: None)
    fallback = [read(tx,q,**params) for q,params in queries]
    assert repr(direct) == repr(fallback)
    tx.rollback()
    assert read(db,'MATCH (n:Item {id:0}) RETURN n.val AS v') == [[True]]
    db.close()


def test_host_point_in_time_export_and_restore(tmp_path):
    import shutil
    path = tmp_path/'live'
    backup = tmp_path/'independent.zyx'
    db = db_at(path)
    a,b = db.create_nodes('Item',[{'id':17,'val':1.5},{'id':42,'val':True}])
    edge = db.create_edges('Link',[(a,b,{'w':7})])[0]
    # The live main is required-WAL here. save() is not used for this backup.
    raw=tmp_path/'required-only.zyx'
    shutil.copyfile(path/'main',raw)
    from zyx_development_wal.durable import Corrupt
    with pytest.raises(Corrupt,match='WAL'):
        db_at(raw)
    with db.begin_transaction() as second:
        read(second,'MATCH (n:Item {id:17}) SET n.val=9')
        read(second,'CREATE (:Item {id:99,val:false})')
        assert db.export_snapshot(str(backup))==2
    assert read(db,'MATCH (n:Item {id:99}) RETURN n.val AS v')==[[False]]
    # New process, separate working directory, sole input is the exported file.
    isolated=tmp_path/'isolated'
    isolated.mkdir()
    single=isolated/'only.zyx'
    shutil.copyfile(backup,single)
    script="""
import json,sys
from zyxdb import Database
db=Database('only.zyx',backend='experimental-wal')
def vals(q):
    r=db.execute(q)
    assert r.is_success,r.error
    return [row.values() for row in r]
print(json.dumps([vals('MATCH (n:Item) RETURN n.id AS k,n.val AS v,id(n) AS sid'),
                  vals('MATCH (n:Item)-[r:Link]->(m:Item) RETURN id(r) AS rid,r.w AS w'),
                  vals('MATCH (n:Item {id:99}) RETURN n.val AS v')]))
try:db.begin_transaction()
except PermissionError:pass
else:raise AssertionError('writable export')
db.close()
"""
    import json
    rows,edges,absent=json.loads(subprocess.check_output([sys.executable,'-c',script],cwd=isolated,text=True))
    assert rows==[[17,1.5,a],[42,True,b]] and type(rows[1][1]) is bool
    assert edges==[[edge,7]] and absent==[]
    assert list(isolated.iterdir())==[single]
    restored=zyxdb.Database.restore_snapshot(str(single),str(tmp_path/'restored'),backend='experimental-wal')
    assert read(restored,'MATCH (n:Item {id:17}) RETURN id(n) AS sid,n.val AS v')==[[a,1.5]]
    assert restored.create_node('Item',{'id':100})>max(a,b)
    restored.close()
    assert read(db,'MATCH (n:Item {id:100}) RETURN n.val AS v')==[]
    db.close()
    assert set(x.name for x in path.iterdir()) == {'main'}
    clean = isolated/'clean-B.zyx'
    shutil.copyfile(path/'main', clean)
    bview = db_at(clean)
    assert read(bview,'MATCH (n:Item {id:99}) RETURN n.val AS v') == [[False]]
    assert read(bview,'MATCH (n:Item {id:17}) RETURN id(n) AS sid,n.val AS v') == [[a,9]]
    bview.close()
    invalid_destination = tmp_path/'invalid-restore'
    with pytest.raises(Corrupt,match='WAL'):
        zyxdb.Database.restore_snapshot(str(raw),str(invalid_destination),backend='experimental-wal')
    assert not invalid_destination.exists()


def test_pending_primary_key_queries_keep_the_overlay(tmp_path, monkeypatch):
    from zyx_development_wal.snapshot import Image
    db=db_at(tmp_path/'delta')
    t=db.begin_transaction()
    read(t,'CREATE (:Item {id:9,val:2.5,grp:true})')
    def forbid(*args):
        raise AssertionError('point lookup materialized the whole write view')
    with monkeypatch.context() as m:
        m.setattr(Image,'clone',forbid)
        assert read(t,'MATCH (n:Item {id:$k,grp:true}) RETURN n.val AS v',k=9)==[[2.5]]
        assert read(t,'MATCH (n:Item) WHERE $k=n.id RETURN n.grp AS g',k=9)==[[True]]
        assert read(t,'MATCH (n:Item) WHERE n.id=$k RETURN id(n) AS eid',k=None)==[]
        assert read(t,'MATCH (n:Item {id:$k}) RETURN n.val AS v',k=True)==[]
    t.rollback()
    assert read(db,'MATCH (n:Item {id:9}) RETURN id(n) AS eid')==[]
    db.close()

def test_query_only_reader_keeps_its_committed_view(tmp_path):
    live=tmp_path/'live'
    writer=db_at(live)
    writer.create_nodes('Item',[{'id':k,'val':v,'grp':True} for k,v in
                               enumerate([None,False,-0.0,0.0,1,1.0])])
    writer.save()
    ro=zyxdb.Database(str(live),backend='experimental-wal',query_only=True)
    old=ro.begin_read_only_transaction()
    assert read(old,'MATCH (n:Item) WHERE n.val=$v RETURN n.id AS k',v=0.0)==[[2],[3]]
    for q in ('CREATE (:Item {id:7})','MATCH (n:Item {id:1}) SET n.val=3'):
        with pytest.raises(PermissionError):old.execute(q)
    old.rollback()
    for operation in (ro.begin_transaction,ro.save,lambda:ro.create_node('Item',{'id':7}),
                      lambda:ro.execute('CREATE (:Item {id:7})')):
        with pytest.raises(PermissionError):operation()
    old=ro.begin_read_only_transaction()
    assert read(old,'MATCH (n:Item {id:1}) RETURN n.val AS v')==[[False]]
    writer.execute('MATCH (n:Item {id:1}) SET n.val=44')
    writer.create_node('Item',{'id':8,'val':True})
    assert read(old,'MATCH (n:Item {id:1}) RETURN n.val AS v')==[[False]]
    old.commit()
    assert read(ro,'MATCH (n:Item) WHERE n.val=$v RETURN n.id AS k',v=True)==[[8]]
    assert read(ro,'MATCH (n:Item {id:1}) RETURN n.val AS v')==[[44]]
    assert read(ro,'MATCH (n:Item) RETURN n.id AS k LIMIT 1')==[[0]]
    writer.save()
    assert read(ro,'MATCH (n:Item) WHERE n.val=$v RETURN n.id AS k',v=1.0)==[[5]]
    ro.close();writer.close()
    with pytest.raises(FileNotFoundError):
        zyxdb.Database(str(tmp_path/'absent'),backend='experimental-wal',query_only=True)

def test_query_only_projection_matches_scalar_and_large_receipt_history(tmp_path):
    live = tmp_path / 'types'
    w = db_at(live)
    w.create_nodes('Item',[{'id':k,'val':v} for k,v in enumerate(
        [None,False,True,-0.0,0.0,1.0,1,-(1<<63),(1<<63)-1])])
    w.create_node('Item',{'id':9})
    w.save()
    r = zyxdb.Database(str(live),backend='experimental-wal',query_only=True)
    for v in (None,False,True,0.0,1,1.0,-(1<<63),(1<<63)-1):
        sql='MATCH (n:Item) WHERE n.val=$v RETURN n.id AS k,n.val AS v'
        assert repr(read(r,sql,v=v)) == repr(read(w,sql,v=v))
    for j in range(132):
        read(w,'MATCH (n:Item {id:4}) SET n.val=$v',v=j)
    w.save()
    assert read(r,'MATCH (n:Item {id:4}) RETURN n.val AS v') == [[131]]
    read(w,'MATCH (n:Item {id:4}) SET n.val=$v',v=132)
    assert read(r,'MATCH (n:Item {id:4}) RETURN n.val AS v') == [[132]]
    w.close();r.close()

def test_query_only_full_typed_multiset_and_stream_boundaries(tmp_path):
    live = tmp_path / 'windows'
    writer = db_at(live)
    writer.create_nodes('Item', [
        {'id': 2*k, 'val': [False, 1, -0.0, None][k%4], 'grp': k%7}
        for k in range(2060)
    ])
    writer.save()
    reader = zyxdb.Database(str(live), backend='experimental-wal', query_only=True)
    all_rows = 'MATCH (n:Item) RETURN n.id AS k,id(n) AS eid,n.val AS v,n.grp AS g'
    filtered = ('MATCH (n:Item) WHERE n.grp=$g '
                'RETURN n.id AS k,id(n) AS eid,n.val AS v')
    # Pin before writes. Check the original ordered typed output after publication.
    cursor = reader.execute(all_rows)
    before = read(writer, all_rows)
    assert repr(next(iter(cursor)).values()) == repr(before[0])
    tx = writer.begin_transaction()
    for k in (0, 2046, 2048, 4096):
        read(tx, 'MATCH (n:Item {id:$k}) SET n.grp=$g', k=k, g=4)
    for k in (-1, 1, 2047, 4097, 5001):
        read(tx, 'CREATE (:Item {id:$k,val:$v,grp:$g})', k=k, v=-0.0, g=4)
    tx.commit()
    assert repr([r.values() for r in cursor]) == repr(before[1:])
    for suffix in ('', ' LIMIT 0', ' LIMIT 1', ' LIMIT 128', ' LIMIT 2048'):
        for q, params in ((all_rows, {}), (filtered, {'g':4}), (filtered, {'g':99})):
            assert repr(read(reader,q+suffix,**params)) == repr(read(writer,q+suffix,**params))
    writer.save()
    assert repr(read(reader,all_rows)) == repr(read(writer,all_rows))
    reader.close()
    writer.close()
