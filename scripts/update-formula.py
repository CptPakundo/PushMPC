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

# the Mac-only archives live in the formula's one on_macos block (brew audit allows one, before the resources),
# each between its own marker comments: "    # >>> plex-mac" ... "    # <<< plex-mac" (and kodi-mac)
def part_re(name):
    return r'    # >>> ' + name + r'\n(?:.*\n)*?    # <<< ' + name + r'\n'


def res_re(name):
    return r'(    resource "' + name + r'" do\n      url )".*"(\n      sha256 )".*"'


def part_block(name, url, sha):
    return (f'    # >>> {name}\n    resource "{name}" do\n      url "{url}"\n      sha256 "{sha}"\n    end\n'
            f'    # <<< {name}\n')


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
    # Plex for Mac, then Kodi for Mac: replaced in its part, added at the end of the on_macos block (after the
    # molten-vk line, or after the plex-mac part), or its part left out
    for name, path, url, after in (('plex-mac', a.plex, a.plex_url, None), ('kodi-mac', a.kodi, a.kodi_url, 'plex-mac')):
        part = re.search(part_re(name), text)
        if path and os.path.isfile(path):
            sha = sha256(path)
            if part:
                text = sub1(res_re(name), lambda m: f'{m.group(1)}"{url}"{m.group(2)}"{sha}"', text, f'resource "{name}"')
            elif after and re.search(part_re(after), text):
                text = sub1('(' + part_re(after) + ')', lambda m: m.group(1) + part_block(name, url, sha), text,
                            f'the end of the {after} part')
            else:
                text = sub1(r'(^    depends_on "molten-vk"\n)', lambda m: m.group(1) + part_block(name, url, sha), text,
                            'depends_on "molten-vk" in the on_macos block')
            print(f'update-formula: {name} ->', url)
        else:
            if part:
                text = re.sub(part_re(name), '', text, count=1)
            print(f'update-formula: no {name} archive: the {name} resource is left out')
    open(a.formula, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'update-formula: {a.formula} -> {a.version} ({a.src_url})')


if __name__ == '__main__':
    main()
