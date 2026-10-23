from pathlib import Path
from collections import Counter,defaultdict
import subprocess,json,re,ctypes,hashlib
root=Path('/workspace/zyx/benchmarks/architecture-design/results/20261022-core-cost/current-cpu')
lib=ctypes.CDLL('libc.so.6');dl=ctypes.CDLL(None)
class Info(ctypes.Structure):_fields_=[('path',ctypes.c_char_p),('base',ctypes.c_void_p),('name',ctypes.c_char_p),('address',ctypes.c_void_p)]
starts={}
for name in ['memcmp','memcpy','memset','strcmp','strlen']:
 addr=ctypes.cast(getattr(lib,name),ctypes.c_void_p).value;info=Info();dl.dladdr(ctypes.c_void_p(addr),ctypes.byref(info));starts[name]=addr-info.base
# IFUNC implementations have bounded text regions verified on this host; retain
# every raw PC, and do not turn nearest exported libc symbols into false names.
widths={'memcmp':0x600,'memcpy':0x800,'memset':0x400,'strcmp':0x700,'strlen':0x400}
all_results=[]
for case in ['label_scan_filter','edge_batch_1000','property_set']:
 groups=defaultdict(Counter)
 lines=(root/(case+'-final-sampled.pcs')).read_text().splitlines()
 for line in lines[1:]:
  path,addr,weight=line.split('\t');groups[path][addr]+=int(weight)
 funcs=Counter();cats=Counter()
 for path,addresses in groups.items():
  ordered=list(addresses);names=['unknown']*len(ordered)
  if Path(path).is_file() and not path.endswith('libc.so.6'):
   p=subprocess.run(['addr2line','-f','-C','-e',path],input=''.join('0x'+a+'\n' for a in ordered),capture_output=True,text=True,check=True)
   names=p.stdout.splitlines()[::2];assert len(names)==len(ordered)
  for a,f in zip(ordered,names):
   v=addresses[a];cat='other/unattributed'
   if path.endswith('libc.so.6'):
    f='libc internal/unresolved';cat='libc internal/unattributed'
    for n,start in starts.items():
     if start<=int(a,16)<start+widths[n]:f='libc IFUNC '+n;cat='memory/string comparison' if n in ['memcmp','strcmp','strlen'] else 'memory copy/clear';break
   elif path=='linux-vdso.so.1':f='vDSO clock/runtime';cat='clock/runtime'
   elif 'crc32' in f or 'calculateCrc' in f:cat='checksums'
   elif any(n in f for n in ['TransactionBTreeOverlay','PackedBTreePageCodec','InitialBTreeBuilder','BTreePageStore::plan']):cat='BTree mutation/packing'
   elif any(n in f for n in ['OwnedHeapReservation','PrepaidMemoryResource','ResourceAccount','OwnedStdVector','OwnedPmrAllocator']):cat='resource admission/owned containers'
   elif 'std::_Function_handler' in f:cat='callback dispatch/consumer'
   elif any(n in f for n in ['MetadataPageReadContext','MetadataPageFile','MetadataPageRouteDirectory','CatalogTree','LocationDirectory','AllocatorDirectory','CatalogRadix']):cat='metadata lookup/cache/validation'
   elif any(n in f for n in ['LittleEndianCodec','Codec::decode','SlottedPageReadView','validateOffsetDirectory','validateRecord']):cat='decode/record validation'
   elif any(n in f for n in ['makeLogicalGraphMutationFence','makeLogicalGraphSchemaIdentity','validateSnapshot','validateBorrowVersion','QueryGuard','clock_gettime']):cat='binding/snapshot/guard checks'
   elif any(n in f for n in ['BufferPool','StorageIO','std::basic_filebuf','std::basic_streambuf','fstat','pread']):cat='storage I/O/cache machinery'
   elif any(n in f for n in ['NodeEntityRecord','NativeReadSession::put']):cat='entity cache/record objects'
   elif any(n in f for n in ['malloc','operator new','allocate','deallocate','_Hash_bytes']):cat='allocation/hash machinery'
   elif any(n in f for n in ['DirectPropertyColumns','LogicalPlanRead','tryVisitBatchedNativeNodeRefScan','BoundExpression','Aggregation']):cat='scan/filter/group computation'
   funcs[(cat,f)]+=v;cats[cat]+=v
 total=sum(cats.values())
 cpu={}
 for label in ['sampled','control','wall-control']:
  line=(root/(case+'-final-'+('sampled' if label=='sampled' else 'control')+'.pcs')).read_text().splitlines()[0]
  cpu[label]={k:float(v) for k,v in re.findall(r'(user_cpu_seconds|system_cpu_seconds|wall_seconds|samples) ([0-9.]+)',line)}
 wall=cpu['wall-control'];wall['off_cpu_estimate_seconds']=max(0,wall['wall_seconds']-wall['user_cpu_seconds']-wall['system_cpu_seconds'])
 result={'case':case,'raw_samples':len(lines)-1,'weighted_ticks':total,'nominal_period_ms':1,'cpu_controls':cpu,'categories':[{'category':c,'ticks':v,'percent':100*v/total} for c,v in cats.most_common()],'functions':[{'category':c,'name':f,'ticks':v,'percent':100*v/total} for (c,f),v in funcs.most_common()]}
 all_results.append(result)
(root/'exclusive-cpu-attribution.json').write_text(json.dumps({'diagnostic_only':True,'source_base':'P7 rejected candidate','engine_archive_sha256':hashlib.file_digest(Path('/workspace/zyx-build-core-candidate/libzyx_core.a').open('rb'),'sha256').hexdigest(),'observer_period_ms':1,'weighted_ticks_include_signal_overruns':True,'libc_ifunc_starts':starts,'libc_ifunc_verified_region_widths':widths,'limitations':['raw_samples reports independent interrupted-PC counts; weighted ticks are not independent samples','leaf-PC buckets are exclusive, callback/consumer frames are opaque without full stacks','libc internal symbols remain unclassified; no attribution to a fabricated nearest exported name','off-CPU estimate uses aggregate process CPU with one query worker; tiny observer/background overhead remains','profiling CPU deltas are bounded diagnostic controls, not formal performance results','gprofng attempts had timer-loss warnings and are rejected'], 'cases':all_results},indent=2)+'\n')
for r in all_results:
 print(r['case'],[(c['category'],round(c['percent'],1)) for c in r['categories']]);print(r['cpu_controls']['wall-control'])
