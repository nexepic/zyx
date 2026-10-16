from pathlib import Path
import shlex,subprocess
root=Path('/workspace/zyx');s=(root/'apps/compare_benchmark/main.cpp').read_text();s=s.replace('#include <algorithm>','#include <algorithm>\n#include <cstdlib>',1)
needle='''            validateColumns(columns, count);
            std::vector<int64_t> result; result.reserve(count);'''
replacement='''            validateColumns(columns, count);
            if (std::getenv("ZYX_DIAGNOSTIC_RESUME")) {
                std::unordered_map<std::string,int64_t> ids;
                for (size_t skip = 0U; skip < count; skip += 1000U) {
                    auto observed = database_.execute("MATCH (n:" + label + ") RETURN id(n) AS native,n.id AS external SKIP " + std::to_string(skip) + " LIMIT 1000");
                    if (!observed.isSuccess()) throw std::runtime_error(observed.getError());
                    while (observed.hasNext()) { observed.next(); ids.emplace(std::get<std::string>(observed.get("external")), std::get<int64_t>(observed.get("native"))); }
                }
                if (ids.size() != count) throw std::runtime_error("resume requires complete node inputs");
                std::vector<int64_t> existing; existing.reserve(count);
                for (const auto &value : columns.front().values) existing.push_back(ids.at(std::get<std::string>(value)));
                std::cerr << "resumed nodes " << label << " " << count << std::endl;
                return existing;
            }
            std::vector<int64_t> result; result.reserve(count);'''
assert needle in s;s=s.replace(needle,replacement,1)
needle='''            for (size_t offset = 0U; offset < sources.size();) {'''
replacement='''            size_t committed = 0U;
            if (std::getenv("ZYX_DIAGNOSTIC_RESUME")) {
                auto observed = database_.execute("MATCH ()-[r:" + type + "]->() RETURN count(*) AS count");
                if (!observed.isSuccess()) throw std::runtime_error("cannot count committed edges: " + observed.getError()); if (!observed.hasNext()) throw std::runtime_error("missing committed edge count row");
                observed.next(); committed = static_cast<size_t>(std::get<int64_t>(observed.get("count")));
                if (committed > sources.size() || (committed != sources.size() && committed % 1000U)) throw std::runtime_error("unaligned committed edge prefix");
                result.resize(committed);
                std::cerr << "resumed edges " << type << " " << committed << "/" << sources.size() << std::endl;
            }
            for (size_t offset = committed; offset < sources.size();) {'''
assert needle in s;s=s.replace(needle,replacement,1)
s=s.replace('} else tracePhase("load.remove_existing_db", [&]() { removeDatabaseArtifacts(dbPath); });','} else if (!std::getenv("ZYX_DIAGNOSTIC_RESUME")) tracePhase("load.remove_existing_db", [&]() { removeDatabaseArtifacts(dbPath); });',1)
source=Path('/tmp/zyx-fixture-resume-5000.cpp');source.write_text(s);obj=Path('/tmp/zyx-fixture-resume-5000.o');binary=Path('/tmp/zyx-fixture-resume-5000')
base=shlex.split(Path('/tmp/zyx-bounded-compare-compile-command.txt').read_text().strip().splitlines()[-1]);cmd=[];i=0
while i<len(base):
 if base[i] in ['-o','-MF','-MT','-c']:i+=2;continue
 if base[i]=='-MD':i+=1;continue
 cmd.append(base[i]);i+=1
cmd+=['-c',str(source),'-o',str(obj)];subprocess.run(cmd,cwd=root/'buildDir_perf_control',check=True)
lines=(root/'buildDir_perf_control/build.ninja').read_text().splitlines();start=next(i for i,v in enumerate(lines) if v.startswith('build zyx-compare-bench:'));libs=next(v.split(' = ',1)[1] for v in lines[start:start+15] if v.strip().startswith('LINK_LIBRARIES = '));subprocess.run([cmd[0],'-O3','-DNDEBUG',str(obj),*shlex.split(libs),'-o',str(binary)],cwd=root/'buildDir_perf_control',check=True)
