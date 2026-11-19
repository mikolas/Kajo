#!/usr/bin/env bash
# Repeatable static checks for bad practices that the normal build does not flag.
# Read-only: builds go to /tmp, nothing in the working tree is changed.
#
#   scripts/audit.sh warnings   extra compiler warnings (-Wshadow, -Wformat=2, ...)
#   scripts/audit.sh tidy       clang-tidy / clang static analyzer over src/ and tests/
#   scripts/audit.sh sizes      longest functions and files (split candidates)
#   scripts/audit.sh dups       duplicated 8-line windows grouped by file pair
#   scripts/audit.sh blocking   synchronous subprocess / D-Bus calls (UI-thread risks)
#   scripts/audit.sh lifetimes  async calls that pass a raw pointer with no cancellable
#   scripts/audit.sh colors     raw hex colours in style.css that should be tokens
#   scripts/audit.sh comp       kajo-comp rules (spec §7.2/§7.3): no assert/abort/blocking calls in
#                               src/comp, no wlroots/Vulkan/GLib in src/comp/core. Exits 1 on violation.
#   scripts/audit.sh all
#
# Memory/UB bugs are found at runtime instead: scripts/asan-test.sh
set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$src_dir"

cmd_warnings() {
    local b=/tmp/kajo-audit-warn
    rm -rf "$b"
    CFLAGS="-Wshadow -Wformat=2 -Wnull-dereference -Wduplicated-cond -Wlogical-op -Wcast-qual -Wstrict-prototypes" \
        meson setup "$b" "$src_dir" >/dev/null
    echo "== extra warnings by flag (system headers and generated protocol code excluded)"
    ninja -C "$b" 2>&1 | grep -E "warning:" | grep -v "^/usr/" | grep -v "protocol" \
        | sed -E 's/.*\[(-W[^]]+)\].*/\1/' | sort | uniq -c | sort -rn
    echo "== top files"
    ninja -C "$b" -t clean >/dev/null; ninja -C "$b" 2>&1 | grep -E "warning:" | grep -v "/usr/" \
        | sed -E 's#^(\.\./)*##; s#:[0-9]+:[0-9]+:.*##' | sort | uniq -c | sort -rn | head -10
    rm -rf "$b"
}

cmd_tidy() {
    command -v clang-tidy >/dev/null || { echo "clang-tidy not installed" >&2; exit 1; }
    local b=/tmp/kajo-audit-clang
    rm -rf "$b"
    CC=clang meson setup "$b" "$src_dir" >/dev/null
    ninja -C "$b" >/dev/null 2>&1 || true   # generates protocol headers / resources
    echo "== clang-analyzer findings (insecureAPI noise from glib headers disabled)"
    local out
    out="$(find src tests -name '*.c' ! -path '*vendor*' -print0 \
        | xargs -0 -P "$(nproc)" -n 1 clang-tidy -p "$b" --quiet \
            --checks='-*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.*,-clang-analyzer-optin.performance.Padding,bugprone-sizeof-expression,bugprone-suspicious-string-compare,bugprone-not-null-terminated-result' \
            2>/dev/null | grep -E "warning:" | grep -v "^/usr/" | sed 's#\.\./\.\./.*/Kajo/##' | sort -u || true)"
    if [ -n "$out" ]; then
        echo "$out"
        echo "-- $(echo "$out" | wc -l) finding(s). Known false positives: g_autofree not modelled (leak), flag-enum casts"
        echo "   (EnumCastOutOfRange), sizeof(*ptr) in g_new0. Confirm each by reading the code before changing it."
    else
        echo "clean"
    fi
    rm -rf "$b"
}

cmd_sizes() {
    python3 - <<'EOF'
import glob, re
funcs = []
for f in glob.glob('src/**/*.c', recursive=True):
    if 'vendor' in f: continue
    L = open(f).read().split('\n'); i = 0
    while i < len(L):
        line = L[i]
        if re.match(r'^[A-Za-z_][\w\s\*]*\(', line) and not line.rstrip().endswith(';') \
           and not line.startswith(('typedef', 'return', 'if', 'else', 'static const', 'const ')):
            k, ok = i, False
            while k < len(L) and k < i + 15:
                if L[k].rstrip().endswith(';') and '{' not in L[k]: break
                if '{' in L[k]: ok = True; break
                k += 1
            if ok:
                name = re.findall(r'([A-Za-z_]\w*)\s*\(', line)[0]
                e = k
                while e < len(L) and not L[e].startswith('}'): e += 1
                funcs.append((e - i + 1, f, name)); i = e + 1; continue
        i += 1
funcs.sort(reverse=True)
print("== longest functions (heuristic; > 150 lines are split candidates)")
for n, f, name in funcs[:15]: print(f"{n:5d}  {f}:{name}")
print(f"functions > 100 lines: {sum(1 for x in funcs if x[0] > 100)}, > 300: {sum(1 for x in funcs if x[0] > 300)}")
files = sorted(((sum(1 for _ in open(f)), f) for f in glob.glob('src/**/*.[ch]', recursive=True)
                if 'vendor' not in f and 'emoji_dataset' not in f), reverse=True)
print("== largest files (> 1500 lines are split candidates)")
for n, f in files[:10]: print(f"{n:5d}  {f}")
EOF
}

cmd_dups() {
    python3 - <<'EOF'
import glob, re, hashlib, collections
W = 8
idx = collections.defaultdict(list)
for f in glob.glob('src/**/*.c', recursive=True):
    if 'vendor' in f: continue
    norm = []
    for n, l in enumerate(open(f).read().split('\n')):
        s = re.sub(r'\s+', ' ', l.strip())
        if not s or s in ('{', '}', '};', '});') or s.startswith(('//', '/*', '*', '#include')): continue
        norm.append((n + 1, s))
    for i in range(len(norm) - W + 1):
        h = hashlib.md5('\n'.join(x[1] for x in norm[i:i + W]).encode()).hexdigest()
        idx[h].append((f, norm[i][0]))
pairs = collections.Counter()
for locs in idx.values():
    if len(locs) < 2: continue
    fs = sorted(set(l[0] for l in locs))
    pairs[tuple(fs) if len(fs) > 1 else (fs[0] + ' (self)',)] += 1
print(f"== duplicated {W}-line windows: {sum(1 for l in idx.values() if len(l) > 1)}")
for k, v in pairs.most_common(15): print(f"{v:4d}  {' | '.join(k)}")
EOF
}

cmd_blocking() {
    echo "== synchronous calls that block the GTK main loop (count per file)"
    grep -rnoE "g_dbus_(connection|proxy)_call_sync|g_dbus_proxy_new_for_bus_sync|g_bus_get_sync|g_spawn_sync|g_spawn_command_line_sync|g_subprocess_communicate(_utf8)?\b[^_]|system *\(|popen *\(" src \
        | awk -F: '{print $1}' | sort | uniq -c | sort -rn
}

cmd_lifetimes() {
    python3 - <<'EOF'
import re, glob
pat = re.compile(r'\b(g_dbus_proxy_call|g_dbus_connection_call|g_file_\w+_async|g_mount_\w+_with_operation|g_volume_mount|gtk_file_dialog_\w+)\s*\(')
print("== async calls with a NULL cancellable and a raw user_data (use-after-free risk if the owner can die first)")
for f in sorted(glob.glob('src/**/*.c', recursive=True)):
    s = open(f).read()
    for m in pat.finditer(s):
        i, d = m.end(), 1
        while i < len(s) and d:
            d += (s[i] == '(') - (s[i] == ')'); i += 1
        parts, cur, dep = [], '', 0
        for c in s[m.end():i - 1]:
            if c in '([{': dep += 1
            if c in ')]}': dep -= 1
            if c == ',' and dep == 0: parts.append(cur.strip()); cur = ''
            else: cur += c
        parts.append(cur.strip())
        if len(parts) >= 3 and parts[-3] == 'NULL' and 'NULL' not in parts[-2] and not re.search(r'weak|g_object_ref|ctx|job', parts[-1]):
            print(f"{f}:{s[:m.start()].count(chr(10)) + 1}: {m.group(1)} ud={' '.join(parts[-1].split())}")
EOF
}

cmd_colors() {
    echo "== raw hex colours in data/defaults/style.css (token roles: @metro-*; fold repeats into tokens)"
    grep -v '^@define-color' data/defaults/style.css | grep -oiE '#[0-9a-f]{6}\b' | tr 'A-F' 'a-f' \
        | sort | uniq -c | sort -rn | head -20
}

cmd_comp() {
    local bad=0
    echo "== kajo-comp: forbidden calls in src/comp (spec §7.2.1, §7.2.3)"
    # waitpid is only allowed with WNOHANG; sleeps and synchronous subprocess helpers never
    if grep -rnE "\b(assert|abort|g_error|g_spawn_sync|g_spawn_command_line_sync|system|popen|sleep|usleep|nanosleep)[[:space:]]*\(" \
            src/comp --include='*.c' --include='*.h'; then bad=1; fi
    if grep -rnE "\bwaitpid[[:space:]]*\(" src/comp --include='*.c' | grep -v WNOHANG; then bad=1; fi
    echo "== kajo-comp: src/comp/core must not include wlroots, Vulkan or GLib (spec §7.2.4)"
    if grep -rnE "#include[[:space:]]*<(wlr/|vulkan/|glib|gio/|wayland-)" src/comp/core; then bad=1; fi
    echo "== kajo-comp: wlr_kfx_* and WLR_HAS_KFX restricted to src/comp/fx/ (spec §15 CB1, CB2)"
    if rg -n "wlr_kfx_|WLR_HAS_KFX" src/comp --glob '!src/comp/fx/**'; then bad=1; fi
    [ "$bad" = 0 ] && echo "ok"
    return "$bad"
}

case "${1:-all}" in
    warnings)  cmd_warnings ;;
    tidy)      cmd_tidy ;;
    sizes)     cmd_sizes ;;
    dups)      cmd_dups ;;
    blocking)  cmd_blocking ;;
    lifetimes) cmd_lifetimes ;;
    colors)    cmd_colors ;;
    comp)      cmd_comp ;;
    all)       cmd_sizes; cmd_dups; cmd_blocking; cmd_lifetimes; cmd_colors; cmd_comp; cmd_warnings; cmd_tidy ;;
    *) sed -n '2,17p' "$0"; exit 2 ;;
esac
