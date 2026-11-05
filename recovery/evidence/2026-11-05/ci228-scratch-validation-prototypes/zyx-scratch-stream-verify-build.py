from pathlib import Path
import subprocess,os,json,shlex,concurrent.futures
root=Path('/workspace/zyx-ci-fix');build=Path('/workspace/zyx-ci-config');base=Path('/workspace/zyx-ci228-full-asan-core');out=Path('/tmp/zyx-scratch-stream-verify');out.mkdir(exist_ok=True)
def original(rel):return subprocess.check_output(['git','show','ff02643c:'+rel],cwd=root,text=True)
h='include/graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp'
p=out/h;p.parent.mkdir(parents=True,exist_ok=True);s=original(h);needle='[[nodiscard]] PhysicalSegmentContents read(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const;';assert needle in s;s=s.replace(needle,needle+'\n        void validateContents(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const;');p.write_text(s)
rel='src/storage/relocatable/allocation/PhysicalSegmentAllocator.cpp';s=original(rel);a=s.index('\tPhysicalSegmentContents PhysicalSegmentAllocator::read(');b=s.index('\n\tvoid PhysicalSegmentAllocator::validateRequests',a);old=s[a:b]
prefix=old[:old.index('\n\t\tstd::vector<std::byte> bytes(RSS_SEGMENT_SIZE);')].replace('PhysicalSegmentContents PhysicalSegmentAllocator::read(','void PhysicalSegmentAllocator::validateContents(')
identity=old[old.index('\n\t\tconst uint8_t invalid'):old.index('\n\n\t\tconst uint32_t headerExtensionBytes')]
method=prefix+'''
        std::array<std::byte, RSS_PAGE_SIZE> bytes{};
        SegmentEnvelope envelope;
        uint32_t crc = 0U;
        for (size_t offset = 0U; offset < RSS_SEGMENT_SIZE; offset += bytes.size()) {
            if (io_->readAtSequential(segmentOffset + offset, bytes.data(), bytes.size()) != bytes.size())
                throwPhysicalAllocatorError(FormatError::FERR_BUFFER_TOO_SMALL,
                    "relocatable physical segment is truncated");
            if (offset == 0U) {
                envelope = SegmentEnvelopeCodec::decode(std::span<const std::byte>(bytes.data(), RSS_ENVELOPE_BYTES));
'''+identity+'''
            }
            const size_t begin = (std::max)(offset, static_cast<size_t>(envelope.header_bytes));
            const size_t end = (std::min)(offset + bytes.size(),
                static_cast<size_t>(envelope.header_bytes) + envelope.payload_bytes);
            if (begin < end) crc = utils::updateCrc(crc, bytes.data() + begin - offset, end - begin);
        }
        if (crc != envelope.payload_crc)
            throwPhysicalAllocatorError(FormatError::FERR_CHECKSUM_MISMATCH,
                "relocatable physical segment payload checksum does not match");
    }
'''
if os.environ.get('ZYX_SCRATCH_READ_REUSE') == '1':
 s=s.replace('#include <algorithm>','#include <algorithm>\n#include <cstring>')
 prototype=Path('/tmp/zyx-PhysicalSegmentAllocator-scratch-reuse.cpp').read_text()
 reuse=prototype[prototype.index('        // Prototype: keep'):prototype.index('\t\tstd::vector<std::byte> payload(bytes.begin() + envelope.header_bytes,')]
 marker='\t\tstd::vector<std::byte> payload(bytes.begin() + envelope.header_bytes,'
 assert s.count(marker)==1;s=s.replace(marker,reuse+marker)
s=s.replace('#include <algorithm>','#include <algorithm>\n#include <array>');s=s[:s.index('\n\tvoid PhysicalSegmentAllocator::validateRequests')]+ '\n'+method+s[s.index('\n\tvoid PhysicalSegmentAllocator::validateRequests'):];(out/Path(rel).name).write_text(s)
rel2='src/storage/relocatable/index/NativeBulkRun.cpp';s=original(rel2);needle='static_cast<void>(physical_.read(snapshot, location.segment_id));';assert s.count(needle)==1;s=s.replace(needle,'physical_.validateContents(snapshot, location.segment_id);');(out/Path(rel2).name).write_text(s)
manifest=json.loads((base/'compile-manifest.json').read_text())['units'];env={**os.environ,'LIBRARY_PATH':'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'}
def compile_one(rel):
 item=next(x for x in manifest if x['source']==str(root/rel));cmd=item['command'].copy();obj=out/(Path(rel).name+'.o');cmd[cmd.index('-o')+1]=str(obj);cmd[cmd.index('-c')+1]=str(out/Path(rel).name);cmd[1:1]=['-I'+str(out/'include')]
 with obj.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT)
 print(Path(rel).name,r.returncode,flush=True);return r.returncode,str(obj)
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:results=list(pool.map(compile_one,[rel,rel2]))
if any(x[0] for x in results):raise SystemExit(1)
s=Path('/tmp/zyx-ci228-full-asan-tests-relink.py').read_text().replace("args[1:1]=[*[x[1] for x in results],'/tmp/zyx-tree-focused/gtest_main.o']","args[1:1]=[*[x[1] for x in results],'/tmp/zyx-scratch-stream-verify/PhysicalSegmentAllocator.cpp.o','/tmp/zyx-scratch-stream-verify/NativeBulkRun.cpp.o','/tmp/zyx-tree-focused/gtest_main.o']").replace("out/'native_tests'","out/'scratch-stream-verify'");exec(compile(s,'<prototype-link>','exec'))
