#!/usr/bin/env python3
"""Build the documentation, verify local links/content, and copy it to docs/html."""
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / 'docs/site'
OUTPUT = SITE / 'dist'


class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.links = []
        self.title = False

    def handle_starttag(self, tag, attrs):
        data = dict(attrs)
        if 'id' in data:
            self.ids.add(data['id'])
        if tag == 'a' and 'name' in data:
            self.ids.add(data['name'])
        if tag == 'title':
            self.title = True
        for attr in ('href', 'src'):
            if data.get(attr):
                self.links.append((tag, data[attr], data))


def verify(base="/"):
    pages = {}
    # Preserve the original manual as an archive; do not impose modern HTML
    # conventions on its historical pages or obsolete external references.
    for path in OUTPUT.rglob('*.html'):
        if 'legacy' in path.relative_to(OUTPUT).parts:
            continue
        parser = Page()
        parser.feed(path.read_text())
        pages[path.resolve()] = parser
    issues = []
    links = 0
    for path, page in pages.items():
        if not page.title:
            issues.append(f'{path.name}: missing page title')
        for tag, url, attrs in page.links:
            split = urlsplit(url)
            if split.scheme or split.netloc:
                continue
            name = unquote(split.path)
            if name.startswith('/'):
                if not name.startswith(base):
                    issues.append(f'{path.relative_to(OUTPUT)}: URL escapes hosting base {base}: {url}')
                    continue
                dest = (OUTPUT / name[len(base):]).resolve()
            else:
                dest = (path.parent / name).resolve() if name else path
            if not dest.is_relative_to(OUTPUT.resolve()):
                issues.append(f'{path.relative_to(OUTPUT)}: URL escapes documentation directory: {url}')
                continue
            if dest.is_dir():
                dest = dest / 'index.html'
            if not dest.exists() and not dest.suffix:
                dest = dest.with_suffix('.html')
            links += 1
            if not dest.exists():
                issues.append(f'{path.relative_to(OUTPUT)}: missing {url}')
            elif split.fragment and dest in pages and unquote(split.fragment) not in pages[dest].ids:
                issues.append(f'{path.relative_to(OUTPUT)}: missing anchor {url}')
            if (tag == 'a' and dest.is_file() and dest.suffix == '.html' and dest not in pages
                    and 'target' not in attrs and 'download' not in attrs):
                issues.append(f'{path.relative_to(OUTPUT)}: standalone HTML link needs a target to bypass the VitePress router: {url}')
    options = set(re.findall(r'a == "(-[^\"]+)"', (ROOT / 'qtalh/services/options.cc').read_text()))
    reference = (SITE / 'reference/command-line.md').read_text()
    for option in options:
        if option not in reference:
            issues.append(f'Undocumented command-line spelling: {option}')
    config = (ROOT / 'qtalh/core/config.cc').read_text()
    supported = config.split('const QSet<QString> supported = {', 1)[1].split('};', 1)[0]
    directives = set(re.findall(r'"([A-Z_]+)"', supported)) | {'BEEPSEVERITY', 'END'}
    reference = (SITE / 'reference/configuration.md').read_text()
    for directive in directives:
        if '$' + directive not in reference:
            issues.append(f'Undocumented configuration directive: {directive}')
    if issues:
        raise RuntimeError('\n'.join(sorted(set(issues))))
    print(f'Validated {len(pages)} HTML pages, {links} local links/assets, {len(options)} option spellings, and {len(directives)} directives.')


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--check-only', action='store_true', help='Validate an existing build without rebuilding')
    args.add_argument('--base', default=os.environ.get('DOCS_BASE', '/'),
                      help='Hosting URL prefix, e.g. /manuals/QtALH/ (default: /)')
    options = args.parse_args()
    if (not re.fullmatch(r'/(?:[A-Za-z0-9_.-]+/)*', options.base)
            or any(part in ('.', '..') for part in options.base.split('/'))):
        args.error('--base must be a URL path with leading/trailing slashes and no dot segments')
    if not options.check_only:
        subprocess.run([sys.executable, str(ROOT / 'scripts/sync-docs.py')], check=True)
        npm = shutil.which('npm') or shutil.which('npm.cmd')
        if not npm:
            raise RuntimeError('Install Node.js and npm to build the documentation.')
        if not (SITE / 'node_modules/vitepress').is_dir():
            subprocess.run([npm, 'ci', '--no-audit', '--no-fund'], cwd=SITE, check=True)
        subprocess.run([npm, 'run', 'build'], cwd=SITE, check=True,
                       env={**os.environ, 'DOCS_BASE': options.base})
    if not (OUTPUT / 'index.html').is_file():
        raise RuntimeError('No built documentation found. Run without --check-only first.')
    verify(options.base)
    if not options.check_only:
        destination = ROOT / 'docs/html'
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(OUTPUT, destination)
        print('Built documentation: ' + str(destination / 'index.html'))
        print('Hosting URL prefix: ' + options.base)


if __name__ == '__main__':
    main()
