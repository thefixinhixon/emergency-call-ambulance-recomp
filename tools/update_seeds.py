#!/usr/bin/env python3
"""Merge hybrid-core miss logs into a ppcrecomp seed file.

    tools/update_seeds.py config/eca_seeds.txt build/run/ppc_misses.txt [more logs...]

Each miss log lists guest PCs the dispatcher had to interpret because no recompiled
unit started there. Adding them as seeds makes the next build recompile them.
Header comment lines of the seed file are kept; addresses are de-duplicated and sorted.
"""
import sys


def read_addrs(path):
    addrs = set()
    try:
        with open(path) as f:
            for line in f:
                line = line.split('#', 1)[0].strip()
                if line:
                    addrs.add(int(line, 16))
    except FileNotFoundError:
        pass
    return addrs


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    seed_path = sys.argv[1]
    header = []
    try:
        with open(seed_path) as f:
            header = [l for l in f if l.startswith('#')]
    except FileNotFoundError:
        pass
    seeds = read_addrs(seed_path)
    before = len(seeds)
    for log in sys.argv[2:]:
        seeds |= read_addrs(log)
    with open(seed_path, 'w') as f:
        f.writelines(header)
        for a in sorted(seeds):
            f.write('%08X\n' % a)
    print('%s: %d seeds (+%d)' % (seed_path, len(seeds), len(seeds) - before))


if __name__ == '__main__':
    main()
