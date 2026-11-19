#ifndef MINI_REDIS_DICT_H
#define MINI_REDIS_DICT_H

#include <stddef.h>

/* 极简哈希表：链地址法 + djb2，键 C 字符串，值二进制安全。 */

typedef struct {
    char *data;
    size_t len;
} DbValue;

typedef struct DictNode {
    char *key;
    DbValue val;
    struct DictNode *next;
} DictNode;

typedef struct {
    DictNode **buckets;
    size_t nbuckets;
    size_t count;
} Dict;

void dict_init(Dict *d);
int  dict_set(Dict *d, const char *key, const char *data, size_t len); /* 0 ok, -1 oom */
int  dict_get(const Dict *d, const char *key, DbValue *out);           /* 0 found, -1 miss */
void dict_free(Dict *d);

#endif