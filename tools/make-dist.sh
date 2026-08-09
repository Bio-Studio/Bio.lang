#!/bin/sh
# ═══════════════ make-dist.sh ═══════════════
# Build cross-platform release trees under bin/:
#
#   bin/<platform>/bin/bio(.exe)              CLI launcher (dynamic)
#   bin/<platform>/lib/libbio.so|dll|dylib    shared runtime (own artifact)
#   bin/<platform>/lib/...                    runtime dependencies pulled in
#   Platforms: linux-x86_64, linux-arm64, win32, win64, win-arm64,
#              macos-x86_64, macos-arm64
#
# Multi-file mode: the launcher is NOT a standalone single-file binary; it is
# dynamically linked against the shared runtime and its dependencies in ../lib
# (rpath $ORIGIN / @loader_path; Windows uses bio.bat + PATH).
# Requires zig on PATH (zig cc cross-compiles without foreign SDKs).
set -e

ZIG=${ZIG:-zig}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src"
OUT="$ROOT/bin"
export ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-/tmp/bio-zig-global}
export ZIG_LOCAL_CACHE_DIR=${ZIG_LOCAL_CACHE_DIR:-/tmp/bio-zig-local}

LIB_SRC="$SRC/arena.c $SRC/lexer.c $SRC/value.c $SRC/builtin.c $SRC/parser.c \
         $SRC/interp.c $SRC/bts.c $SRC/compile.c $SRC/toml.c $SRC/project.c \
         $SRC/platform.c $SRC/pack.c"

collect_linux_deps() {
    libdir="$1"; shift
    for f in "$@"; do
        ldd "$f" 2>/dev/null || true
    done | awk '/=> \// {print $3} /^\// {print $1}' | sort -u | while read -r so; do
        case "$so" in
            *linux-vdso*) continue ;;
            "$libdir"/*) continue ;;
        esac
        cp -f "$so" "$libdir"/ 2>/dev/null || true
    done
}

collect_win_deps() {
    libdir="$1"
    # mingw runtime DLLs are materialized inside zig's cache during the build.
    for dll in libwinpthread-1.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll \
               libgcc_s_sjlj-1.dll libstdc++-6.dll; do
        found=$(find "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR" \
                    -name "$dll" -type f 2>/dev/null | head -n1)
        if [ -n "$found" ]; then
            cp -f "$found" "$libdir"/
        fi
    done
}

build() {
    name="$1"; target="$2"; exe_ext="$3"; lib_name="$4"
    bindir="$OUT/$name/bin"
    libdir="$OUT/$name/lib"
    mkdir -p "$bindir" "$libdir"

    # Shared runtime: every platform's own compiled product. The SONAME /
    # install_name keeps the launcher's NEEDED entry as a bare name (resolved
    # through rpath), and the library's own rpath pulls its deps from ../lib.
    case "$name" in
        linux-*)
            $ZIG cc -shared -fPIC -O2 -Isrc -target "$target" \
                -Wl,-soname,"$lib_name" -Wl,-rpath,'$ORIGIN' \
                -o "$libdir/$lib_name" $LIB_SRC
            ;;
        macos-*)
            $ZIG cc -shared -fPIC -O2 -Isrc -target "$target" \
                -Wl,-install_name,@rpath/"$lib_name" \
                -o "$libdir/$lib_name" $LIB_SRC
            ;;
        *)
            $ZIG cc -shared -fPIC -O2 -Isrc -target "$target" \
                -o "$libdir/$lib_name" $LIB_SRC
            ;;
    esac

    # Launcher, dynamically linked against the shared runtime next to it.
    rpath='$ORIGIN/../lib'
    case "$name" in macos-*) rpath='@loader_path/../lib' ;; esac
    if ! $ZIG cc -O2 -Isrc -target "$target" -o "$bindir/bio$exe_ext" \
            "$SRC/main.c" -L"$libdir" -lbio -Wl,-rpath,"$rpath" -lm 2>/dev/null; then
        $ZIG cc -O2 -Isrc -target "$target" -o "$bindir/bio$exe_ext" \
            "$SRC/main.c" "$libdir/$lib_name" -Wl,-rpath,"$rpath"
    fi

    case "$name" in
        linux-*)
            collect_linux_deps "$libdir" "$bindir/bio$exe_ext" "$libdir/$lib_name"
            ;;
        win*)
            collect_win_deps "$libdir"
            # zig materializes an import library for bio.dll; give it a
            # conventional name (bio.lib) instead of arena.lib.
            if [ -f "$libdir/arena.lib" ]; then
                mv -f "$libdir/arena.lib" "$libdir/bio.lib"
            fi
            printf '@echo off\r\nset "BIO_BIN=%%~dp0"\r\nset "PATH=%%BIO_BIN%%..\\lib;%%PATH%%"\r\n"%%BIO_BIN%%bio.exe" %%*\r\n' \
                > "$bindir/bio.bat"
            ;;
        macos-*)
            : # system frameworks are provided by macOS; nothing extra to bundle
            ;;
    esac
    printf '%-16s %-24s ok: %s/bio%s + %s/%s\n' \
        "$name" "$target" "$bindir" "$exe_ext" "$libdir" "$lib_name"
}

build linux-x86_64   x86_64-linux-gnu    ""  libbio.so
build linux-arm64    aarch64-linux-gnu   ""  libbio.so
build win32          x86-windows-gnu     .exe bio.dll
build win64          x86_64-windows-gnu  .exe bio.dll
build win-arm64      aarch64-windows-gnu .exe bio.dll
build macos-x86_64   x86_64-macos        ""  libbio.dylib
build macos-arm64    aarch64-macos       ""  libbio.dylib

echo "done: release trees under $OUT"
