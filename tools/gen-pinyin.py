#!/usr/bin/env python3
"""生成「汉字 -> 无声调拼音」索引表（编进二进制，无运行时依赖）。

数据来源：ICU 的 Han-Latin 转写（本机 uconv 78.2），批量喂进去一次转完，
再用 Latin-ASCII 去掉声调符号。

    ./tools/gen-pinyin.py > src/pinyin_table.h

表结构：Basic CJK（U+4E00..U+9FFF）连续区间，每字 1 个 uint16 索引，
0 = 无拼音；索引 1..N 指向音节字符串表。查表 O(1)，21000 字只占 ~42 KB。
"""
import subprocess
import sys

START, END = 0x4E00, 0x9FFF
EXCEPTIONS = {0x3007: "ling"}   # 〇，不在基本区

def transliterate(cps):
    """一次 uconv 调用转完整区间；行数必须与输入对齐（否则报错退出）。"""
    text = "\n".join(chr(cp) for cp in cps) + "\n"
    out = subprocess.run(
        ["uconv", "-x", "Han-Latin; Latin-ASCII; Lower"],
        input=text.encode(), capture_output=True, check=True).stdout.decode()
    lines = out.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) != len(cps):
        sys.exit(f"行数不对齐：{len(lines)} != {len(cps)}（uconv 版本差异？）")
    return lines

def main():
    cps = list(range(START, END + 1))
    sylls = transliterate(cps)

    ids, table, index = {}, [], []
    for cp, s in zip(cps, sylls):
        s = "".join(ch for ch in s if ch.isalpha())
        if not s:
            index.append(0)
            continue
        if s not in ids:
            ids[s] = len(table) + 1
            table.append(s)
        index.append(ids[s])

    for cp, s in EXCEPTIONS.items():
        if s not in ids:
            ids[s] = len(table) + 1
            table.append(s)

    w = sys.stdout.write
    w("/* 由 tools/gen-pinyin.py 生成，勿手改。\n")
    w(" * 数据来源 ICU Han-Latin（去声调），覆盖 U+%04X..U+%04X。*/\n" % (START, END))
    w("#pragma once\n\n")
    w("#define UQ_PY_FIRST 0x%04X\n#define UQ_PY_LAST 0x%04X\n\n" % (START, END))
    w("static const char *const py_syll[] = {\n    \"\",\n")
    for i in range(0, len(table), 8):
        w("    " + ", ".join('"%s"' % s for s in table[i:i + 8]) + ",\n")
    w("};\n\n")
    w("static const unsigned short py_idx[%d] = {\n" % len(index))
    for i in range(0, len(index), 16):
        w("    " + ", ".join(str(v) for v in index[i:i + 16]) + ",\n")
    w("};\n\n")
    w("static const struct { unsigned int cp; unsigned short id; } py_except[] = {\n")
    for cp, s in EXCEPTIONS.items():
        w("    { 0x%04X, %d },\n" % (cp, ids[s]))
    w("};\n")
    sys.stderr.write(f"音节 {len(table)} 个，汉字 {len(cps)} 个，映射 {sum(1 for i in index if i)} 个\n")

main()
