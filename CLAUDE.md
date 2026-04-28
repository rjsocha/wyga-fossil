# CLAUDE.md - fossil fork (timer + agent ergonomics)

This repo is a **fork** of upstream Fossil-SCM (cloned from
fossil-scm.org's git mirror).  Base is trunk around fossil 2.29.  We
are NOT tracking upstream - this is a personal fork, frozen to
whatever was on `master` when work began.  Treat upstream sync as
explicit user request only.

The fork repurposes fossil as an agent-friendly per-project storage
backend (wiki + tickets + first-class time tracking).  The driving
skill expects:

- Repo root on disk: `~/space/fossil/`
- Per project: `~/space/fossil/<NAMESPACE>/<NAME>/.fossil`
  (the hidden dotfile naming is the fork's convention)
- Single user (`fossil ui` localauth as developer); no separate `robot`
- Resilio Sync handles cross-machine replication at the filesystem
  level - fossil HTTP sync is unused

## Custom changes

Each item below is one line; the source is the source of truth.

### New first-class artifact type: timer events

Manifest directive `Y` carries a timer action (`start` or `stop`).
Type **CFTYPE_TIMER** (id 9) added to the parser.  Timer state is
denormalized into a `timer` table; the existing `event` table receives
one row per operation (type='m') so the timeline doubles as an audit
log.

- `src/manifest.c` - `CFTYPE_TIMER` constant, `manifestCardTypes[]`
  row, `azNameOfMType[]` entry, `Manifest::zTimerAction` field, `case 'Y'`
  parser, and `manifest_crosslink` block that mirrors timer artifacts
  into both `event` (type='m') and `timer`.
- `src/schema.c` - `CREATE TABLE timer(rid, start_mtime, stop_rid,
  stop_mtime, comment)`.  Each row is a full session.
- `src/timer.c` - `timer_create_artifact()` builder, `timer_aggregate()`
  reporter, the `timer_*_cmd()` CLI handlers, and the `/timer` and
  `/timeredit` web pages.  Includes `timer-min-seconds` SETTING for
  silently discarding too-short segments.
- `src/main.mk` and `tools/makemake.tcl` - `timer.c` added to source
  lists (no tclsh on the dev host so main.mk was edited by hand).

### Wiki extensions

- **`wiki_meta` table** in `src/schema.c` (`name PK, description TEXT`).
  Stores per-page short descriptions.  Repo-local; not synced through
  fossil HTTP sync (Resilio replicates the whole file).
- **`fossil wiki create|commit` flags**:
  - `-b|--body TEXT` - inline content; mutually exclusive with FILE.
    Backslash escapes (`\n`, `\r`, `\t`, `\0`, `\\`) are decoded so
    agents can pass literal `\n` in their shell quoting.
  - `-d|--description TEXT` - saves into `wiki_meta` after commit.
- **`fossil wiki desc PAGE [TEXT]`** - get/set/clear description.
- **`fossil wiki index [--format md|plain]`** - machine-readable
  page list.  Default Markdown table (`| Name | Description |`),
  `--format plain` gives `name - description` lines for `cut`/grep.
  No URLs in CLI output - agents call `wiki export NAME` directly.
- **`fossil wiki purge PAGE [--obliterate]`** - permanently delete
  every revision of a page plus its `wiki_meta` row.  Default sends
  artifacts to the graveyard (`fossil purge undo` restores);
  `--obliterate` skips the graveyard for irreversible deletion.
  CLI-only - no UI surface.
- **Default mimetype for new CLI wiki pages: `text/x-markdown`** (was
  `text/x-fossil-wiki` in upstream).  Existing pages still inherit
  their previous mimetype on `commit`.

### `/home` change

`src/wiki.c::home_page()`:

- Reads `home-wiki-page` SETTING (default `INDEX`) instead of
  `project-name`.
- When the page does not exist, renders an **auto-index** of all wiki
  pages as an HTML table (Name / Description columns, header in
  `#e8eef5`, all body rows in `#fafafa` - inline backgrounds defeat
  the skin's `body.wiki tr:nth-child(odd)` zebra rule).
- Same data layout as `fossil wiki index` CLI output.

### `/wikidesc` web page

POST endpoint to set/clear a wiki page's description.  The form is
embedded inline on `/wiki?name=PAGE` for users with `WrWiki`.
`/wikinew` (the "enter new page name" form) was extended with a
description field; saved before the redirect to the wikiedit SPA.

### Filename convention: `.fossil`

The fork treats the bare basename `.fossil` as the per-project
repository file.  Multiple places had to be relaxed because upstream
intentionally rejects that name:

- `src/repolist.c` - the GLOB filter (`*[^/].fossil`) was widened
  to also accept `*/.fossil` and standalone `.fossil`.
- `src/vfile.c` - the dot-prefix skip in `vfile_scan` exempts
  `.fossil` (and `.efossil` under SEE) so the repolist scanner
  discovers hidden-basename repos without `SCAN_ALL`.
- `src/main.c` - the special-case "Assume any file with a basename
  of '.fossil' does not exist" was removed.

### CLI ergonomics around `-R` / repository paths

- `find_repository_option` (`src/main.c`) - when `-R` points at a
  directory containing `.fossil`, expand to that file path.  Works for
  EVERY CLI subcommand transparently.
- `cmd_webserver` (`src/main.c`) - same shortcut for the implicit
  positional REPOSITORY argument of `ui`/`server` (skipped when
  `--repolist` is explicit).
- `create_repository_cmd` (`src/db.c`) - `fossil init PATH`:
  - PATH exists as directory -> create `<PATH>/.fossil`
  - PATH does not exist and ends in `.fossil` -> create the file
    (mkdir parents)
  - PATH does not exist otherwise -> mkdir + `<PATH>/.fossil`

### `/wcontent` CSS

`src/default.css` - `.brlist table` cells got ~+20% horizontal
padding overall, and the first column (`Name`) gets +40% extra so
wiki names breathe.

### Server defaults

- `src/cgi.c` - listen message rewritten to a single line
  `Listening on http://host:port`.  IPv6 binding was removed; fossil
  listens on IPv4 only.
- `src/timeline.c` - `/timeline` default filter changed from
  upstream's `ci` (check-ins only) to `all`.  Also registered new
  SETTING `timeline-default-style` (default `j` - Columnar View)
  so first paint matches what the user wants.
- `src/style.c` - `Chat` removed from the default `mainmenu`.  The
  feature itself is unchanged; just no nav link.

### Audit trail (timer ops)

Every destructive timer operation (add / rm / edit / sub-threshold
discard) emits a technote tagged `timer:audit` with bgcolor `#fef`.
This appears on the regular timeline (filterable with `-t e`) and
gives a full audit log even when the underlying timer artifacts have
been purged.  Aggregator queries `event.type='m'`, so audit technotes
do not affect totals.

## Building

A pre-baked Alpine builder image avoids re-pulling deps on every run:

```bash
# One-time: build the builder image (~12s, ~190MB)
docker build -f Dockerfile.builder -t fossil-builder .

# Per build: ~30s clean / seconds when cached
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD:/src" -w /src fossil-builder
```

Output: `./fossil` in the bind mount, owned by you (thanks to `--user`).
The default CMD does **strip + UPX**, producing a ~3 MB statically
linked binary that runs on any Linux x86_64.  Sizes:

- raw static link: ~32 MB
- after `strip --strip-all`: ~9 MB
- after `upx --best --lzma`: ~3 MB

Override the CMD if you want to skip post-processing:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD:/src" -w /src fossil-builder \
    sh -c 'make clean && ./configure --static && make -j$(nproc)'
```

If you ever ran the builder WITHOUT `--user` (root-owned `bld/`),
one-shot cleanup as root:

```bash
docker run --rm -v "$PWD:/src" -w /src --entrypoint sh fossil-builder \
    -c 'rm -rf bld fossil'
```

The host has musl-gcc and glibc-static libs, but **does not** have
musl-versions of openssl/zlib, so a host-side musl build would fail.
Glibc-static build also fails due to missing libzstd (transitive dep
of OpenSSL).  Stick with the Alpine-musl docker recipe.

## Gotchas worth remembering

- **`@`-line translator (`tools/translate.c`)** turns `@ ...` lines
  into `cgi_printf` calls.  Format-arg syntax: `%X(expr)` - a single
  pair of parens per placeholder, NO trailing collected `(args)`.  A
  literal `%` in HTML/JS bodies must be doubled: `%%`.  Modulo
  arithmetic inside `(expr)` is fine (it is C-side).
- **CSP nonce**: every inline `<script>` you add to a page MUST carry
  `nonce="%h(style_nonce())"` or the browser silently refuses to run
  it.  No console error reaches the casual user - they just see a
  page with broken JS.
- **Y-card type detection**: the parser sets `p->type = CFTYPE_TIMER`
  at parse time when it sees a Y card, before the
  `if( p->type==0 )` fallback in the post-loop.  This matters because
  a timer artifact also carries a C card and would otherwise be
  classified as `CFTYPE_MANIFEST`.
- **Floating-point rounding for durations**: julianday subtraction
  times 86400 sometimes yields 59.999999... for a literal 60-second
  span.  All `int` casts in `timer.c` use `+ 0.5` rounding to avoid
  off-by-one rejections of `timer-min-seconds`.
- **Purge graveyard tables are lazy**: `purge_artifact_list` only
  creates `purgeevent` and `purgeitem` when called with the
  `PURGE_MOVETO_GRAVEYARD` flag.  Don't `DELETE FROM` them
  unconditionally - they may not exist.
- **Skin zebra rule**: `body.wiki .content tr:nth-child(odd)` from
  the default skin recolors odd rows on `/home`.  For uniform color
  in a wiki-class table, use inline `style="..."` on each `<tr>`
  (specificity bias) - a single `<tbody style>` is not enough.
- **Avoid em-dash (U+2014) in CLI output and code comments**: it
  breaks ASCII greps, copy-paste through Excel/Word, and terminals
  without UTF-8.  Use plain `-` or `--`.  This also bit
  `listAllWikiPages` once when a sed sweep over em-dashes happened to
  match a `--` SQL comment.

## What is intentionally not done

- **Native sync of CFTYPE_TIMER through fossil's HTTP sync protocol**
  - not relevant; the user replicates by filesystem.  The artifact
  format is sync-correct (proper Z-card hashing); only cluster/sync
  paths haven't been audited for the new type.
- **JSON API for timer / wiki extensions** - explicitly rejected.
  Agents go through CLI.
- **Custom capabilities** - existing `RdWiki` / `WrWiki` are reused.
- **Edit history for timer events** - edits purge the old session
  and create a new one; audit technotes give the human-readable trail.
- **Tests** - no automated tests added.  Existing fossil test suite
  still runs but doesn't cover the new code paths.
- **Upstream contribution** - private fork; do not push to
  fossil-scm.org.

## Notes for the next agent

- Maintainer prefers short, factual responses with concrete commands.
  Estimating in "human-developer hours" misses the mark - for an LLM
  agent doing focused mechanical work, real wall-clock time is
  minutes, not weeks.  Don't quote inflated estimates.
- Maintainer reads code and runs the binary.  Don't explain things
  visible in `git diff` or in the file you just edited.
- When making web pages, test the actual HTTP response with `curl`,
  not just the build success.  Inline JS without a CSP nonce is the
  classic silent failure.
- `fossil ui` auto-launches a browser; use `fossil server` in test
  scripts to avoid spawning unwanted browser windows.
