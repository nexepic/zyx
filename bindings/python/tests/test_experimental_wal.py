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
