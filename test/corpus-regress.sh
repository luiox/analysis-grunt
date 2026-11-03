#!/usr/bin/env bash
# 语料回归：对语料目录里的每个 jar 跑完整残留链，检查
#   1) CLI 退出码
#   2) 输出 jar 可被重新读取（类数不增）
#   3) 有 JDK 时逐类过校验器（含死代码）
#
# 用法：CORPUS=<dir> ./test/corpus-regress.sh
#      NO_VERIFY=1 跳过校验器（无 JDK 环境）
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="${CORPUS:-$ROOT/../run-lab}"
CLI="$ROOT/build/windows/x64/release/grunt_cli.exe"
[ -x "$CLI" ] || CLI="$ROOT/build/linux/x64/release/grunt_cli"
PASSES="${PASSES:-grunt.ConstantCall}"
OUT="${OUT:-$(mktemp -d)}"

if [ ! -x "$CLI" ]; then
    echo "找不到 CLI，先跑 xmake build grunt_cli" >&2
    exit 2
fi
# OUT 目录不自动创建时，stderr 重定向直接失败，所有 jar 都会伪装成 FAIL run。
mkdir -p "$OUT"
if [ ! -d "$CORPUS" ]; then
    echo "语料目录不存在：$CORPUS（用 CORPUS=<dir> 指定）" >&2
    exit 2
fi

fail=0
total=0
for jar in "$CORPUS"/*.jar; do
    [ -e "$jar" ] || continue
    total=$((total + 1))
    name="$(basename "$jar" .jar)"
    dst="$OUT/$name.out.jar"

    if ! "$CLI" -i "$jar" -o "$dst" -p "$PASSES" 2>"$OUT/$name.err"; then
        echo "FAIL run   $name"
        sed -n '1,5p' "$OUT/$name.err" >&2
        fail=$((fail + 1))
        continue
    fi

    # 类数不增：用 unzip -l 数 .class 条目
    before=$(unzip -l "$jar" 2>/dev/null | grep -c '\.class$')
    after=$(unzip -l "$dst" 2>/dev/null | grep -c '\.class$')
    if [ "$before" != "$after" ]; then
        echo "FAIL count $name（$before -> $after）"
        fail=$((fail + 1))
        continue
    fi

    if [ "${NO_VERIFY:-0}" != "1" ] && command -v java >/dev/null 2>&1; then
        if ! java -Xverify:all "$ROOT/test/tools/VerifyJar.java" "$dst" --quiet >"$OUT/$name.verify" 2>&1; then
            echo "FAIL verify $name"
            sed -n '1,10p' "$OUT/$name.verify" >&2
            fail=$((fail + 1))
            continue
        fi
    fi
    echo "ok   $name"
done

echo "语料 $total 个，失败 $fail 个（产物：$OUT）"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
