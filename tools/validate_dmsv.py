#!/usr/bin/env python3
"""
Validate EVERY SPIR-V shader inside a DMSV (v1/v2 zstd) shader bundle using spirv-val.
Reports exact pass/fail counts, stage breakdown, and writes a JSON/TSV audit log.
"""
import argparse, collections, concurrent.futures as cf, json, os, struct, subprocess, sys, tempfile
from pathlib import Path

STAGES = {0: 'vertex', 1: 'pixel', 2: 'geometry', 3: 'hull', 4: 'domain', 5: 'compute'}

def read_dmsv(path: str):
    data = Path(path).read_bytes()
    assert data[:4] == b'DMSV', f'Not a DMSV bundle: {path}'
    ver, count = struct.unpack_from('<II', data, 4)
    assert ver in (1, 2), f'Unsupported DMSV version {ver}'
    entries = []
    # dmsv.py entry: u64 key, u32 stage, u32 spv_off, u32 spv_len, u32 refl_off, u32 refl_len, u32 spv_size (32 bytes)
    off = 12
    for i in range(count):
        key, stage, spv_off, spv_len, refl_off, refl_len, spv_size = struct.unpack_from('<QIIIIII', data, off)
        off += 32
        entries.append((key, stage, spv_off, spv_len, spv_size, refl_off, refl_len))
    return data, ver, entries

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bundle')
    ap.add_argument('--spirv-val', required=True)
    ap.add_argument('--target-env', default='vulkan1.2')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    ap.add_argument('--out-json', required=True)
    a = ap.parse_args()

    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime_src/nxnative/dm/android/tools'))
    import dmsv as _dmsv
    zmod = _dmsv.zstd_mod()

    data, ver, entries = read_dmsv(a.bundle)
    print(f'[validate_dmsv] bundle={a.bundle} ver={ver} entries={len(entries)} target={a.target_env}', flush=True)

    def validate_one(item):
        idx, key, stage, spv_off, spv_len, spv_size = item
        chunk = data[spv_off:spv_off + spv_len]
        if spv_size:
            spv = zmod.decompress(chunk)
        else:
            spv = chunk
        if len(spv) < 20 or struct.unpack_from('<I', spv, 0)[0] != 0x07230203:
            return (key, stage, False, 'invalid SPIR-V magic')
        p = subprocess.run(
            [a.spirv_val, '--target-env', a.target_env, '-'],
            input=spv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        if p.returncode != 0:
            err = (p.stderr or p.stdout).decode('utf-8', 'replace').strip().splitlines()
            return (key, stage, False, err[0] if err else f'rc={p.returncode}')
        return (key, stage, True, '')

    work = [(i, e[0], e[1], e[2], e[3], e[4]) for i, e in enumerate(entries)]
    stage_ok = collections.Counter()
    stage_fail = collections.Counter()
    failures = []
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for key, stage, ok, msg in ex.map(validate_one, work):
            sname = STAGES.get(stage, str(stage))
            if ok:
                stage_ok[sname] += 1
            else:
                stage_fail[sname] += 1
                failures.append({'key': f'{key:016x}', 'stage': sname, 'error': msg})

    passed = sum(stage_ok.values())
    failed = sum(stage_fail.values())
    rep = {
        'bundle': a.bundle,
        'dmsv_version': ver,
        'target_env': a.target_env,
        'total': len(entries),
        'passed': passed,
        'failed': failed,
        'by_stage_passed': dict(stage_ok),
        'by_stage_failed': dict(stage_fail),
        'failures_sample': failures[:50],
    }
    Path(a.out_json).write_text(json.dumps(rep, indent=2), encoding='utf-8')
    print(f'[validate_dmsv] total={len(entries)} passed={passed} failed={failed}', flush=True)
    for st in sorted(set(stage_ok) | set(stage_fail)):
        print(f'  {st:10s}: passed={stage_ok[st]} failed={stage_fail[st]}', flush=True)
    if failures:
        print('Sample failures:', flush=True)
        for f in failures[:10]:
            print(f"  {f['key']} ({f['stage']}): {f['error']}", flush=True)
    sys.exit(0 if failed == 0 else 1)

if __name__ == '__main__':
    main()
