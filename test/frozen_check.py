#!/usr/bin/env python3
"""frozen_check.py - the ABIs frozen at nui 1.0.0 stay frozen.

    python frozen_check.py               every frozen line still in the live layout files (exit 1 if not)
    python frozen_check.py --self-test   a changed offset, a removed field and a renamed mapping in temp
                                         copies must each fail the check; an appended field must pass

test/frozen/layout_uihook.1.0.0.txt and layout_nuicache.1.0.0.txt are the contract: UiHook ABI 2
revision 1 and NuiCache major 1 exactly as released in nui 1.0.0. The live layout files
(layout_uihook.txt, layout_nuicache.txt, checked against the headers by hook_matrix and test_cache)
may only gain lines (a field appended into reserved space by a later revision; the reserved* lines
move and shrink then, so they are not frozen). Any other change needs a new major (a new mapping
name), which is a new contract, not an edit of this one.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAIRS = [('uihook', 'frozen/layout_uihook.1.0.0.txt', 'layout_uihook.txt'),
         ('nuicache', 'frozen/layout_nuicache.1.0.0.txt', 'layout_nuicache.txt'),
         ('padhook', 'frozen/layout_padhook.1.3.0.txt', 'layout_padhook.txt')]   # PadHook 1, frozen at 1.3.0


def frozen_lines(path):
    out = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line.strip() or line.startswith('#') or 'reserved' in line.split(' ')[0]:
                continue
            out.append(line)
    return out


def check(root, quiet=False):
    bad = 0
    for name, frozen, live in PAIRS:
        want = frozen_lines(os.path.join(root, frozen))
        with open(os.path.join(root, live), encoding='utf-8') as f:
            have = set(line.rstrip('\r\n') for line in f)
        missing = [line for line in want if line not in have]
        if not quiet:
            print('  %-8s %d frozen lines, %d changed' % (name, len(want), len(missing)))
            for line in missing:
                print('FAIL [frozen] %s: %s' % (name, line))
        bad += len(missing)
    return bad == 0


def self_test():
    cases = {
        'changed offset': lambda t: t.replace('NcEntry.obj 40', 'NcEntry.obj 44'),
        'removed field': lambda t: '\n'.join(l for l in t.split('\n') if not l.startswith('NcHeader.gen ')),
        'renamed mapping': None,
        'appended field (allowed)': lambda t: t + 'NcHeader.new_field 116\n',
    }
    ok = True
    for what, fn in cases.items():
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, 'frozen'))
            for _, frozen, live in PAIRS:
                shutil.copy(os.path.join(HERE, frozen), os.path.join(tmp, frozen))
                shutil.copy(os.path.join(HERE, live), os.path.join(tmp, live))
            if fn is None:
                p = os.path.join(tmp, 'layout_uihook.txt')
                t = open(p, encoding='utf-8').read()
                open(p, 'w', encoding='utf-8', newline='\n').write(t.replace('FFXiUiHook_v2_', 'FFXiUiHook_v3_'))
            else:
                p = os.path.join(tmp, 'layout_nuicache.txt')
                t = open(p, encoding='utf-8').read()
                open(p, 'w', encoding='utf-8', newline='\n').write(fn(t))
            passed = check(tmp, quiet=True)
            want = what.endswith('(allowed)')
            good = passed == want
            ok = ok and good
            print('  %-26s %s' % (what, ('passes' if passed else 'fails') + ('' if good else '  <-- WRONG')))
    print('frozen self-test: %s' % ('pass' if ok else 'FAILED'))
    return ok


if __name__ == '__main__':
    if '--self-test' in sys.argv:
        sys.exit(0 if self_test() else 1)
    ok = check(HERE)
    print('frozen ABIs: %s' % ('unchanged' if ok else 'CHANGED'))
    sys.exit(0 if ok else 1)
