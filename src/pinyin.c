/* pinyin.c — 汉字转拼音（首字母 + 全拼），用编进二进制的查表实现
 *
 * 目的：让 "wyy" 能搜到「网易云音乐」、「sz」能搜到「设置」。
 * 表由 tools/gen-pinyin.py 从 ICU 的 Han-Latin 转写生成（见 pinyin_table.h），
 * 查表是 O(1) 数组索引，不依赖 ICU/libpinyin 等运行时库。
 *
 * 已知取舍：逐字取音，多音字用 ICU 的单字默认读音（乐→le、长→chang），
 * 所以「音乐」的全拼是 yinle 而不是 yinyue；首字母场景基本不受影响。
 */
#include "uquick.h"

#include "pinyin_table.h"

#include <string.h>

static const char *syllable_for(gunichar cp) {
  if (cp >= UQ_PY_FIRST && cp <= UQ_PY_LAST) {
    unsigned short id = py_idx[cp - UQ_PY_FIRST];
    if (id) return py_syll[id];
    return NULL;
  }
  for (gsize i = 0; i < G_N_ELEMENTS(py_except); i++)
    if (py_except[i].cp == cp) return py_syll[py_except[i].id];
  return NULL;
}

gboolean uq_pinyin(const char *utf8, GString *initials, GString *full) {
  gboolean found = FALSE;
  if (!utf8) return FALSE;
  for (const char *p = utf8; *p; p = g_utf8_next_char(p)) {
    gunichar cp = g_utf8_get_char(p);
    const char *s = syllable_for(cp);
    if (s) {
      found = TRUE;
      g_string_append_c(initials, s[0]);
      g_string_append(full, s);
    } else if (cp < 0x80) {
      char c = (char)cp;
      if (g_ascii_isalnum(c)) {
        c = (char)g_ascii_tolower(c);
        g_string_append_c(initials, c);
        g_string_append_c(full, c);
      } else if (c == ' ' || c == '-' || c == '_' || c == '.' || c == '/') {
        /* 分隔符保留，避免跨词粘连成假匹配 */
        g_string_append_c(initials, ' ');
        g_string_append_c(full, ' ');
      }
    } else {
      /* 非汉字的非 ASCII（全角标点、emoji 等）：当分隔符 */
      g_string_append_c(initials, ' ');
      g_string_append_c(full, ' ');
    }
  }
  return found;
}
