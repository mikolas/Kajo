# `kajo-amiga` Motorola 68000 & C Cross-Toolchain Guide

This document explains which external tools `kajo-amiga` expects on Linux, how to install the fastest prebuilt bundle, and how to compile every component from source (including matching host GCC/Binutils versions on rolling distributions such as CachyOS / Arch Linux).

> [!NOTE]
> `adflib` (`0.10.7` built-in) is compiled directly into `kajo-amiga` for `.adf` creation and staging, and **Amiberry** (`/usr/bin/amiberry` or `PATH`) is packaged by most distributions. Only the cross-compiler/assembler/linker tools below need to be installed separately.

---

## 1. What `kajo-amiga` Probes & Calls

`amiga_project_probe_toolchain()` (`src/amiga/amiga_project.c`) searches `PATH` first and then any directories configured in `extra_bin_paths` (`amiga.toml` / `kajo-settings --page amiga`).

| Tool Binary | Role | Required For |
| :--- | :--- | :--- |
| **`vasmm68k_mot`** | Portable 680x0 assembler (Motorola syntax) | All pipelines (`elf2hunk`, `hunk_vlink`, `single_asm`) |
| **`m68k-amiga-elf-gcc`** | GCC cross-compiler targeting `m68k-amiga-elf` | C / mixed C+ASM (`elf2hunk`) |
| **`m68k-amiga-elf-ld`** | GNU ELF linker (`--emit-relocs -Ttext=0`) | `elf2hunk` pipeline + auto `$SDKDIR` discovery |
| **`m68k-amiga-elf-objdump`** | ELF / DWARF3 inspection fallback | Fallback when `libdw` symbol info is absent |
| **`elf2hunk`** | Converts linked ELF executables to Amiga Hunk | `elf2hunk` pipeline |
| **`vlink`** | Portable Amiga Hunk linker (`-bamigahunk`) | `hunk_vlink` pipeline |
| **`vc`** | Optional VBCC C compiler frontend | Optional Hunk C workflows |
| **`amiberry`** | Amiga emulator ($\ge$ 8.5.0 with `-s gdbserver`) | Live Run / Attach / RSP debugging |
| **`clangd`** *(optional)* | C Language Server (consumes `.kajo-amiga/compile_commands.json`) | C IDE diagnostics & completion |
| **`m68k-lsp-server`** *(optional)* | 68k Assembly Language Server | 68k ASM IDE diagnostics & completion |

### Build Pipelines in `src/amiga/amiga_build.c`

| Pipeline | Assemble / Compile | Link / Package | Debug Info |
| :--- | :--- | :--- | :--- |
| **`elf2hunk`** *(default, Option D)* | `vasmm68k_mot -Felf -dwarf=3` + `m68k-amiga-elf-gcc -c -g -O2 -MMD -MF` | `m68k-amiga-elf-ld --emit-relocs -Ttext=0` → `elf2hunk` | Full DWARF3 `.debug_line` + ELF symbols |
| **`hunk_vlink`** | `vasmm68k_mot -Fhunk -linedebug` | `vlink -bamigahunk` | Native Amiga `HUNK_SYMBOL` + `HUNK_DEBUG LINE` |
| **`single_asm`** | `vasmm68k_mot -Fhunkexe -linedebug` | Direct Hunk executable | Single-file 68k ASM experiments |

### Auto-Discovered `$SDKDIR`

When `m68k-amiga-elf-ld` is found, `amiga_project_discover_sdkdir()` automatically resolves `$SDKDIR` to:

```text
<dir_of(m68k-amiga-elf-ld)>/../m68k-amiga-elf/sys-include
```

For example, if binaries are under `~/.local/opt/amiga-elf/bin/`, NDK headers placed in `~/.local/opt/amiga-elf/m68k-amiga-elf/sys-include/` are automatically expanded for `-I$SDKDIR` targets and `compile_commands.json`.

> [!WARNING]
> **`bebbo/amiga-gcc` (`m68k-amigaos-gcc`) vs `m68k-amiga-elf-gcc`:** Bebbo's classic `amiga-gcc` toolchain builds `m68k-amigaos-gcc` (direct Hunk/a.out output), **not** `m68k-amiga-elf-gcc` / `elf2hunk`. It provides `vasmm68k_mot` and `vlink` for `hunk_vlink`/`single_asm`, but will not satisfy `kajo-amiga`'s default `elf2hunk` Option D pipeline.

---

## 2. Option 1 — Fast Prebuilt Toolchain (`vscode-amiga-debug` `.vsix`)

If you do not need to compile GCC yourself, Bartman/Abyss distributes prebuilt Linux x86_64 binaries (`m68k-amiga-elf-gcc`, `m68k-amiga-elf-ld`, `m68k-amiga-elf-objdump`, `elf2hunk`, `vasmm68k_mot`) together with Amiga NDK `sys-include` headers inside the `.vsix` package on [`BartmanAbyss/vscode-amiga-debug` Releases](https://github.com/BartmanAbyss/vscode-amiga-debug/releases). A `.vsix` file is a standard ZIP archive:

```bash
mkdir -p ~/.local/opt/amiga-elf && cd /tmp
# 1. Download the latest amiga-debug-*.vsix release from GitHub into /tmp
# 2. Extract only the Linux toolchain tree:
unzip -q amiga-debug-*.vsix 'extension/bin/linux/opt/*'
cp -a extension/bin/linux/opt/* ~/.local/opt/amiga-elf/
chmod +x ~/.local/opt/amiga-elf/bin/*
```

The extracted layout (`bin/m68k-amiga-elf-*` and `m68k-amiga-elf/sys-include`) matches `kajo-amiga`'s expectations out of the box and includes Bartman's Amiga register-parameter (`regparm`) compiler patches.

---

## 3. Option 2 — Compile From Source on Linux

### 3.1 Host Prerequisites

**Arch Linux / CachyOS:**

```bash
sudo pacman -S --needed base-devel git wget gmp mpfr libmpc flex bison texinfo lhasa unzip
```

**Debian / Ubuntu:**

```bash
sudo apt install build-essential git wget libgmp-dev libmpfr-dev libmpc-dev flex bison texinfo lhasa unzip
```

Set a prefix (no `sudo` required under `~/.local/opt`):

```bash
export AMIGA_PREFIX="$HOME/.local/opt/amiga-elf"
mkdir -p "$AMIGA_PREFIX/bin" ~/src/amiga-toolchain
export PATH="$AMIGA_PREFIX/bin:$PATH"
```

---

### 3.2 `vasmm68k_mot` and `vlink` (~15 seconds)

Building `vasmm68k_mot` (plus `vlink`) alone is enough for **`single_asm`** and **`hunk_vlink`** assembly projects.

**On Arch / CachyOS via AUR:**

```bash
paru -S vasm vlink-git
```

**Or from Frank Wille's official source tarballs (any distro):**

```bash
cd ~/src/amiga-toolchain

# vasm (Motorola 68k syntax -> vasmm68k_mot)
wget http://sun.hasenbraten.de/vasm/release/vasm.tar.gz
tar xf vasm.tar.gz && cd vasm
make -j"$(nproc)" CPU=m68k SYNTAX=mot
cp vasmm68k_mot "$AMIGA_PREFIX/bin/"

# vlink (optional unless using hunk_vlink pipeline)
cd ~/src/amiga-toolchain
wget http://sun.hasenbraten.de/vlink/release/vlink.tar.gz
tar xf vlink.tar.gz && cd vlink
mkdir -p objects
make -j"$(nproc)"
cp vlink "$AMIGA_PREFIX/bin/"
```

---

### 3.3 `elf2hunk`

```bash
cd ~/src/amiga-toolchain
git clone https://github.com/BartmanAbyss/elf2hunk.git
cd elf2hunk
make -j"$(nproc)"
cp elf2hunk "$AMIGA_PREFIX/bin/"
```

---

### 3.4 `m68k-amiga-elf` Binutils + GCC

> [!IMPORTANT]
> **Match your host GCC version on rolling distros!** On systems running **GCC 15 or GCC 16** as the host compiler (for example CachyOS with GCC 16.2.1 / Binutils 2.47), GCC defaults to `-std=gnu23` and stricter C++ standard library headers, causing older source tarballs like GCC 14.2.0 to fail to compile. Use **Binutils 2.47** + **GCC 16.2.0** (matching your host compiler series).

```bash
cd ~/src/amiga-toolchain
wget https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz
tar xf binutils-2.47.tar.xz
tar xf gcc-16.2.0.tar.xz

# 1. Binutils (m68k-amiga-elf-as, -ld, -objdump, -ar, ...)
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
../binutils-2.47/configure \
  --target=m68k-amiga-elf \
  --prefix="$AMIGA_PREFIX" \
  --disable-nls --disable-werror
make -j"$(nproc)"
make install

# 2. Bare-metal GCC C compiler + libgcc
# Ensure "$AMIGA_PREFIX/bin" is already in $PATH so libgcc finds m68k-amiga-elf-as / ld!
cd ~/src/amiga-toolchain
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../gcc-16.2.0/configure \
  --target=m68k-amiga-elf \
  --prefix="$AMIGA_PREFIX" \
  --enable-languages=c \
  --without-headers \
  --disable-nls --disable-shared --disable-threads \
  --disable-libssp --disable-libgomp --disable-libquadmath \
  --disable-werror
make -j"$(nproc)" all-gcc all-target-libgcc
make install-gcc install-target-libgcc
```

**Vanilla GCC vs Bartman's patched GCC:**
- The vanilla build above compiles bare-metal C + inline/linked 68k ASM demos cleanly (`-c -g -O2 -mcpu=68000`).
- If you also need Bartman's AmigaOS `__reg("...")` / `regparm` patches and pre-bundled NDK headers from source, apply the patches (`gcc-barto.patch` / `gcc-regparm.patch`) and `sys-include` tree from `BartmanAbyss/vscode-amiga-debug` before configuring GCC (or use [Option 1](#2-option-1--fast-prebuilt-toolchain-vscode-amiga-debug-vsix)).

---

## 4. Option 3 — Optional Language Servers (`clangd` & `m68k-lsp-server`)

```bash
# C LSP (Arch / CachyOS)
sudo pacman -S clang

# 68k ASM LSP (requires Node.js + pnpm)
cd ~/src/amiga-toolchain
git clone https://github.com/grahambates/m68k-tools.git
cd m68k-tools
corepack enable && pnpm install --frozen-lockfile && pnpm build
# Link the compiled CLI bin (from packages/m68k-lsp-server) into ~/.local/bin/m68k-lsp-server
```

---

## 5. Wiring the Toolchain Into `kajo-amiga` & Verifying

You can expose the toolchain in either of two ways:

1. **Via `PATH`** in `~/.bashrc` / `~/.zshrc`:
   ```bash
   export PATH="$HOME/.local/opt/amiga-elf/bin:$PATH"
   ```
2. **Via `kajo-settings` (`amiga.toml`):**
   - Launch `kajo-settings --page amiga`.
   - Open Card **2. TOOLCHAIN, ADFLIB & HRM DOCS**.
   - Set **Extra Cross-Toolchain PATH** (`extra_bin_paths`, colon-separated) to `/home/<user>/.local/opt/amiga-elf/bin` and click **SAVE & APPLY**.

### Quick CLI Verification

```bash
amiberry --version
vasmm68k_mot -version | head -n 2
m68k-amiga-elf-gcc --version | head -n 1
m68k-amiga-elf-ld --version | head -n 1
elf2hunk --help 2>&1 | head -n 2
ls -d ~/.local/opt/amiga-elf/m68k-amiga-elf/sys-include
```

Then open `kajo-settings --page amiga` and confirm the live probe badges under **2. TOOLCHAIN, ADFLIB & HRM DOCS** switch from `MISSING` to green paths (`adflib` should show `0.10.7`).

---

## 6. Troubleshooting

- **`gcc` fails to compile with C23 / header errors on Arch / CachyOS:** You are compiling an older GCC tarball (such as `14.2.0`) with host GCC 15/16. Switch to **Binutils 2.47 + GCC 16.2.0** (matching the host GCC series).
- **`all-target-libgcc` fails with `m68k-amiga-elf-as: command not found`:** Binutils was installed to `$AMIGA_PREFIX`, but `$AMIGA_PREFIX/bin` was not added to `PATH` in the current shell before running GCC `configure`/`make`. Run `export PATH="$AMIGA_PREFIX/bin:$PATH"` and rebuild inside a clean `build-gcc/`.
- **Upstream `config.sub` rejects `--target=m68k-amiga-elf`:** Bare-metal `m68k-elf` is accepted by all upstream GNU tarballs. If a specific tarball rejects `m68k-amiga-elf`, configure with `--target=m68k-elf` and create compatibility symlinks inside `$AMIGA_PREFIX/bin/`:
  ```bash
  cd "$AMIGA_PREFIX/bin"
  for t in gcc ld objdump as ar strip; do
    ln -sf "m68k-elf-$t" "m68k-amiga-elf-$t"
  done
  ```
- **NDK headers not picked up by `$SDKDIR`:** Confirm `m68k-amiga-elf/sys-include` exists one directory above `bin/` (`dirname(m68k-amiga-elf-ld)/../m68k-amiga-elf/sys-include`).
