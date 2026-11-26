#!/usr/bin/env python3
"""update-formula.py: points Formula/pushmpc.rb at a release's archives (or CI's local ones).

  update-formula.py <formula> --version V --src FILE --src-url URL --pmml FILE --pmml-url URL
                    [--plex FILE --plex-url URL] [--kodi FILE --kodi-url URL]

Sets the formula's url / version / sha256 (the source tarball), the pmml-graphs resource, the plex-mac resource
(Plex for Mac) and the kodi-mac resource (the Kodi add-on for Kodi on the Mac). Without --plex / --kodi, or when that
file does not exist, that block is left out (the formula then installs without it). The SHA-256 values are computed from the files. Exit 1 with the reason when the
formula does not look as expected (nothing is written then).
"""
import argparse
import hashlib
import os
import re
import sys

PLEX_BLOCK = '''  on_macos do
    resource "plex-mac" do
      url "{url}"
      sha256 "{sha}"
    end
  end
'''
KODI_BLOCK = PLEX_BLOCK.replace('"plex-mac"', '"kodi-mac"')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('formula')
    ap.add_argument('--version', required=True)
    ap.add_argument('--src', required=True)
    ap.add_argument('--src-url', required=True)
    ap.add_argument('--pmml', required=True)
    ap.add_argument('--pmml-url', required=True)
    ap.add_argument('--plex', default='')
    ap.add_argument('--plex-url', default='')
    ap.add_argument('--kodi', default='')
    ap.add_argument('--kodi-url', default='')
    a = ap.parse_args()
    for f in (a.src, a.pmml):
        if not os.path.isfile(f):
            sys.exit(f'update-formula: {f} is missing')
    text = open(a.formula, encoding='utf-8').read()

    def sub1(pattern, repl, s, what):
        out, n = re.subn(pattern, repl, s, count=1, flags=re.M)
        if n != 1:
            sys.exit(f'update-formula: {what} not found in {a.formula}')
        return out

    # the formula's own source (the first url / version / sha256 lines, at two spaces)
    text = sub1(r'^  url ".*"$', f'  url "{a.src_url}"', text, 'url')
    text = sub1(r'^  version ".*"$', f'  version "{a.version}"', text, 'version')
    text = sub1(r'^  sha256 ".*"$', f'  sha256 "{sha256(a.src)}"', text, 'sha256')
    # pmml's graphs
    text = sub1(r'(  resource "pmml-graphs" do\n    url )".*"(\n    sha256 )".*"',
                lambda m: f'{m.group(1)}"{a.pmml_url}"{m.group(2)}"{sha256(a.pmml)}"', text, 'resource "pmml-graphs"')
    # Plex for Mac: replaced, added after pmml-graphs, or left out
    plex_re = r'  on_macos do\n    resource "plex-mac" do\n      url ".*"\n      sha256 ".*"\n    end\n  end\n'
    has = re.search(plex_re, text) is not None
    if a.plex and os.path.isfile(a.plex):
        block = PLEX_BLOCK.format(url=a.plex_url, sha=sha256(a.plex))
        if has:
            text = re.sub(plex_re, lambda m: block, text, count=1)
        else:
            text = sub1(r'(  resource "pmml-graphs" do\n    url .*\n    sha256 .*\n  end\n)', lambda m: m.group(1) + '\n' + block, text,
                        'the end of resource "pmml-graphs"')
        print('update-formula: plex-mac ->', a.plex_url)
    else:
        if has:
            text = re.sub(plex_re, '', text, count=1)
        print('update-formula: no Plex for Mac archive: the plex-mac resource is left out')
    # Kodi for Mac: replaced, added after the last resource block before it (plex-mac or pmml-graphs), or left out
    kodi_re = plex_re.replace('"plex-mac"', '"kodi-mac"')
    has = re.search(kodi_re, text) is not None
    if a.kodi and os.path.isfile(a.kodi):
        block = KODI_BLOCK.format(url=a.kodi_url, sha=sha256(a.kodi))
        if has:
            text = re.sub(kodi_re, lambda m: block, text, count=1)
        elif re.search(plex_re, text):
            text = sub1(r'(' + plex_re + r')', lambda m: m.group(1) + '\n' + block, text, 'the end of resource "plex-mac"')
        else:
            text = sub1(r'(  resource "pmml-graphs" do\n    url .*\n    sha256 .*\n  end\n)', lambda m: m.group(1) + '\n' + block, text,
                        'the end of resource "pmml-graphs"')
        print('update-formula: kodi-mac ->', a.kodi_url)
    else:
        if has:
            text = re.sub(r'\n?' + kodi_re, '', text, count=1)
        print('update-formula: no Kodi for Mac package: the kodi-mac resource is left out')
    open(a.formula, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'update-formula: {a.formula} -> {a.version} ({a.src_url})')


if __name__ == '__main__':
    main()
