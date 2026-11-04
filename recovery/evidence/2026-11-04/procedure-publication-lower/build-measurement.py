from pathlib import Path
import shlex,subprocess,os
src=Path('/workspace/zyx-ci-fix'); build=Path('/workspace/zyx-ci-config'); root=Path('/tmp/zyx-procedure-lower'); root.mkdir(exist_ok=True)
rel='tests/src/query/api/test_LogicalVectorProcedureBatch_Materialization.cpp'; test=src/rel
s=test.read_text(); s='#include "/tmp/zyx-allocation-tracker-live.inc"\n'+s
start=s.index('TEST_F(LogicalVectorPropertyBatchTest, RetainsConvertedQueryVectorThroughResultPublication)'); end=s.index('TEST_F(',start+8)
part=s[start:end].replace('static_cast<void>(execute(project));','sample_alloc::start(); try { auto result=execute(project); sample_alloc::stop(); } catch (...) { sample_alloc::stop(); throw; }',1)
s=s[:start]+part+s[end:]
start=s.index('TEST_F(LogicalVectorPropertyBatchTest, MetadataJointRetentionRejectsLowBudgetAndCanRetry)');end=s.index('TEST_F(',start+8)
part=s[start:end].replace('const auto result = execute(project);','sample_alloc::start(); const auto result = [&] { try { auto r=execute(project); sample_alloc::stop(); return r; } catch (...) { sample_alloc::stop(); throw; } }();')
s=s[:start]+part+s[end:]; s=s.replace('ASSERT_EQ(result.rowCount(), 3U);\n\t\t\tconst std::array<uint64_t, 3U> order', 'size_t resultLower = result.getRows().capacity() * sizeof(QueryResult::Row) + result.getColumns().capacity() * sizeof(std::string);\n            for (const auto &resultRow : result.getRows()) {\n                resultLower += resultRow.size() * sizeof(QueryResult::Row::value_type);\n                for (const auto &[name, value] : resultRow) {\n                    const auto &list = value.asPrimitive().getList();\n                    resultLower += list.capacity() * sizeof(PropertyValue);\n                    for (const auto &item : list) {\n                        const auto &text = std::get<std::string>(item.getVariant());\n                        if (text.size() > sizeof(std::string)) resultLower += text.capacity() + 1U;\n                    }\n                }\n            }\n            std::fprintf(stderr, "RESULT_HEAP_LOWER=%zu property_value=%zu row_pair=%zu row_capacity=%zu\\n", resultLower, sizeof(PropertyValue), sizeof(QueryResult::Row::value_type), result.getRows().capacity());\n            ASSERT_EQ(result.rowCount(), 3U);\n            const std::array<uint64_t, 3U> order')
(root/'test.cpp').write_text(s)
commands=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','CMakeFiles/zyx_test_suite.dir/'+rel+'.o'],cwd=build,text=True).splitlines();args=shlex.split(next(x for x in commands if '-c '+str(test) in x));new=[];i=0
while i<len(args):
 a=args[i]
 if a in ['-MF','-MT']:i+=2;continue
 if a in ['-MD','-MMD']:i+=1;continue
 if a=='-o':new+=['-o',str(root/'test.o')];i+=2;continue
 new.append(str(root/'test.cpp') if a==str(test) else a);i+=1
env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu';env['LD_LIBRARY_PATH']=env['LIBRARY_PATH']
with (root/'build.log').open('w') as f:subprocess.run(new,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
link=shlex.split(Path('/tmp/zyx-cell-baseline/link-command.txt').read_text());link=[a for a in link if a!='/tmp/zyx-cell-baseline/runtime-before.o'];link[link.index('-o')+1]=str(root/'suite');link=[str(root/'test.o') if a.endswith(rel+'.o') else a for a in link]
link=[x for x in link if not x.endswith('.o') and not x.startswith('--dependency-file=') and x!='-Xlinker']
link[link.index('libzyx_core.a'):link.index('libzyx_core.a')]=[str(root/'test.o'),'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a']
with (root/'link.log').open('w') as f:subprocess.run(link,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
with (root/'test.log').open('w') as f:r=subprocess.run([str(root/'suite'),'--gtest_filter=LogicalVectorPropertyBatchTest.MetadataJointRetentionRejectsLowBudgetAndCanRetry'],env=env,stdout=f,stderr=subprocess.STDOUT)
print(r.returncode)
