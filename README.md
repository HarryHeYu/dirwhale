# dirwhale 🐋

A tiny, zero-dependency disk usage analyzer written in pure C99.
Find out what's eating your disk in seconds.

```
$ dirwhale -d 1 -n 5 E:/code
E:/code [17.9 GiB, 203628 files]
  |- 【持续更新】吴恩达大模型/  (7.81 GiB, 846 files)
  |- thecodeofc/  (5.07 GiB, 1849 files)
  |- kernel/  (1.54 GiB, 102893 files)
  |- linux-6.18.15.tar  (1.50 GiB, 1 file)
  |- Idea2Paper/  (305 MiB, 208 files)
  `- ... and 4 more entries (8.68 MiB)
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
