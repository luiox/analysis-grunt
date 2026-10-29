/// @file        runtime_names.hpp
/// @brief       Grunt 家族知识：JDK runtime 常量名。
///
/// CONVENTION §2：家族知识唯一落点。这里钉的是「九大原始类型包装类的
/// `TYPE` 字段」——它们是 JVM 规范意义上的编译期常量引用：`Long.TYPE` 静态
/// 等价于 CONSTANT_Class 的原始类型条目，内联时可以落成 `ldc`。
#pragma once

#include <string_view>

namespace grunt::knowledge {

/// @brief `TYPE` 字段所在类 → 原始类型描述符字符；非持有类返回 `'\0'`。
///
/// `getstatic java/lang/Long.TYPE Ljava/lang/Class;` 的取值与运行环境无关，
/// JVMS §4.4.1 允许 CONSTANT_Class 引用原始类型描述符名，所以它能落成
/// `ldc (Class "J")`。`Boolean.TRUE` 这类堆对象常量不在表内——没有可静态
/// 编码的等价形式，调用方必须拒绝。
inline char primitiveTypeDesc(std::string_view owner) noexcept
{
    if (owner == "java/lang/Byte") return 'B';
    if (owner == "java/lang/Short") return 'S';
    if (owner == "java/lang/Integer") return 'I';
    if (owner == "java/lang/Long") return 'J';
    if (owner == "java/lang/Float") return 'F';
    if (owner == "java/lang/Double") return 'D';
    if (owner == "java/lang/Character") return 'C';
    if (owner == "java/lang/Boolean") return 'Z';
    if (owner == "java/lang/Void") return 'V';
    return '\0';
}

}   // namespace grunt::knowledge
