// SPDX-License-Identifier: Zlib
/*
 * TINYEXPR - Tiny recursive descent parser and evaluation engine in C
 *
 * Copyright (c) 2015-2020 Lewis Van Winkle
 *
 * http://CodePlea.com
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software
 * in a product, an acknowledgement in the product documentation would be
 * appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "tinyexpr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TE_NUL 0
#define TE_ADD 1
#define TE_SUB 2
#define TE_MUL 3
#define TE_DIV 4
#define TE_POW 5
#define TE_MOD 6

#define TE_FLAG_PURE 1

typedef struct state {
    const char *start;
    const char *next;
    int type;
    union {
        double value;
        const double *bound;
        const void *function;
    };
    void *context;

    const te_variable *lookup;
    int lookup_len;
} state;

#define TE_PURE_1(name) { #name, (const void*)(double(*)(double))name, TE_FLAG_PURE | 1, 0 }
#define TE_PURE_2(name) { #name, (const void*)(double(*)(double,double))name, TE_FLAG_PURE | 2, 0 }

static double pi(void) { return 3.14159265358979323846; }
static double e(void) { return 2.71828182845904523536; }
static double fac(double a) {
    if (a < 0.0 || a != floor(a)) return NAN;
    double res = 1.0;
    for (int i = 1; i <= (int)a; i++) res *= i;
    return res;
}

static const te_variable functions[] = {
    {"abs", (const void*)(double(*)(double))fabs, TE_FLAG_PURE | 1, 0},
    {"acos", (const void*)(double(*)(double))acos, TE_FLAG_PURE | 1, 0},
    {"asin", (const void*)(double(*)(double))asin, TE_FLAG_PURE | 1, 0},
    {"atan", (const void*)(double(*)(double))atan, TE_FLAG_PURE | 1, 0},
    {"atan2", (const void*)(double(*)(double,double))atan2, TE_FLAG_PURE | 2, 0},
    {"ceil", (const void*)(double(*)(double))ceil, TE_FLAG_PURE | 1, 0},
    {"cos", (const void*)(double(*)(double))cos, TE_FLAG_PURE | 1, 0},
    {"cosh", (const void*)(double(*)(double))cosh, TE_FLAG_PURE | 1, 0},
    {"e", (const void*)e, TE_FLAG_PURE | 0, 0},
    {"exp", (const void*)(double(*)(double))exp, TE_FLAG_PURE | 1, 0},
    {"fac", (const void*)fac, TE_FLAG_PURE | 1, 0},
    {"floor", (const void*)(double(*)(double))floor, TE_FLAG_PURE | 1, 0},
    {"ln", (const void*)(double(*)(double))log, TE_FLAG_PURE | 1, 0},
    {"log", (const void*)(double(*)(double))log10, TE_FLAG_PURE | 1, 0},
    {"log10", (const void*)(double(*)(double))log10, TE_FLAG_PURE | 1, 0},
    {"pi", (const void*)pi, TE_FLAG_PURE | 0, 0},
    {"pow", (const void*)(double(*)(double,double))pow, TE_FLAG_PURE | 2, 0},
    {"sin", (const void*)(double(*)(double))sin, TE_FLAG_PURE | 1, 0},
    {"sinh", (const void*)(double(*)(double))sinh, TE_FLAG_PURE | 1, 0},
    {"sqrt", (const void*)(double(*)(double))sqrt, TE_FLAG_PURE | 1, 0},
    {"tan", (const void*)(double(*)(double))tan, TE_FLAG_PURE | 1, 0},
    {"tanh", (const void*)(double(*)(double))tanh, TE_FLAG_PURE | 1, 0},
    {0, 0, 0, 0}
};

struct te_expr {
    int type;
    union {
        double value;
        const double *bound;
        const void *function;
    };
    void *context;
    te_expr *parameters[1];
};

static te_expr *new_expr(int type, void *context) {
    te_expr *ret = (te_expr*)malloc(sizeof(te_expr));
    if (!ret) return 0;
    ret->type = type;
    ret->value = 0;
    ret->context = context;
    ret->parameters[0] = 0;
    return ret;
}

static void next_token(state *s) {
    s->start = s->next;
    while (*s->next && (*s->next == ' ' || *s->next == '\t' || *s->next == '\n' || *s->next == '\r')) {
        s->next++;
    }
    s->start = s->next;

    if (!*s->next) {
        s->type = TE_NUL;
        return;
    }

    if ((*s->next >= '0' && *s->next <= '9') || *s->next == '.') {
        char *end;
        s->value = strtod(s->next, &end);
        s->next = end;
        s->type = 0;
        return;
    }

    if ((*s->next >= 'a' && *s->next <= 'z') || (*s->next >= 'A' && *s->next <= 'Z') || *s->next == '_') {
        while ((*s->next >= 'a' && *s->next <= 'z') || (*s->next >= 'A' && *s->next <= 'Z') || (*s->next >= '0' && *s->next <= '9') || *s->next == '_') {
            s->next++;
        }
        s->type = 0;
        return;
    }

    switch (*s->next++) {
        case '+': s->type = TE_ADD; break;
        case '-': s->type = TE_SUB; break;
        case '*': s->type = TE_MUL; break;
        case '/': s->type = TE_DIV; break;
        case '^': s->type = TE_POW; break;
        case '%': s->type = TE_MOD; break;
        case '(': s->type = '('; break;
        case ')': s->type = ')'; break;
        case ',': s->type = ','; break;
        default: s->type = -1; break;
    }
}

static te_expr *base(state *s);

static te_expr *factor(state *s) {
    te_expr *ret = base(s);
    while (s->type == TE_POW) {
        next_token(s);
        te_expr *b = base(s);
        te_expr *n = (te_expr*)malloc(sizeof(te_expr) + sizeof(te_expr*));
        if (!n) { te_free(ret); te_free(b); return 0; }
        n->type = TE_POW;
        n->parameters[0] = ret;
        n->parameters[1] = b;
        ret = n;
    }
    return ret;
}

static te_expr *term(state *s) {
    te_expr *ret = factor(s);
    while (s->type == TE_MUL || s->type == TE_DIV || s->type == TE_MOD) {
        int op = s->type;
        next_token(s);
        te_expr *b = factor(s);
        te_expr *n = (te_expr*)malloc(sizeof(te_expr) + sizeof(te_expr*));
        if (!n) { te_free(ret); te_free(b); return 0; }
        n->type = op;
        n->parameters[0] = ret;
        n->parameters[1] = b;
        ret = n;
    }
    return ret;
}

static te_expr *expr(state *s) {
    te_expr *ret = term(s);
    while (s->type == TE_ADD || s->type == TE_SUB) {
        int op = s->type;
        next_token(s);
        te_expr *b = term(s);
        te_expr *n = (te_expr*)malloc(sizeof(te_expr) + sizeof(te_expr*));
        if (!n) { te_free(ret); te_free(b); return 0; }
        n->type = op;
        n->parameters[0] = ret;
        n->parameters[1] = b;
        ret = n;
    }
    return ret;
}

static const te_variable *find_var(const state *s, const char *name, int len) {
    for (int i = 0; i < s->lookup_len; i++) {
        if (strncmp(s->lookup[i].name, name, len) == 0 && s->lookup[i].name[len] == '\0')
            return &s->lookup[i];
    }
    for (int i = 0; functions[i].name; i++) {
        if (strncmp(functions[i].name, name, len) == 0 && functions[i].name[len] == '\0')
            return &functions[i];
    }
    return 0;
}

static te_expr *base(state *s) {
    te_expr *ret = 0;

    if (s->type == TE_SUB) {
        next_token(s);
        te_expr *b = factor(s);
        ret = (te_expr*)malloc(sizeof(te_expr) + sizeof(te_expr*));
        if (!ret) { te_free(b); return 0; }
        ret->type = TE_SUB;
        ret->parameters[0] = new_expr(0, 0);
        ret->parameters[1] = b;
        return ret;
    }

    if (s->type == TE_ADD) {
        next_token(s);
        return factor(s);
    }

    if (s->type == '(') {
        next_token(s);
        ret = expr(s);
        if (s->type != ')') { te_free(ret); return 0; }
        next_token(s);
        return ret;
    }

    if (s->start != s->next && ((*s->start >= 'a' && *s->start <= 'z') || (*s->start >= 'A' && *s->start <= 'Z') || *s->start == '_')) {
        int len = (int)(s->next - s->start);
        const te_variable *var = find_var(s, s->start, len);
        if (!var) { return 0; }

        next_token(s);
        if (var->type & (TE_FLAG_PURE | 1 | 2)) {
            int arity = var->type & 0xF;
            if (s->type != '(') return 0;
            next_token(s);

            te_expr *n = (te_expr*)malloc(sizeof(te_expr) + sizeof(te_expr*) * (arity > 0 ? arity : 1));
            if (!n) return 0;
            n->type = var->type;
            n->function = var->address;

            for (int i = 0; i < arity; i++) {
                n->parameters[i] = expr(s);
                if (i < arity - 1) {
                    if (s->type != ',') { te_free(n); return 0; }
                    next_token(s);
                }
            }
            if (s->type != ')') { te_free(n); return 0; }
            next_token(s);
            return n;
        } else {
            ret = new_expr(var->type, var->context);
            if (!ret) return 0;
            ret->type = var->type;
            ret->bound = (const double*)var->address;
            return ret;
        }
    }

    if (s->start != s->next && ((*s->start >= '0' && *s->start <= '9') || *s->start == '.')) {
        ret = new_expr(0, 0);
        if (!ret) return 0;
        ret->value = s->value;
        next_token(s);
        return ret;
    }

    return 0;
}

te_expr *te_compile(const char *expression, const te_variable *variables, int var_count, int *error) {
    state s;
    s.start = expression;
    s.next = expression;
    s.lookup = variables;
    s.lookup_len = var_count;

    next_token(&s);
    te_expr *ret = expr(&s);

    if (s.type != TE_NUL) {
        if (error) *error = (int)(s.start - expression + 1);
        te_free(ret);
        return 0;
    }

    if (error) *error = 0;
    return ret;
}

double te_eval(const te_expr *n) {
    if (!n) return NAN;

    switch (n->type) {
        case 0: return n->value;
        case TE_ADD: return te_eval(n->parameters[0]) + te_eval(n->parameters[1]);
        case TE_SUB: return te_eval(n->parameters[0]) - te_eval(n->parameters[1]);
        case TE_MUL: return te_eval(n->parameters[0]) * te_eval(n->parameters[1]);
        case TE_DIV: return te_eval(n->parameters[0]) / te_eval(n->parameters[1]);
        case TE_POW: return pow(te_eval(n->parameters[0]), te_eval(n->parameters[1]));
        case TE_MOD: return fmod(te_eval(n->parameters[0]), te_eval(n->parameters[1]));
        default: break;
    }

    int arity = n->type & 0xF;
    if (arity == 0) {
        double (*f)(void) = (double(*)(void))n->function;
        return f();
    } else if (arity == 1) {
        double (*f)(double) = (double(*)(double))n->function;
        return f(te_eval(n->parameters[0]));
    } else if (arity == 2) {
        double (*f)(double, double) = (double(*)(double, double))n->function;
        return f(te_eval(n->parameters[0]), te_eval(n->parameters[1]));
    }

    return NAN;
}

void te_free(te_expr *n) {
    if (!n) return;
    int arity = n->type & 0xF;
    if (n->type == TE_ADD || n->type == TE_SUB || n->type == TE_MUL || n->type == TE_DIV || n->type == TE_POW || n->type == TE_MOD) {
        te_free(n->parameters[0]);
        te_free(n->parameters[1]);
    } else if (arity > 0) {
        for (int i = 0; i < arity; i++) {
            te_free(n->parameters[i]);
        }
    }
    free(n);
}

double te_interp(const char *expression, int *error) {
    te_expr *n = te_compile(expression, 0, 0, error);
    if (!n) return NAN;
    double ret = te_eval(n);
    te_free(n);
    return ret;
}
