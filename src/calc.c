/* calc.c — 极简表达式求值（递归下降，无分配，无依赖）
 *
 * 支持：+ - * / % ^ 括号，一元负号，sqrt/abs/floor/ceil/round/
 *       sin/cos/tan/ln/log/exp，常量 pi/e/tau，数字可带下划线分组。
 */
#include "uquick.h"

#include <math.h>
#include <string.h>
#include <ctype.h>

typedef struct {
  const char *p;
  gboolean    ok;
} Calc;

static void skip_ws(Calc *c) {
  while (*c->p == ' ' || *c->p == '\t' || *c->p == ',') c->p++;
}

static double parse_expr(Calc *c);

static double parse_primary(Calc *c) {
  skip_ws(c);
  if (*c->p == '(') {
    c->p++;
    double v = parse_expr(c);
    skip_ws(c);
    if (*c->p == ')') c->p++;
    else c->ok = FALSE;
    return v;
  }
  if (isalpha((unsigned char)*c->p)) {
    const char *start = c->p;
    while (isalpha((unsigned char)*c->p)) c->p++;
    size_t len = (size_t)(c->p - start);
    char name[16];
    if (len >= sizeof(name)) { c->ok = FALSE; return 0; }
    memcpy(name, start, len);
    name[len] = 0;

    if (strcmp(name, "pi") == 0) return M_PI;
    if (strcmp(name, "e") == 0) return M_E;
    if (strcmp(name, "tau") == 0) return 2.0 * M_PI;

    skip_ws(c);
    if (*c->p != '(') { c->ok = FALSE; return 0; }
    c->p++;
    double a = parse_expr(c);
    skip_ws(c);
    if (*c->p == ')') c->p++; else c->ok = FALSE;

    if (strcmp(name, "sqrt") == 0) return sqrt(a);
    if (strcmp(name, "abs") == 0) return fabs(a);
    if (strcmp(name, "floor") == 0) return floor(a);
    if (strcmp(name, "ceil") == 0) return ceil(a);
    if (strcmp(name, "round") == 0) return round(a);
    if (strcmp(name, "sin") == 0) return sin(a);
    if (strcmp(name, "cos") == 0) return cos(a);
    if (strcmp(name, "tan") == 0) return tan(a);
    if (strcmp(name, "ln") == 0) return log(a);
    if (strcmp(name, "log") == 0) return log10(a);
    if (strcmp(name, "exp") == 0) return exp(a);
    c->ok = FALSE;
    return 0;
  }
  /* 数字：允许 1_000 与 1e9 */
  const char *s = c->p;
  gboolean any = FALSE;
  while (isdigit((unsigned char)*c->p) || *c->p == '_') {
    if (*c->p != '_') any = TRUE;
    c->p++;
  }
  if (*c->p == '.') {
    c->p++;
    while (isdigit((unsigned char)*c->p) || *c->p == '_') {
      if (*c->p != '_') any = TRUE;
      c->p++;
    }
  }
  if (!any) { c->ok = FALSE; return 0; }
  if (*c->p == 'e' || *c->p == 'E') {
    const char *save = c->p;
    c->p++;
    if (*c->p == '+' || *c->p == '-') c->p++;
    if (isdigit((unsigned char)*c->p)) {
      while (isdigit((unsigned char)*c->p)) c->p++;
    } else {
      c->p = save;
    }
  }
  char buf[64];
  size_t n = 0;
  for (const char *q = s; q < c->p && n < sizeof(buf) - 1; q++)
    if (*q != '_') buf[n++] = *q;
  buf[n] = 0;
  char *end = NULL;
  double v = g_ascii_strtod(buf, &end);
  if (!end || *end) { c->ok = FALSE; return 0; }
  return v;
}

static double parse_unary(Calc *c) {
  skip_ws(c);
  if (*c->p == '-') { c->p++; return -parse_unary(c); }
  if (*c->p == '+') { c->p++; return parse_unary(c); }
  return parse_primary(c);
}

static double parse_pow(Calc *c) {
  double base = parse_unary(c);
  skip_ws(c);
  if (*c->p == '^') { c->p++; return pow(base, parse_pow(c)); }
  return base;
}

static double parse_term(Calc *c) {
  double v = parse_pow(c);
  for (;;) {
    skip_ws(c);
    char op = *c->p;
    if (op != '*' && op != '/' && op != '%') break;
    c->p++;
    double r = parse_pow(c);
    if (op == '*') v *= r;
    else if (op == '/') { if (r == 0) { c->ok = FALSE; return 0; } v /= r; }
    else { if (r == 0) { c->ok = FALSE; return 0; } v = fmod(v, r); }
  }
  return v;
}

static double parse_expr(Calc *c) {
  double v = parse_term(c);
  for (;;) {
    skip_ws(c);
    char op = *c->p;
    if (op != '+' && op != '-') break;
    c->p++;
    double r = parse_term(c);
    v = (op == '+') ? v + r : v - r;
  }
  return v;
}

gboolean uq_calc_eval(const char *expr, double *out) {
  if (!expr || !*expr) return FALSE;
  Calc c = { expr, TRUE };
  double v = parse_expr(&c);
  skip_ws(&c);
  if (!c.ok || *c.p != 0) return FALSE;
  if (!isfinite(v)) return FALSE;
  *out = v;
  return TRUE;
}
