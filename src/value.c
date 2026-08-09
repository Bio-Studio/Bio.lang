#include "bio.h"

/* Values / request results */
Value *mk_num(double d) { Value *v = aalloc(sizeof(Value)); v->kind = V_NUM; v->num = d; return v; }

Value *mk_str(const char *s) { Value *v = aalloc(sizeof(Value)); v->kind = V_STR; v->str = s; return v; }

/* Stream reference (V_STREAM): streams are first-class values, passable as arguments */
Value *mk_streamref(Stream *s) { Value *v = aalloc(sizeof(Value)); v->kind = V_STREAM; v->stream_ref = s; return v; }

/* Field default value: string → "", [] → empty array, numeric/generic → 0 */
Value *field_default(const char *type) {
    if (type && strstr(type, "string")) return mk_str("");
    if (type && strstr(type, "[]")) return mk_arr(0);
    return mk_num(0);
}

RefTarget *ref_target_var(VarMap *map, const char *name) {
    RefTarget *t = aalloc(sizeof(RefTarget));
    t->kind = 0; t->map = map; t->name = name;
    return t;
}

RefTarget *ref_target_elem(Value *arr, int index) {
    RefTarget *t = aalloc(sizeof(RefTarget));
    t->kind = 1; t->arr = arr; t->index = index;
    return t;
}

RefTarget *ref_target_field(Value *obj, const char *name) {
    RefTarget *t = aalloc(sizeof(RefTarget));
    t->kind = 2; t->obj = obj; t->name = name;
    return t;
}

/* Underlying Solid storage of an array value (V_ARR itself, or an Array object's `data` field). */
static Value *ref_data_array(Value *arr) {
    if (!arr) return NULL;
    if (arr->kind == V_ARR) return arr;
    if ((arr->kind == V_OBJ || (arr->kind == V_ARR && arr->obj_fields)) && arr->obj_fields)
        return var_get_layer(arr->obj_fields, "data");
    return NULL;
}

Value *ref_read(RefTarget *t, const char **err) {
    if (err) *err = NULL;
    if (!t) { if (err) *err = "reference target is missing"; return NULL; }
    if (t->kind == 0) {
        Value *v = t->map ? var_get_layer(t->map, t->name) : NULL;
        if (!v) { if (err) *err = "reference target does not exist"; return NULL; }
        return v;
    }
    if (t->kind == 1) {
        Value *d = ref_data_array(t->arr);
        int i = t->index;
        if (!d || d->kind != V_ARR || i < 0 || d->head + i >= d->len) {
            if (err) *err = "reference pointer is out of bounds";
            return NULL;
        }
        return d->items[d->head + i];
    }
    if (t->kind == 2) {
        Value *v = t->obj && t->obj->obj_fields ? var_get_layer(t->obj->obj_fields, t->name) : NULL;
        if (!v) { if (err) *err = "reference target field does not exist"; return NULL; }
        return v;
    }
    if (err) *err = "unknown reference target";
    return NULL;
}

int ref_write(RefTarget *t, Value *v, const char **err) {
    if (err) *err = NULL;
    if (!t) { if (err) *err = "reference target is missing"; return -1; }
    if (t->kind == 0) {
        if (!t->map) { if (err) *err = "reference target does not exist"; return -1; }
        var_set(t->map, t->name, v);
        return 0;
    }
    if (t->kind == 1) {
        Value *d = ref_data_array(t->arr);
        int i = t->index;
        if (!d || d->kind != V_ARR || i < 0 || d->head + i >= d->len) {
            if (err) *err = "reference pointer is out of bounds";
            return -1;
        }
        d->items[d->head + i] = v;
        return 0;
    }
    if (t->kind == 2) {
        if (!t->obj || !t->obj->obj_fields) { if (err) *err = "reference target object is missing"; return -1; }
        var_set(t->obj->obj_fields, t->name, v);
        return 0;
    }
    if (err) *err = "unknown reference target";
    return -1;
}

int ref_move(RefTarget *t, int delta, const char **err) {
    if (err) *err = NULL;
    if (!t) { if (err) *err = "reference target is missing"; return -1; }
    if (t->kind != 1) {
        if (err) *err = "reference is not a moving pointer (only array-element references move with ++/--)";
        return -1;
    }
    Value *d = ref_data_array(t->arr);
    int ni = t->index + delta;
    if (!d || d->kind != V_ARR || ni < 0 || d->head + ni >= d->len) {
        if (err) *err = "reference pointer moved out of bounds";
        return -1;
    }
    t->index = ni;
    return 0;
}

Value *mk_refobj(const char *perm, const char *follow, RefTarget *tgt) {
    Value *v = aalloc(sizeof(Value));
    v->kind = V_REF;
    v->ref_perm = perm;
    v->ref_follow = follow;
    v->ref_tgt = tgt;
    return v;
}

Value *mk_arr(int cap) {
    Value *v = aalloc(sizeof(Value));
    v->kind = V_ARR;
    v->cap = cap > 0 ? cap : BIO_ARR_CAP;
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
    if (v->kind == V_RES) {
        if (v->res->ref) {
            /* A refusal is truthy only when it carries a real reason;
             * an empty refusal (nothing / "") is false. */
            return v->res->ref[0] != 0 && strcmp(v->res->ref, NOTHING) != 0;
        }
        Value *r = v->res->res;
        if (!r) return 0;
        if (r->kind == V_ARR || r->kind == V_OBJ || r->kind == V_STREAM) return 1;
        return truthy(r);
    }
    if (v->kind == V_NUM) return v->num != 0;
    if (v->kind == V_STREAM) return v->stream_ref != NULL;
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
            printf("&%s %s", v->ref_perm, v->ref_follow);
            if (v->ref_tgt) {
                if (v->ref_tgt->kind == 0) printf(" %s", v->ref_tgt->name);
                else if (v->ref_tgt->kind == 1) printf("[%d]", v->ref_tgt->index);
                else if (v->ref_tgt->kind == 2) printf(" .%s", v->ref_tgt->name);
            }
            break;
        case V_STREAM:
            printf("%s", v->stream_ref ? v->stream_ref->name : "(nil stream)");
            break;
        case V_OBJ:
            /* Array objects (Array/Vector classes, data field = Solid stream) display as arrays */
            if (v->obj_fields) {
                Value *d = var_get_layer(v->obj_fields, "data");
                if (d && d->kind == V_ARR) { print_value(d); break; }
            }
            printf("<object %s {", v->obj_cls);
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
