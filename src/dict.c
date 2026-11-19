#include "dict.h"
#include <stdlib.h>
#include <string.h>

#define DICT_INIT_BUCKETS 128

static unsigned long djb2(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != 0) h = ((h << 5) + h) + (unsigned)c;
    return h;
}

void dict_init(Dict *d) {
    d->nbuckets = DICT_INIT_BUCKETS;
    d->count = 0;
    d->buckets = calloc(d->nbuckets, sizeof(*d->buckets));
}

/* 负载因子超 0.75 时翻倍重建 */
static int dict_rehash(Dict *d) {
    size_t nb = d->nbuckets * 2;
    DictNode **nbk = calloc(nb, sizeof(*nbk));
    if (!nbk) return -1;
    for (size_t i = 0; i < d->nbuckets; i++) {
        DictNode *n = d->buckets[i];
        while (n) {
            DictNode *next = n->next;
            size_t idx = djb2(n->key) % nb;
            n->next = nbk[idx];
            nbk[idx] = n;
            n = next;
        }
    }
    free(d->buckets);
    d->buckets = nbk;
    d->nbuckets = nb;
    return 0;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

int dict_set(Dict *d, const char *key, const char *data, size_t len) {
    size_t idx = djb2(key) % d->nbuckets;
    for (DictNode *n = d->buckets[idx]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) { /* 覆盖已有 key */
            char *nd = malloc(len);
            if (!nd) return -1;
            memcpy(nd, data, len);
            free(n->val.data);
            n->val.data = nd;
            n->val.len = len;
            return 0;
        }
    }
    DictNode *n = calloc(1, sizeof(*n));
    if (!n) return -1;
    n->key = dup_str(key);
    n->val.data = malloc(len);
    if (!n->key || !n->val.data) {
        free(n->key);
        free(n->val.data);
        free(n);
        return -1;
    }
    memcpy(n->val.data, data, len);
    n->val.len = len;
    n->next = d->buckets[idx];
    d->buckets[idx] = n;
    d->count++;
    if (d->count > d->nbuckets * 3 / 4) dict_rehash(d);
    return 0;
}

int dict_get(const Dict *d, const char *key, DbValue *out) {
    size_t idx = djb2(key) % d->nbuckets;
    for (DictNode *n = d->buckets[idx]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            if (out) {
                out->data = n->val.data;
                out->len = n->val.len;
            }
            return 0;
        }
    }
    return -1;
}

int dict_del(Dict *d, const char *key) {
    size_t idx = djb2(key) % d->nbuckets;
    DictNode **pp = &d->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            DictNode *victim = *pp;
            *pp = victim->next;
            free(victim->key);
            free(victim->val.data);
            free(victim);
            d->count--;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

void dict_free(Dict *d) {
    for (size_t i = 0; i < d->nbuckets; i++) {
        DictNode *n = d->buckets[i];
        while (n) {
            DictNode *next = n->next;
            free(n->key);
            free(n->val.data);
            free(n);
            n = next;
        }
    }
    free(d->buckets);
    d->buckets = NULL;
    d->nbuckets = d->count = 0;
}