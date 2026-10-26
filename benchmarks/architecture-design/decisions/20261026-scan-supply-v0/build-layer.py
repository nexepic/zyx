from pathlib import Path
import json,subprocess
D=Path(__file__).resolve().parent
recipe=json.loads((D/'layer-recipes.json').read_text())
with (D/'layer-build.log').open('w') as log:
 for key in ('compile','link'):
  subprocess.run(recipe[key],stdout=log,stderr=subprocess.STDOUT,check=True)
