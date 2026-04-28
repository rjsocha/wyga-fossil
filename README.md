# wyga-fossil

Personal fork of [Fossil-SCM](https://fossil-scm.org) extended with
native time tracking and a handful of CLI / UI ergonomics tweaks
aimed at LLM-agent-driven workflows.

> **Status: work in progress / experimental.**  The maintainer is
> shaping the tool around a personal workflow.  Changes are not
> well-tested, design decisions are not necessarily right or stable,
> and breaking changes can land at any time.  Treat this as an
> experiment, not a product.  Do not rely on it for anything you
> care about losing.

The executable is named `wyga-fossil` (via `Makefile.in::APPNAME`),
but otherwise the application identifies itself as fossil internally
(commands, version string, etc).

Base: trunk around fossil 2.29.  Not tracking upstream; this is a
frozen-in-time fork.

## What's different from upstream

- **First-class timer artifacts** (`CFTYPE_TIMER`, manifest Y-card).
  Sessions live in their own `timer` table; one row per session with
  start/stop times inline.  Each operation also lands as an event in
  the regular timeline so the audit log is immediate.
- **`fossil timer` CLI**: `start`, `stop`, `status`, `list`, `total`,
  `add`, `rm`, `edit` plus the `timer-min-seconds` setting.
- **`/timer` web dashboard** with live JS counter, START/STOP
  buttons, segment list with edit/delete actions, retroactive add
  form; `/timeredit` for single-session edits.
- **Wiki ergonomics**:
  - `wiki_meta` table for per-page descriptions.
  - `--body TEXT` (with `\n`/`\t`/`\\` escape decoding) and
    `--description TEXT` flags on `wiki create|commit`.
  - New subcommands: `wiki desc`, `wiki index` (md table or plain
    TAB), `wiki purge` (with `--obliterate` for irreversible delete).
  - Default mimetype for new CLI wiki pages is `text/x-markdown`.
- **`/home` change**: renders an auto-index table of wiki pages when
  the configured home page (default `INDEX`, settable via
  `home-wiki-page`) doesn't exist.
- **Filename convention**: per-project repos can use the bare
  basename `.fossil` (a hidden dotfile per project directory).
  Repolist scan, `vfile_scan`, and `main.c` URL routing were
  relaxed to discover and serve those files.
- **CLI shortcuts**: `-R DIR` auto-expands to `<DIR>/.fossil` when
  the directory contains one.  `fossil init PATH` mkdir-p's the
  parent and creates `<PATH>/.fossil` if PATH names a directory.
  `fossil ui DIR` does the same lookup.
- **UI defaults**: `/timeline` defaults to "Any Type" instead of
  check-ins-only.  `timeline-default-style` is registered as a
  setting and defaults to `j` (Columnar View).  `Chat` was removed
  from the default mainmenu (the feature itself is unchanged).
- **Server**: simplified `Listening on http://host:port` startup
  message; binds IPv4 only.

For implementation details and gotchas see [CLAUDE.md](CLAUDE.md).

## Build

A pre-baked Alpine builder image avoids re-pulling deps on every run:

```bash
# One-time: build the builder image (~12s, ~190 MB)
docker build -f Dockerfile.builder -t wyga-fossil-builder .

# Per build: ~30s clean / seconds when cached
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD:/src" -w /src wyga-fossil-builder
```

The default CMD does `./configure --static && make` followed by
`strip --strip-all` and `upx --best --lzma`, producing a ~3 MB
statically linked binary that runs on any Linux x86_64.  The output
file is `./wyga-fossil` in the bind mount, owned by you (thanks to
`--user`).

Override the CMD if you want to skip post-processing:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD:/src" -w /src wyga-fossil-builder \
    sh -c 'make clean && ./configure --static && make -j$(nproc)'
```

## License

BSD 2-Clause, inherited from upstream Fossil.  See `COPYRIGHT-BSD2.txt`.

## Upstream

For everything that isn't unique to this fork, the upstream
documentation at <https://fossil-scm.org> is authoritative.
