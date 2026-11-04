import hashlib
import json
from pathlib import Path
import subprocess

root = Path(r'D:\Projects\OceanBase\seekdb')
here = Path(__file__).resolve().parent
stage = here / 'round11-proposal'
manifest = json.loads((stage / 'manifest.json').read_text(encoding='utf-8'))
for item in manifest['source_files']:
    assert hashlib.sha256((root / item['path']).read_bytes()).hexdigest() == item['original_content_sha256'], item['path']
    assert hashlib.sha256((stage / item['path']).read_bytes()).hexdigest() == item['new_sha256'], item['path']
for item in manifest['source_files']:
    (root / item['path']).write_bytes((stage / item['path']).read_bytes())
# Diagnostic logs enable a separate DEBUG-only check of the actual runtime path.
adapter = root / 'src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp'
content = adapter.read_text(encoding='utf-8')
marker = '  if (OB_NOT_NULL(ir_ctdef_->field_boost_expr_)) {'
assert content.count(marker) == 1
content = content.replace(marker, '''  LOG_DEBUG("text retrieval result materialization", K_(sr_iter_param));
''' + marker)
adapter.write_text(content, encoding='utf-8', newline='\r\n')
patch = subprocess.check_output(['git', 'diff', '--binary', '--', 'src'], cwd=root)
(here / 'round11-source.patch').write_bytes(patch)
(here / 'round11-source.json').write_text(json.dumps({
    'parent': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
    'patch': 'round11-source.patch', 'sha256': hashlib.sha256(patch).hexdigest(),
    'changes': 'Prune external materialization for value-free root natural-language IR consumers; existing serialized output array, no new serialized flag.',
    'validation': 'Pending full build, official-original fixture comparison, runtime eligibility and full benchmark.',
}, indent=2) + '\n', encoding='utf-8')
print('APPLIED round11', hashlib.sha256(patch).hexdigest())
