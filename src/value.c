#include "bio.h"

/* 值 / 请求结果 */
Value *mk_num(double d) { Value *v = aalloc(sizeof(Value)); v->kind = V_NUM; v->num = d; return v; }

Value *mk_str(const char *s) { Value *v = aalloc(sizeof(Value)); v->kind = V_STR; v->str = s; return v; }

/* 字段默认值：string → 空串，[] → 空数组，数值/泛型 → 0 */
Value *field_default(const char *type) {
    if (type && strstr(type, "string")) return mk_str("");
    if (type && strstr(type, "[]")) return mk_arr(0);
    return mk_num(0);
}

Value *mk_refobj(const char *perm, const char *follow, const char *name) {
    Value *v = aalloc(sizeof(Value));
    v->kind = V_REF;
    v->ref_perm = perm;
    v->ref_follow = follow;
    v->ref_name = name;
    return v;
}

Value *mk_arr(int cap) {
    Value *v = aalloc(sizeof(Value));
    v->kind = V_ARR;
    v->cap = cap > 0 ? cap : 8;
    v->len = 0;
    v->head = 0;
    v->obj_fields = NULL;
    v->items = aalloc(sizeof(Value *) * (size_t)v->cap);
    return v;
}

Result *mk_res(Value *v) { Result *r = aalloc(sizeof(Result)); r->res = v; r->ref = NULL; return r; }

Result *mk_ref(const char *reason) { Result *r = aalloc(sizeof(Result)); r->res = NULL; r->ref = reason; return r; }

Value *mk_refval(const char *reason) { Value *w = aalloc(sizeof(Value)); w->kind = V_RES; w->res = mk_ref(reason); return w; }

int is_rejected(Value *v) { return v->kind == V_RES && v->res->ref != NULL; }

const char *reject_reason(Value *v) { return v->res->ref; }

int truthy(Value *v) {
    if (!v) return 0;
    if (v->kind == V_RES) return !v->res->ref && truthy(v->res->res);
    if (v->kind == V_NUM) return v->num != 0;
    return v->kind == V_STR && v->str[0] != 0;
}

void print_value(Value *v) {
    if (!v) { printf("nil"); return; }
    switch (v->kind) {
        case V_NUM:
            if (v->num == (double)(long)v->num) printf("%ld", (long)v->num);
            else printf("%g", v->num);
            break;
        case V_STR: printf("%s", v->str); break;
        case V_ARR:
            printf("[");
            for (int i = 0; i < v->len; i++) {
                if (i) printf(", ");
                print_value(v->items[i]);
            }
            printf("]");
            break;
        case V_REF:
            printf("&%s %s %s", v->ref_perm, v->ref_follow, v->ref_name);
            break;
        case V_OBJ:
            /* 数组对象（Array/Vector 类，data 字段=Solid 连续流）显示为数组 */
            if (v->obj_fields) {
                Value *d = var_get_layer(v->obj_fields, "data");
                if (d && d->kind == V_ARR) { print_value(d); break; }
            }
            printf("<对象 %s {", v->obj_cls);
            for (int i = 0; i < v->obj_fields->n; i++) {
                if (i) printf(", ");
                printf("%s: ", v->obj_fields->names[i]);
                print_value(v->obj_fields->vals[i]);
            }
            printf("}>");
            break;
        case V_RES:
            if (v->res->ref) printf("ref(%s)", v->res->ref);
            else { printf("res("); print_value(v->res->res); printf(")"); }
            break;
    }
}

