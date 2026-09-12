# dirwhale 🐋

A tiny, zero-dependency disk usage analyzer written in pure C99.
Find out what's eating your disk in seconds.

```
$ dirwhale -d 1 -n 5 ~/projects
/home/you/projects [23.4 GiB, 412077 files]
  |- media/  (12.6 GiB, 3102 files)
  |- mygame/  (6.02 GiB, 88413 files)
  |- node_modules/  (1.87 GiB, 288501 files)
  |- big-export.tar  (1.63 GiB, 1 file)
  |- notes/  (412 MiB, 3321 files)
  `- ... and 9 more entries (741 MiB)
```

## Features

- **Zero dependencies** — single C99 source file, only libc + dirent
- **Fast** — scans hundreds of thousands of files without breaking a sweat
- **Human-readable sizes** — B / KiB / MiB / GiB / TiB, auto-scaled
- **Tree view with depth & top-N control** — zoom in only as far as you want
- **Wildcard excludes** — skip `node_modules`, `*.iso`, whatever you like
- **File-type breakdown** — see which extensions dominate (`--types`)
- **JSON export** — pipe the results into your own tooling (`--json`)

## Build

```sh
gcc -O2 -o dirwhale src/main.c     # or just: make
```

Works on Windows (MinGW) and POSIX systems.

## Usage

```
dirwhale [options] [path]

  -d, --depth N      show children down to depth N (default 1)
  -n, --top N        biggest N entries per level (default 15)
  -e, --exclude PAT  skip wildcard pattern, may repeat
  -t, --types        show file-type breakdown
  -j, --json FILE    also write results as JSON
  -h, --help         show this help
```

Examples:

```sh
dirwhale E:/                      # what's big at the top level of E:?
dirwhale -d 2 -n 10 .             # two levels deep, top 10 per level
dirwhale -e node_modules -e .git  # ignore common junk
dirwhale -t ~/projects            # which file types eat the most?
dirwhale -j out.json C:/          # machine-readable output
```

> Note: dirwhale reports *apparent* file sizes (logical bytes). Tools like
> `du` report *disk usage*, which counts allocated clusters — numbers will
> differ slightly for many small files.
>
> Scan progress and errors go to **stderr**; the report itself goes to
> stdout, so redirecting stdout gives you a clean report.
>
> Symlinks are never followed (no recursive scans, no double counting):
> on POSIX a file symlink is counted by its own path length (like `du`),
> on Windows a file symlink/junction shows the target's size.
>
> Sizes use binary units (1 KiB = 1024 bytes). On Windows with MinGW,
> `mingw32-make` produces `dirwhale.exe`; plain `gcc -O2 -o dirwhale
> src/main.c` works everywhere.

## Why "dirwhale"?

Because you point it at a directory and it surfaces the whales — the
giant blobs hiding deep in your filesystem.

## License

MIT — see [LICENSE](LICENSE).
