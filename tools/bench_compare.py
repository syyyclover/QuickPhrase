#!/usr/bin/env python3
"""Interleaved five-run host medians; fail a hotspot regression over 10%."""
import argparse,json,statistics,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--runs',type=int,default=5)
    a=p.parse_args()
    if a.runs<5:p.error('at least five runs are required')
    samples={'baseline':[],'candidate':[]}
    for n in range(a.runs):
        order=('baseline','candidate') if n%2==0 else ('candidate','baseline')
        for name in order:
            result=subprocess.run([str(getattr(a,name).resolve())],capture_output=True,text=True,check=True,timeout=120)
            data=json.loads(result.stdout)
            if data.get('schema')!=1:raise ValueError('benchmark schema mismatch')
            samples[name].append(data)
    medians={name:{k:statistics.median(row[k] for row in rows) for k in rows[0] if k!='schema'}
             for name,rows in samples.items()}
    ratio={k:medians['candidate'][k]/medians['baseline'][k] for k in medians['baseline'] if k.endswith('_ms')}
    passed=all(v<=1.10 for v in ratio.values())
    print(json.dumps({'runs':a.runs,'scope':'host synthetic, not Windows acceptance',
                      'medians':medians,'ratios':ratio,'hotspot_gate_passed':passed},indent=2))
    return 0 if passed else 1
if __name__=='__main__':raise SystemExit(main())
