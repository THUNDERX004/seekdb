#!/usr/bin/env python3
"""Archive round11 evidence byte-for-byte. Stdlib only; never executes SQL/build/Git.

Default output is local evidence/. The parent can copy the reviewed package to
the repository asset directory. Existing output is never overwritten.
--verify DIR verifies a completed package without writing.
"""
import argparse
import difflib
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
OFFICIAL = '5bce9e06d1281347ad61344e4b01fab9158c58a2'
PARENT = 'ecbf498d120dabaa01b60b89c49aa18a6b9926a1'
PATCH_SHA = '40ea9aa36f6efd0559d4efad439ece943822e32c19ef2dbd90a6337fb904deaf'
INITIAL_PATCH_SHA = '4158f606656c5c22d9184225711eb33d84ee1d0464d449f0affee7c4560732f0'
BINARY_SHA = '9c239e363020a77c201c31456c940084f948d5b5741ec73472af55632c9b91da'
ORIGINAL_SHA = 'bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762'
CONTROL_SHA = '4b5da29dccff5458257668e17e6209676e751363213b649faebbbdbaf35e2a82'
INITIAL_BINARY_SHA = '64bbff2c706fc2a85b88c73f8170b217f60d1669f5f3fd2dadb0c51b8cc53ecd'
FIXTURE_SHA = '8bb2ccc372e011a1fc290221d601857ca5093d4cdd5fc1449eae20b73b8a2c93'
TEST_SHA = 'fcbf477f2856d24b386fe12ac8a5e2e1c796289d21ae2e44cf3d835cbf5a143a'
WORKLOAD_SHA = 'f6b33052f9971b04bd2be834859df038a2378e5e2f8df23e005b58526047e6ea'
PARAMETERS = dict(rows=20000, batch=500, rounds=3000, query_rounds=200,
                  samples=3, warmup=30, skip_load=0)
CATEGORIES = {
    'build': ['build_ik_all_sec', 'build_ik_content_sec', 'build_beng_en_sec'],
    'tokenize': ['tokenize_ik_avg_ms', 'tokenize_beng_avg_ms'],
    'query': ['query_cn_avg_ms', 'query_beng_avg_ms', 'query_mixed_avg_ms', 'query_limit_avg_ms'],
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(path):
    return json.loads(path.read_bytes())


def encode(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf-8')


def file_sha(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def close(a, b, name):
    require(math.isclose(a, b, rel_tol=1e-11, abs_tol=1e-12), 'Metric mismatch: ' + name)


def report_values(path):
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        key, separator, value = line.partition(':')
        if separator:
            values[key.strip()] = value.strip()
    for key, expected in PARAMETERS.items():
        require(int(values[key]) == expected, f'{path.name}: {key}')
    for key, expected in dict(query_cn_hits=8001, query_beng_hits=11000,
                              query_mixed_hits=7332, query_limit_hits=20).items():
        require(int(values[key]) == expected, f'{path.name}: {key}')
    return {key: float(values[key]) for keys in CATEGORIES.values() for key in keys}


def verify(directory):
    manifest = load(directory / 'manifest.json')
    expected = set(manifest['files']) | {'manifest.json', 'SHA256SUMS'}
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    require(actual == expected, 'Missing or unlisted archive files')
    for name, expected_sha in manifest['files'].items():
        path = directory / name
        require(not path.is_symlink() and path.resolve().is_relative_to(directory.resolve()),
                'Unsafe archive path: ' + name)
        require(file_sha(path) == expected_sha, 'SHA256 mismatch: ' + name)
    sums = ''.join(f'{file_sha(directory / name)}  {name}\n'
                   for name in sorted(expected - {'SHA256SUMS'})).encode('utf-8')
    require((directory / 'SHA256SUMS').read_bytes() == sums, 'SHA256SUMS mismatch')
    return len(manifest['files'])


def make_patch(source_root, relative, fragment, append=False):
    path = source_root / relative
    raw = path.read_bytes()
    old = raw.decode('utf-8').replace('\r\n', '\n')
    section = fragment.read_text(encoding='utf-8').strip()
    require(section.splitlines()[0] not in old, 'Section already present: ' + relative)
    if append:
        new = old.rstrip('\n') + '\n\n' + section + '\n'
    else:
        title, rest = old.split('\n', 1)
        new = title + '\n\n' + section + '\n\n' + rest.lstrip('\n')
    diff = ''.join(difflib.unified_diff(old.splitlines(keepends=True),
                   new.splitlines(keepends=True), fromfile='a/' + relative,
                   tofile='b/' + relative))
    return ('diff --git a/' + relative + ' b/' + relative + '\n' + diff).encode('utf-8'), {
        'path': relative, 'base_raw_sha256': sha(raw),
        'base_lf_sha256': sha(old.encode('utf-8')),
        'result_lf_sha256': sha(new.encode('utf-8')),
    }


def package(args):
    inputs = args.input_dir.resolve()
    root = args.source_root.resolve()
    out = args.output.resolve()
    require(not out.exists(), 'Output already exists; choose a new directory: ' + str(out))
    require(not out.is_relative_to(root),
            'Default safety boundary: output must be outside the D: checkout. Parent can copy reviewed evidence later.')
    raw_files = sorted(p for p in inputs.glob('round11-*') if p.is_file())
    require(raw_files, 'No round11 evidence found')
    proposal = inputs / 'round11-proposal'
    require(proposal.is_dir(), 'Missing complete proposal directory')
    sources = [(p, p.name) for p in raw_files]
    sources += [(p, 'proposal/' + p.relative_to(proposal).as_posix())
                for p in sorted(proposal.rglob('*')) if p.is_file()]
    sources += [(inputs / name, 'helpers/' + name) for name in
                ('apply-round11.py', 'fix-query-fixture-client.py', 'diagnose-materialization.py')]
    fixture_rel = 'unittest/storage/fts/query_materialization_expected.json.gz'
    test_rel = 'unittest/storage/fts/test_query_materialization.py'
    sources += [(root / fixture_rel, 'regression/' + fixture_rel),
                (root / test_rel, 'regression/' + test_rel)]
    sources += [(HERE / name, 'packaging/' + name) for name in
                ('package-round11.py', 'README.md', 'report-en.md', 'report-zh-CN.md',
                 'test-readme-addition.md', 'parent-attestation.json')]
    names = [name for _, name in sources]
    require(len(names) == len(set(names)), 'Duplicate archive path')
    for path, name in sources:
        require(path.is_file() and not path.is_symlink(), 'Missing/symlink input: ' + str(path))
        require(not any(int(n) > 11 for n in re.findall(r'round[-_ ]?(\d+)', name, re.I)),
                'Later-round input refused: ' + name)
    for name, expected in [('round11-trace-source.patch', PATCH_SHA),
                           ('round11-source.patch', INITIAL_PATCH_SHA)]:
        require(file_sha(inputs / name) == expected, 'Wrong production/historical patch: ' + name)
    patch_info = load(inputs / 'round11-trace-source.json')
    require(patch_info['parent'] == PARENT and patch_info['sha256'] == PATCH_SHA, 'Trace source identity')
    require(file_sha(root / fixture_rel) == FIXTURE_SHA, 'Independent fixture changed')
    require(file_sha(root / test_rel) == TEST_SHA, 'Final regression driver changed')
    fixture = load_gzip(root / fixture_rel)
    require(fixture['original'] == dict(commit=OFFICIAL, binary_sha256=ORIGINAL_SHA), 'Original provenance')
    require(len(fixture['results']) == 4416 and fixture['workload_sha256'] == WORKLOAD_SHA, 'Fixture coverage')
    experiments = [load(p) for p in raw_files if p.name.endswith('-experiment.json')]
    by_label = {e['label']: e for e in experiments}
    comparison = load(inputs / 'round11-three-pair-comparison.json')
    for index in (1, 2, 3):
        for label, expected_sha in [(f'round11-value-free-candidate-{index}', BINARY_SHA),
                                    (f'round11-materializing-control-{index}', CONTROL_SHA)]:
            experiment = by_label[label]
            require(experiment['binary_sha256'] == expected_sha, 'Executable identity: ' + label)
            require(all(action['exit_code'] == 0 for action in experiment['actions']), 'Incomplete scored run: ' + label)
        candidate = by_label[f'round11-value-free-candidate-{index}']
        require(candidate['source_commit'] == PARENT + '+' + PATCH_SHA, 'Candidate source provenance')
        require(candidate['log_level'] == 'WARN', 'Performance log level')
    for comparison_name in ('round11-first-pair-comparison.json',
                            'round11-incremental-comparison.json',
                            'round11-three-pair-comparison.json'):
        data = load(inputs / comparison_name)
        for field, mean_field in [('controls', 'control_mean'), ('candidates', 'candidate_mean')]:
            values = [report_values(inputs / (label + '-report.txt')) for label in data[field]]
            for key in data[mean_field]:
                close(sum(v[key] for v in values) / len(values), data[mean_field][key], comparison_name + '/' + key)
        for key in data['control_mean']:
            close(1 - data['candidate_mean'][key] / data['control_mean'][key], data['per_metric_reduction'][key], key)
        for category, keys in CATEGORIES.items():
            close(sum(data['per_metric_reduction'][k] for k in keys) / len(keys),
                  data['per_category_reduction'][category], category)
        close(sum(data['per_category_reduction'].values()) / 3, data['overall_reduction'], 'overall')
    close(comparison['overall_reduction'], 0.0025310035648135453, 'Pinned overall')
    for index, status in [(1, 1), (2, 1), (3, 0)]:
        require(by_label[f'round11-original-query-record-{index}']['actions'][0]['exit_code'] == status,
                'Missing fixture recording failure/success evidence')
    actions = {a['action']: a['exit_code'] for a in by_label['round11-value-free-candidate-1']['actions']}
    for name in ('query-materialization', 'casecopy', 'bmp', 'ranked', 'keytypes',
                 'test_hot_path.py', 'test_tokenize_config.py', 'test_collation.py',
                 'test_match_batches.py', 'test_map_concurrency.py',
                 'test_stopword_concurrency.py', 'test_dictionary_prefixes.py'):
        require(actions.get(name) == 0, 'Missing passing regression: ' + name)
    runtime = (inputs / 'round11-runtime-materialization.log').read_text(encoding='utf-8')
    require(runtime.count('materialize_results:false') == 3 and runtime.count('materialize_results:true') == 2,
            'Runtime diagnostic must retain three false/two true observations')
    binaries = []
    for relative, expected, role in [
        ('build/fts-round11-trace-value-free/seekdb.exe', BINARY_SHA, 'final LOG_TRACE candidate'),
        ('build/fts-round11-value-free/seekdb.exe', INITIAL_BINARY_SHA, 'historical LOG_DEBUG trial')]:
        path = root / relative
        item = dict(path=str(path), expected_sha256=expected, role=role,
                    experiment_sha256=expected, observed_sha256=None)
        if path.exists():
            item.update(observed_sha256=file_sha(path), bytes=path.stat().st_size)
            require(item['observed_sha256'] == expected, 'Saved executable changed: ' + relative)
            item['diagnostic_literal_present'] = contains_literal(path, b'text retrieval result materialization')
        binaries.append(item)
    patch, bases = b'', []
    for language in ('en', 'zh-CN'):
        part, base = make_patch(root, f'docs/developer-guide/{language}/fts-performance.md', HERE / f'report-{language}.md')
        patch += part
        bases.append(base)
    readme_patch, readme_base = make_patch(root, 'unittest/storage/fts/README.md',
                                         HERE / 'test-readme-addition.md', append=True)
    # Snapshot source digests before the first write, then detect concurrent edits.
    source_hashes = {name: file_sha(path) for path, name in sources}
    out.mkdir(parents=True, exist_ok=False)
    inventory = []
    for path, name in sources:
        raw = path.read_bytes()
        require(sha(raw) == source_hashes[name], 'Concurrent source edit: ' + str(path))
        destination = out / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        inventory.append(dict(path=name, source=str(path), bytes=len(raw), sha256=sha(raw)))
    generated = {
        'round11-trace-source.patch.gz': gzip.compress((out / 'round11-trace-source.patch').read_bytes(), mtime=0),
        'reports.patch': patch, 'test-readme.patch': readme_patch,
        'report-bases.json': encode(dict(reports=bases, test_readme=readme_base)),
        'binary-provenance.json': encode(binaries), 'inventory.json': encode(inventory),
    }
    for name, raw in generated.items():
        (out / name).write_bytes(raw)
    failures = [dict(label=e['label'], action=a['action'], exit_code=a['exit_code'])
                for e in experiments for a in e['actions'] if a['exit_code'] != 0]
    manifest = dict(schema_version=1, official_commit=OFFICIAL, parent_commit=PARENT,
        candidate_source_patch='round11-trace-source.patch', source_patch_sha256=PATCH_SHA,
        candidate_binary='build/fts-round11-trace-value-free/seekdb.exe', candidate_binary_sha256=BINARY_SHA,
        independent_fixture=fixture_rel, fixture_sha256=FIXTURE_SHA, fixture_cases=4416,
        comparison='round11-three-pair-comparison.json',
        per_metric_reduction=comparison['per_metric_reduction'],
        per_category_reduction=comparison['per_category_reduction'],
        overall_reduction=comparison['overall_reduction'], formal_score=None, scored_parameters=PARAMETERS,
        decision='Small correctness-validated checkpoint only; stable full marks and stable incremental speedup are not established.',
        limitations=['Local Windows evidence; no new formal platform score or Linux CI validation.',
            'Incremental parent comparison, not a new official-original cumulative measurement.',
            'All six scored runs, all negative metrics and both failed fixture recordings retained.',
            'LOG_DEBUG trial lacked the compiled diagnostic literal; its successful SQL output is not path proof.',
            'Final LOG_TRACE runtime excerpt records three false and two true materialization flags.',
            'Raw proposal metadata describes its earlier offline stage and is preserved unchanged.',
            'Helpers are unexecuted packaging-time snapshots, not a claim of historical script identity.',
            'No SELECT1 subtraction, negative clipping or fastest-sample selection.',
            'No build, SQL, benchmark, source write or Git operation performed by this script.'],
        failed_actions=failures, raw_file_count=len(raw_files), proposal_file_count=sum(n.startswith('proposal/') for n in names),
        integrity='files hashes every payload; SHA256SUMS additionally hashes manifest.json. Neither file hashes itself.',
        files={p.relative_to(out).as_posix(): file_sha(p) for p in sorted(out.rglob('*')) if p.is_file()})
    (out / 'manifest.json').write_bytes(encode(manifest))
    sum_names = sorted(p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file())
    sums = ''.join(f'{file_sha(out / name)}  {name}\n' for name in sum_names)
    (out / 'SHA256SUMS').write_bytes(sums.encode('utf-8'))
    count = verify(out)
    print(f'Packaged and verified {count} payload files: {out}')
    print(f'Raw round11 files: {len(raw_files)}; failed actions retained: {len(failures)}')
    print('Production patch SHA256: ' + PATCH_SHA)
    print('Candidate binary SHA256: ' + BINARY_SHA)


def load_gzip(path):
    return json.loads(gzip.decompress(path.read_bytes()))


def contains_literal(path, literal):
    tail = b''
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            data = tail + chunk
            if literal in data:
                return True
            tail = data[-len(literal):]
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(r'D:\Projects\OceanBase\seekdb'))
    parser.add_argument('--input-dir', type=Path, default=HERE.parent)
    parser.add_argument('--output', type=Path, default=HERE / 'evidence')
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if args.verify:
        print(f'Verified {verify(args.verify.resolve())} payload files: {args.verify}')
    else:
        package(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('Packaging/verification failed: ' + str(error), file=sys.stderr)
        print('Any partial output is retained for inspection; use a fresh output path.', file=sys.stderr)
        sys.exit(1)
