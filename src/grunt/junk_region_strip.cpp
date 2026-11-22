/// @file        junk_region_strip.cpp
/// @brief       残留 `junk_region_strip` 的三段实现：SCAN / REWRITE / PROOF。
///
/// 判据等级：S2 结构事实（不可达字节段对一切执行路径不可见，JVM 校验器同款
/// 语义的可达性走查）。Scan 纯读：逐类开只读会话，方法面直接看 backing 的
/// Code 属性原始字节（不 promote——IR 层在 0xff 上整方法解码失败，正是本
/// 残留存在的理由）；走查器见下。站点用 (methodIndex, codeSlot) 定位，死区
/// 段在 scan 定死。
///
/// Rewrite 纯写：逐类开会话 → decodeAttr(Code) → 按段把 `code` 向量原位写
/// 0x00（长度保持，嵌套属性与异常表不动）→ slot 置 Modified → `writeClass`
/// → 回写容器。无 promote/lower，无帧重算面。
///
/// Proof：类数不增 + 脏类逐个可重新解析（与 flags_strip 同一口径）。
#include "grunt/junk_region_strip.hpp"

#include <unordered_map>
#include <utility>

#include "jbc/core/attribute.hpp"
#include "jbc/core/writer.hpp"

#include "grunt/container.hpp"

namespace grunt::junk_region_strip {

namespace {

constexpr ca::u8 kNop = 0x00;

/// @brief 单操作码长度表（字节含操作码本身）；表外操作码返回 0 = fail-closed。
///        完整 JVM 表（JVMS 6.5）；变长族（switch/wide）由走查器特判。
ca::u8 insnLength(ca::u8 op) noexcept
{
    if (op <= 0x0f) return 1;                       // nop..dconst_1
    if (op == 0x10 || op == 0x12) return 2;         // bipush / ldc
    if (op == 0x11 || op == 0x13 || op == 0x14) return 3;  // sipush / ldc_w / ldc2_w
    if (op >= 0x15 && op <= 0x19) return 2;         // xload
    if (op >= 0x1a && op <= 0x2d) return 1;         // xload_n
    if (op >= 0x2e && op <= 0x35) return 1;         // xaload
    if (op >= 0x36 && op <= 0x3a) return 2;         // xstore
    if (op >= 0x3b && op <= 0x4e) return 1;         // xstore_n
    if (op >= 0x4f && op <= 0x56) return 1;         // xastore
    if (op >= 0x57 && op <= 0x83) return 1;         // pop..lxor
    if (op == 0x84) return 3;                       // iinc
    if (op >= 0x85 && op <= 0x98) return 1;         // i2l..dcmpg
    if (op >= 0x99 && op <= 0xa4) return 3;         // ifeq..if_icmple
    if (op == 0xa5 || op == 0xa6) return 3;         // if_acmpeq/ne
    if (op == 0xa7 || op == 0xa8 || op == 0xa9) return 3;  // goto / jsr / ret
    if (op == 0xaa || op == 0xab) return 0;         // switch：变长，特判
    if (op >= 0xac && op <= 0xb1) return 1;         // xreturn
    if (op >= 0xb2 && op <= 0xb8) return 3;         // getstatic..invokestatic
    if (op == 0xb9 || op == 0xba) return 5;         // invokeinterface / invokedynamic
    if (op == 0xbb) return 3;                       // new
    if (op == 0xbc) return 2;                       // newarray
    if (op == 0xbd || op == 0xc0 || op == 0xc1) return 3;  // anewarray / checkcast / instanceof
    if (op >= 0xbe && op <= 0xbf) return 1;         // arraylength / athrow
    if (op >= 0xc2 && op <= 0xc3) return 1;         // monitor
    if (op == 0xc4) return 0;                       // wide：特判
    if (op == 0xc5) return 4;                       // multianewarray
    if (op == 0xc6 || op == 0xc7) return 3;         // ifnull / ifnonnull
    if (op == 0xc8 || op == 0xc9) return 5;         // goto_w / jsr_w
    return 0;                                       // 0xca+ 表外：fail-closed
}

/// @brief 走查上下文：code 字节 + 可达标记。
///
/// visited 按字节标（死区段计算用）；starts 按指令头标（worklist 去重用）。
/// 分支目标落在已访字节但不是任何指令头 = 语义非法（JVM 校验器同样拒绝），
/// fail-closed——只按 visited 去重会把 goto 的操作数字节误当指令头吞掉目标。
struct Walk
{
    using pc_t = ca::u32;

    const std::vector<ca::u8>& code;
    std::vector<bool>          visited;
    std::vector<bool>          starts;

    bool mark(pc_t pc, ca::u32 len) noexcept
    {
        if (static_cast<ca::usize>(pc) + len > code.size()) return false;
        for (ca::u32 i = 0; i < len; ++i) visited[pc + i] = true;
        starts[pc] = true;
        return true;
    }

    /// @brief 读 s2/s4 有符号跳转偏移。
    ca::i32 branchOff(pc_t pc, ca::u32 bytes) const noexcept
    {
        if (bytes == 2) {
            return static_cast<ca::i32>(
                static_cast<ca::i16>((code[pc] << 8) | code[pc + 1]));
        }
        ca::u32 u = (static_cast<ca::u32>(code[pc]) << 24)
                    | (static_cast<ca::u32>(code[pc + 1]) << 16)
                    | (static_cast<ca::u32>(code[pc + 2]) << 8)
                    | static_cast<ca::u32>(code[pc + 3]);
        return static_cast<ca::i32>(u);
    }
};

/// @brief 从一个可达 pc 展开走查。@return false = fail-closed（可达路径解不开）。
bool walkFrom(Walk& w, Walk::pc_t start)
{
    std::vector<Walk::pc_t> work{start};
    while (!work.empty()) {
        const Walk::pc_t pc = work.back();
        work.pop_back();
        if (pc >= w.code.size()) return false;
        if (w.starts[pc]) continue;              // 已解码的指令头
        if (w.visited[pc]) return false;         // 落在别的指令体内：非法字节码

        const ca::u8 op = w.code[pc];
        if (op == 0xa8 || op == 0xa9 || op == 0xc9) return false;   // jsr/ret/jsr_w：动态返回地址
        if (op == 0xc4) {                                           // wide
            if (pc + 4 > w.code.size()) return false;
            const ca::u8  sub = w.code[pc + 1];
            const ca::u32 len = (sub == 0x84) ? 6 : 4;
            if (!w.mark(pc, len)) return false;
            if (sub == 0xa9) return false;                          // wide ret
            if (pc + len < w.code.size()) work.push_back(pc + len);
            continue;
        }
        if (op == 0xaa || op == 0xab) {                             // table/lookupswitch
            // 4 字节对齐后（JVMS 6.5）：default 在 q；tableswitch 的 low/high
            // 在 q+4/q+8、case 偏移表从 q+12 起（各 4 字节）；lookupswitch 的
            // npairs 在 q+4、(match, offset) 对从 q+8 起（各 8 字节）。
            ca::usize q = pc + 1;
            const ca::usize pad = (4 - (q % 4)) % 4;
            q += pad;
            if (q + 8 > w.code.size()) return false;
            const Walk::pc_t def = pc + static_cast<ca::u32>(w.branchOff(q, 4));
            ca::usize after;
            if (op == 0xaa) {
                if (q + 12 > w.code.size()) return false;
                const Walk::pc_t lo = (static_cast<Walk::pc_t>(w.code[q + 4]) << 24)
                                      | (static_cast<Walk::pc_t>(w.code[q + 5]) << 16)
                                      | (static_cast<Walk::pc_t>(w.code[q + 6]) << 8)
                                      | w.code[q + 7];
                const Walk::pc_t hi = (static_cast<Walk::pc_t>(w.code[q + 8]) << 24)
                                      | (static_cast<Walk::pc_t>(w.code[q + 9]) << 16)
                                      | (static_cast<Walk::pc_t>(w.code[q + 10]) << 8)
                                      | w.code[q + 11];
                if (lo > hi) return false;
                const ca::u64 cnt = static_cast<ca::u64>(hi) - lo + 1;
                after = q + 12 + cnt * 4;
            } else {
                const ca::i32 npairs = w.branchOff(q + 4, 4);
                if (npairs < 0) return false;
                after = q + 8 + static_cast<ca::usize>(npairs) * 8;
            }
            if (after > w.code.size()) return false;
            if (def >= w.code.size()) return false;
            work.push_back(def);
            for (ca::usize t = (op == 0xaa ? q + 12 : q + 8); t + 4 <= after;) {
                // lookupswitch 的偏移在对的后半（t+4），tableswitch 在 t。
                const ca::usize offPos = (op == 0xaa) ? t : t + 4;
                const Walk::pc_t target
                    = pc + static_cast<ca::u32>(w.branchOff(offPos, 4));
                if (target >= w.code.size()) return false;
                work.push_back(target);
                t += (op == 0xaa) ? 4 : 8;
            }
            if (!w.mark(pc, static_cast<ca::u32>(after - pc))) return false;
            if (after < w.code.size()) work.push_back(after);
            continue;
        }

        const ca::u8 len = insnLength(op);
        if (len == 0) return false;                                 // 表外操作码
        if (!w.mark(pc, len)) return false;                         // 越界指令

        // 后继展开：分支双目标、无条件跳转/返回无直落，其余直落。
        const bool isGoto   = (op == 0xa7 || op == 0xc8);
        const bool isReturn = (op >= 0xac && op <= 0xb1) || op == 0xbf;
        const bool isBranch = (op >= 0x99 && op <= 0xa6) || op == 0xc6 || op == 0xc7;
        if (isBranch) {
            const Walk::pc_t target = pc + static_cast<ca::u32>(w.branchOff(pc + 1, 2));
            if (target >= w.code.size()) return false;
            work.push_back(target);
            work.push_back(pc + len);
        } else if (isGoto) {
            const ca::u32 offBytes = (op == 0xa7) ? 2 : 4;
            const Walk::pc_t target = pc + static_cast<ca::u32>(w.branchOff(pc + 1, offBytes));
            if (target >= w.code.size()) return false;
            work.push_back(target);
        } else if (!isReturn) {
            if (pc + len < w.code.size()) work.push_back(pc + len);
        }
    }
    return true;
}

/// @brief 一份 Code 字节上的死区段（可达集合之外的最大连续字节段）。
/// @return false = fail-closed（可达路径解不开，方法整条不动）。
bool junkRanges(const std::vector<ca::u8>& code, const jbc::core::CodeAttr& body,
                std::vector<std::pair<ca::u32, ca::u32>>& out)
{
    if (code.empty()) return true;
    Walk w{code, std::vector<bool>(code.size(), false),
           std::vector<bool>(code.size(), false)};
    if (!walkFrom(w, 0)) return false;
    for (const jbc::core::CodeAttr::ExcEntry& e : body.excTable) {
        if (e.handlerPc >= code.size()) return false;
        if (!walkFrom(w, e.handlerPc)) return false;
    }
    ca::u32 run = 0;
    for (ca::u32 i = 0; i < code.size(); ++i) {
        if (w.visited[i]) {
            if (run != 0) {
                out.emplace_back(i - run, run);
                run = 0;
            }
            continue;
        }
        ++run;
    }
    if (run != 0) out.emplace_back(static_cast<ca::u32>(code.size()) - run, run);
    return true;
}

/// @brief StackMapTable 帧锚面：帧声明 pc 的字节标记。
///
/// 死区 NOP 化的帧一致性问题：JVM 校验器把 SMT 帧声明当类型状态锚点，锚
/// 状态沿 NOP 直落传播——死区尾的 `athrow` 是状态终止符（消费栈顶异常并
/// 终止直落），NOP 掉它会让锚状态泄进可达区的 merge 点（真实样本实证：
/// VerifyError "Instruction type does not match stack map"）。判据：
///   - 段内无帧锚 → 全段 NOP 安全（校验器不关心无锚字节）；
///   - 段内有帧锚 → 段尾必须保留 `athrow`（NOP 化收缩 1 字节）；
///   - 段内有帧锚且段尾不是 `athrow` → 整方法 fail-closed（锚会泄进可达
///     区，不猜）。
/// SMT 属性解码失败 = 无法知道锚在哪 → 整方法 fail-closed（宁留不猜）。
bool frameAnchors(jbc::core::ClassFile& cf, jbc::core::CodeAttr& body,
                  std::vector<bool>& hasFrame)
{
    using jbc::core::StackMapTableAttr;
    for (jbc::core::AttrSlot& slot : body.attrs) {
        if (cf.utf8(slot.nameCp) != "StackMapTable") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        const auto* smt = static_cast<const StackMapTableAttr*>(d.unwrap());
        ca::u32         prev = static_cast<ca::u32>(-1);   /// 首帧 delta 即 pc（JVMS 4.7.4）。
        for (const StackMapTableAttr::Frame& f : smt->frames) {
            ca::u32 delta = 0;
            if (f.type <= StackMapTableAttr::Frame::kSameMax) {
                delta = f.type;
            } else if (f.type >= StackMapTableAttr::Frame::kSame1Base
                       && f.type <= StackMapTableAttr::Frame::kSame1Max) {
                delta = f.type - StackMapTableAttr::Frame::kSame1Base;
            } else if (f.type == StackMapTableAttr::Frame::kSame1Ext
                       || (f.type >= StackMapTableAttr::Frame::kChopMax
                           && f.type <= StackMapTableAttr::Frame::kFull)) {
                delta = f.offset;
            } else {
                return false;   /// 128..246 保留标签：不猜。
            }
            const ca::u32 pc = prev + delta + 1;   /// smtAbsPc 语义（wrap 起点自洽）。
            if (static_cast<ca::usize>(pc) < hasFrame.size()) hasFrame[pc] = true;
            prev = pc;
        }
    }
    return true;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        jbc::core::ClassFile& cf = session->backing();
        for (ca::usize mi = 0; mi < cf.methods.size(); ++mi) {
            jbc::core::MemberInfo&    m     = cf.methods[mi];
            std::vector<jbc::core::AttrSlot>& attrs = m.attrs;
            ca::usize codeSlot = static_cast<ca::usize>(-1);
            for (ca::usize i = 0; i < attrs.size(); ++i) {
                if (cf.utf8(attrs[i].nameCp) == "Code") {
                    codeSlot = i;
                    break;
                }
            }
            if (codeSlot == static_cast<ca::usize>(-1)) continue;
            auto d = cf.decodeAttr(attrs[codeSlot]);
            if (d.is_err()) {
                ++result.methodsDecodeFail;
                continue;
            }
            auto& body = static_cast<jbc::core::CodeAttr&>(*d.unwrap());

            std::vector<std::pair<ca::u32, ca::u32>> ranges;
            if (!junkRanges(body.code, body, ranges)) {
                ++result.methodsFailClosed;
                continue;
            }

            // 帧锚保守面：SMT 帧声明的类型状态沿 NOP 直落传播，死区尾的
            // athrow 是终止符。段内有锚 → 段尾必须保留 athrow（段收缩 1 字
            // 节）；段尾不是 athrow 或 SMT 解不开 → 方法整条 fail-closed。
            std::vector<bool> hasFrame(body.code.size(), false);
            if (!frameAnchors(cf, body, hasFrame)) {
                ++result.methodsFailClosed;
                continue;
            }
            bool          failClosed = false;
            ca::u32       total      = 0;
            std::vector<std::pair<ca::u32, ca::u32>> keep;
            std::vector<ca::u32>                      keptTails;
            keep.reserve(ranges.size());
            for (const auto& r : ranges) {
                bool anchored = false;
                for (ca::u32 i = 0; i < r.second; ++i) {
                    if (hasFrame[r.first + i]) {
                        anchored = true;
                        break;
                    }
                }
                if (!anchored) {
                    total += r.second;
                    keep.push_back(r);
                    continue;
                }
                const ca::u32 tailPc = r.first + r.second - 1;
                if (body.code[static_cast<ca::usize>(tailPc)] != 0xbf) {
                    failClosed = true;
                    break;
                }
                auto shrunk = r;
                shrunk.second -= 1;   // 段尾 athrow 保留（终止符），段内 NOP 化
                keptTails.push_back(tailPc);
                if (shrunk.second != 0) {
                    total += shrunk.second;
                    keep.push_back(shrunk);
                }
            }
            if (failClosed) {
                ++result.methodsFailClosed;
                continue;
            }
            if (total == 0) continue;
            result.junkBytes += total;
            result.sites.push_back(JunkSite{std::string(entry.uri), mi, codeSlot,
                                            std::move(keep), std::move(keptTails)});
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    // 按 uri 分组逐类写回（与 flags_strip 同款两遍式，防整 jar 驻留）。
    std::unordered_map<std::string, std::vector<const JunkSite*>> byUri;
    for (const JunkSite& site : result.sites) byUri[site.uri].push_back(&site);

    for (auto& [uri, sites] : byUri) {
        const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
        if (bytes == nullptr) continue;
        auto session = openSession(*bytes);
        if (!session) continue;
        jbc::core::ClassFile& cf = session->backing();

        bool changed = false;
        ca::usize methods = 0;
        ca::usize nopBytes = 0;
        for (const JunkSite* site : sites) {
            if (site->methodIndex >= cf.methods.size()
                || site->codeSlot >= cf.methods[site->methodIndex].attrs.size()) {
                ++dirty.sitesOutOfRange;
                continue;
            }
            jbc::core::AttrSlot& slot = cf.methods[site->methodIndex].attrs[site->codeSlot];
            if (cf.decodeAttr(slot).is_err()) continue;
            auto* bodyPtr = slot.mutate();   // 置 Modified：写回重编码本槽
            if (bodyPtr == nullptr) continue;
            auto& body = static_cast<jbc::core::CodeAttr&>(*bodyPtr);
            ca::usize methodBytes = 0;
            for (const auto& [start, len] : site->ranges) {
                if (static_cast<ca::usize>(start) + len > body.code.size()) continue;
                for (ca::u32 i = 0; i < len; ++i) body.code[start + i] = kNop;
                methodBytes += len;
            }
            if (methodBytes == 0) continue;
            nopBytes += methodBytes;
            ++methods;
            changed = true;
        }

        if (!changed) continue;
        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) continue;
        replaceClass(container, uri, std::move(encoded).unwrap());
        dirty.uris.push_back(uri);
        dirty.methods += methods;
        dirty.nopBytes += nopBytes;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    ProofResult result;
    if (countClasses(container) != classesBefore) {
        result.ok = false;
        result.failed.emplace_back("class-count");
    }
    for (const std::string& uri : dirty.uris) {
        bool found = false;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (found || entry.uri != uri) return;
            found = openSession(*entry.bytes) != nullptr;
        });
        if (!found) {
            result.ok = false;
            result.failed.push_back(uri);
        }
    }
    return result;
}

}   // namespace grunt::junk_region_strip
