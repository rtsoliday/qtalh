# Maintain these docs

The documentation is a VitePress site built from Markdown. The repository keeps the page sources and build scripts; the generated site lives in `docs/html`. Sites publishes an identical copy of that static output.

## Build and view locally

Install Node.js 22 or newer with npm, and Python 3. From the repository root:

```sh
make docs
python3 -m http.server 8000 --directory docs/html --bind 127.0.0.1
```

Open `http://127.0.0.1:8000/`. Serve `docs/html` as the web root; opening `index.html` directly with a `file://` URL will not provide the site's module loading and local search correctly.

`make docs` installs the locked documentation dependencies when absent, refreshes imported material, builds the static HTML, validates links and reference coverage, then copies the validated result to `docs/html`. For a clean dependency install, run `npm ci --prefix docs/site` first. EPICS and Qt are not required to build the website.

## Host on your own web server

The site is static: copy the entire contents of `docs/html/`, including all
subdirectories, into the destination web directory. Node.js, Python, Qt, and
EPICS are needed only as applicable for building, not for serving these files.
Configure the server to serve `index.html` as the directory index.

The default `make docs` build uses the website root (`/`), as the Sites mirror does.
If the destination is a subdirectory, build with that exact URL prefix first:

```sh
make docs DOCS_BASE=/manuals/QtALH/
```

Copy the resulting `docs/html/` contents into the directory served at
`https://ops.aps.anl.gov/manuals/QtALH/`. This sets the stylesheet, script,
navigation, image, and search URLs correctly. A root build copied into a
subdirectory will appear unstyled and have broken links. Rebuild and copy all
files whenever the prefix changes; do not copy only `index.html`.

The equivalent script command is `python3 scripts/build-docs.py --base /manuals/QtALH/`.
To check that build again, pass the same prefix with `--check-only`.
`make clean` and `make distclean` remove `docs/html`; `make docs` recreates it.

## Choose the source to edit

| Content | Edit here |
| --- | --- |
| Tutorials and focused how-to guides | The corresponding Markdown page under `docs/site/`. |
| Installation and build reference | The repository `README.md`. |
| CLI, configuration, logging, and troubleshooting reference | `docs/qtalh-user-guide.md`. |
| Test guide | `qtalh/tests/README.md`. |
| Compatibility and historical records | The corresponding `docs/qtalh-*.md` file. |
| Author credits | `AUTHORS.md`. |
| Navigation and appearance | `docs/site/.vitepress/config.mts` and `.vitepress/theme/`. |

`scripts/sync-docs.py` refreshes imported pages and downloads. Generated pages contain a comment naming that script; edit their source documents rather than the generated copy. The sync step also carries the original manual and linked benchmark evidence into the website.

## Version control and cleanup

Commit authored Markdown pages, site configuration and theme, `package.json`,
`package-lock.json`, and the retained assets in `public/images/`, along with the
repository's documentation scripts and tutorial examples. Generated Markdown
pages and `public/downloads/` and `public/legacy/` are ignored: their original
sources are already kept elsewhere in the repository. Do not add generated
copies with `git add -f`.

`make clean` removes the generated pages, downloads, archive copies, VitePress
cache, `docs/site/dist`, and `docs/html`, while preserving authored files and
installed npm dependencies. `make distclean` also removes
`docs/site/node_modules`. Both commands additionally clean the application builds;
use `make docs-clean` or `make docs-distclean` to clean only documentation.
Documentation cleanup requires Python 3, but neither Node.js nor npm.

Run `make docs` to recreate the generated files. When adding a new imported page,
register its exact path in `GENERATED_PAGES` in `scripts/sync-docs.py` and in
`docs/site/.gitignore`; never ignore a whole directory containing authored pages.

## Preview while writing

```sh
python3 scripts/sync-docs.py
npm run dev --prefix docs/site
```

Open the local URL printed by VitePress. Changes to authored pages refresh automatically. Rerun the sync command after editing imported source documents.

## Validate a change

The production build checks Markdown links. `scripts/build-docs.py` additionally checks generated page titles, local page/asset links, anchors, command-line spellings, and configuration directive coverage against the parser. Links to standalone HTML archives must use `target="_self"` so VitePress leaves navigation to the browser; the checker verifies this on generated pages. Raw HTML download and archive links use page-relative paths, and the runtime screenshot uses VitePress `withBase`; keep those links portable when editing. The checker rejects URLs that escape the selected hosting prefix. It preserves the legacy manual itself as an archive rather than rewriting its old links.

For an existing build:

```sh
python3 scripts/build-docs.py --check-only
```

Validate new `.alhConfig` examples with `qtalh --validate` when the executable is available. For new operator procedures, state prerequisites, actions, expected results, and how to finish. Keep implementation investigations in Engineering history and include the measurement date and environment.

## Publish the same content

Build with `make docs DOCS_BASE=/` for Sites, even if the previous build targeted the APS subdirectory. The documentation project's `.openai/hosting.json` identifies the Sites mirror and declares `dist` as its static output. Publish only the validated `docs/site/dist` output with the matching documentation source. The local `docs/html` copy must match it byte for byte. No server-side application, account data, or external search service is needed by the documentation itself.

The repository's documentation workflow builds and validates changes in CI and retains the generated HTML as a downloadable artifact. It does not automatically publish Sites or change the site's access permissions.
