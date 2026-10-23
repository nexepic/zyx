from pathlib import Path
for kind in ['hybrid','default']:
 p=Path('/tmp/build-direct-locators-'+kind+'-modules.py');s=p.read_text();start=s.index('with (dest/');s=s[:start]+'''prior = json.loads((dest/'compile.json').read_text()) if (dest/'compile.json').exists() else {}
prior_commands = {c[-1]: c for c in prior.get('commands', [])}
def needs_compile(obj, command):
 if not obj.exists() or prior_commands.get(command[-1]) != command: return True
 dep=Path(str(obj)+'.d')
 if not dep.exists(): return True
 content=dep.read_text().replace('\\\\\\n',' ')
 if ':' not in content: return True
 dependencies=shlex.split(content.split(':',1)[1]);stamp=obj.stat().st_mtime
 for name in dependencies:
  dependency=Path(name)
  if not dependency.is_absolute(): dependency=repo/'buildDir'/dependency
  if not dependency.exists() or dependency.stat().st_mtime>stamp: return True
 return False
'''+s[start:]
 a=s.index('  headers_latest=');b=s.index('   subprocess.run',a)
 s=s[:a]+'''  if needs_compile(obj,cmd):
'''+s[b:];p.write_text(s)
