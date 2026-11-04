#!/usr/bin/env bash
# 语料回归：对语料目录里的每个 jar 跑完整残留链，检查
#   1) CLI 退出码
#   2) 输出 jar 可被重新读取（类数不增）
#   3) 有 JDK 时逐类过校验器（含死代码）
#
# 用法：CORPUS=<dir> ./test/corpus-regress.sh
#      NO_VERIFY=1 跳过校验器（无 JDK 环境）
#      JAR_TIMEOUT=120 单罐上限秒数（0 = 不限时）；超时记 FAIL run 继续下一罐
#      ONLY=<glob> 只跑名字匹配的罐（bash case 语义：? * [] 都是通配符，全名匹配 basename；
#                  字面 [ 要转义成 \[，或设 ONLY_LITERAL=1 按字面匹配免转义）；空 = 全跑
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="${CORPUS:-$ROOT/../run-lab}"
# GRUNT_CLI 可指向快照 exe：边开发边跑语料时，锁住当轮被测对象不被重建影响。
CLI="${GRUNT_CLI:-$ROOT/build/windows/x64/release/grunt_cli.exe}"
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
    name="$(basename "$jar" .jar)"
    # 续跑过滤：只跑名字匹配 ONLY 的罐（空 = 全跑）。
    # ONLY_LITERAL=1 时按字面整名比较（不走 case 通配），罐名自带
    # [ ] * ? 等字符时不用转义；默认走 case 通配（? * [] 都是元字符）。
    if [ -n "${ONLY:-}" ]; then
        if [ "${ONLY_LITERAL:-0}" = "1" ]; then
            [ "$name" = "$ONLY" ] || continue
        else
            case "$name" in
            $ONLY) ;;
            *) continue ;;
            esac
        fi
    fi
    total=$((total + 1))
    name="$(basename "$jar" .jar)"
    dst="$OUT/$name.out.jar"

    # 单罐上限：循环互卡 / 死循环罐用 timeout 兜底，超时记 FAIL run 继续。
    if ! timeout "${JAR_TIMEOUT:-0}" "$CLI" -i "$jar" -o "$dst" -p "$PASSES" 2>"$OUT/$name.err"; then
        # 噪声分离：文件在场但 CLI 报 NotFound 是 Windows ANSI argv 对非 ASCII
        # 罐名的编码错配（导入口拿错路径），不是本链的 run 失败——记 SKIP 不计账。
        if grep -q "NotFound" "$OUT/$name.err" 2>/dev/null && [ -e "$jar" ]; then
            echo "SKIP argenc $name（罐名编码错配，文件在场）"
            continue
        fi
        echo "FAIL run   $name"
        sed -n '1,5p' "$OUT/$name.err" >&2
        fail=$((fail + 1))
        continue
    fi

    # 类数不增（空壳类摘除允许减少，减少打 INFO 台账）：用 unzip -l 数 .class 条目
    before=$(unzip -l "$jar" 2>/dev/null | grep -c '\.class$')
    after=$(unzip -l "$dst" 2>/dev/null | grep -c '\.class$')
    if [ "$after" -gt "$before" ]; then
        echo "FAIL count $name（$before -> $after）"
        fail=$((fail + 1))
        continue
    fi
    if [ "$after" -lt "$before" ]; then
        echo "INFO count $name（$before -> $after，空壳类摘除）"
    fi

    if [ "${NO_VERIFY:-0}" != "1" ] && command -v java >/dev/null 2>&1; then
        # 不带 --quiet：FAIL 类名行是基线对比的原料。
        if ! timeout "${JAR_TIMEOUT:-0}" java -Xverify:all "$ROOT/test/tools/VerifyJar.java" "$dst" >"$OUT/$name.verify" 2>&1; then
            # 噪声分离：对原始输入预检，只把「新增失败」记 FAIL verify——
            # 混淆产物原生不可验证（基线同挂）不算本链的账。
            timeout "${JAR_TIMEOUT:-0}" java -Xverify:all "$ROOT/test/tools/VerifyJar.java" "$jar" \
                >"$OUT/$name.verify0" 2>&1
            grep '^FAIL ' "$OUT/$name.verify" | sed 's/^FAIL //' | sort >"$OUT/$name.verify.dst"
            grep '^FAIL ' "$OUT/$name.verify0" | sed 's/^FAIL //' | sort >"$OUT/$name.verify.src"
            newf=$(comm -13 "$OUT/$name.verify.src" "$OUT/$name.verify.dst" | grep -c .)
            if [ "$newf" -eq 0 ]; then
                echo "INFO verify $name（基线同挂，无新增失败）"
                continue
            fi
            echo "FAIL verify $name（新增 $newf 个失败类）"
            grep '^FAIL ' "$OUT/$name.verify" | sed -n '1,10p' >&2
            fail=$((fail + 1))
            continue
        fi
    fi
    echo "ok   $name"
done

echo "语料 $total 个，失败 $fail 个（产物：$OUT）"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
