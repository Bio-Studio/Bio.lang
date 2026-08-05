#include "bio.h"

/* 词法 */
const char *KW[] = { "program","Stream","Class","Main","need","value","function",
                            "void","overwrite","ALL","res","ref","cause","new",
                            "const","thread",
                            "if","else","while","for","break","continue", NULL };

int is_kw(const char *s) {
    for (int i = 0; KW[i]; i++) if (strcmp(KW[i], s) == 0) return 1;
    return 0;
}

Tok *tokenize(const char *src, int *ntok) {
    Tok *toks = aalloc(sizeof(Tok) * 4096);
    int n = 0;
    const char *p = src;
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        /* 字符串 */
        if (*p == '"') {
            p++; char buf[512]; int bi = 0;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) { p++; }
                if (bi < 511) buf[bi++] = *p;
                p++;
            }
            if (*p == '"') p++;
            buf[bi] = 0;
            toks[n].kind = T_STR; toks[n].text = astrdup(buf); n++;
            continue;
        }
        /* 数字 */
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            char *end;
            toks[n].kind = T_NUM; toks[n].num = strtod(p, &end); p = end; n++;
            continue;
        }
        /* 标识符 / 关键字 */
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            char buf[128]; size_t len = p - start;
            if (len < 128) { memcpy(buf, start, len); buf[len] = 0; }
            toks[n].kind = is_kw(buf) ? T_KW : T_ID;
            toks[n].text = astrdup(buf); n++;
            continue;
        }
        /* 运算符 */
        if (p[0] == ':' && p[1] == ':') { toks[n].kind = T_OP; toks[n].text = "::"; p += 2; n++; continue; }
        if ((p[0] == '=' && p[1] == '=') || (p[0] == '!' && p[1] == '=') ||
            (p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '=') ||
            ((p[0] == '+' || p[0] == '-' || p[0] == '*' || p[0] == '/' || p[0] == '%') && p[1] == '=')) {
            char b[3] = { p[0], p[1], 0 };
            toks[n].kind = T_OP; toks[n].text = astrdup(b); p += 2; n++; continue;
        }
        if (strchr("+-*/=;,.(){}[]<>&", *p)) {
            char b[2] = { *p, 0 };
            toks[n].kind = T_OP; toks[n].text = astrdup(b); p++; n++; continue;
        }
        fprintf(stderr, "词法错误: 无法识别的字符 %c\n", *p);
        exit(1);
    }
    toks[n].kind = T_EOF; toks[n].text = ""; n++;
    *ntok = n;
    return toks;
}

