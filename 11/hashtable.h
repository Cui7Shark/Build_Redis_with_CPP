#pragma once

#include <stddef.h>
#include <stdint.h>

struct HNode {
    HNode *next = NULL;
    uint64_t hcode = 0; // 8字节长度 2^64
};

struct HTab {
    HNode **tab = NULL;
    size_t mask = 0;
    size_t size = 0;
};

struct HMap {
    HTab ht1;
    HTab ht2;
    size_t resizing_pos = 0;
};
HNode *hm_lookup(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *)); // 遍历哈希表
void hm_insert(HMap *hmap, HNode *node); // charu
HNode *hm_pop(HMap *hmap, HNode *key, bool (*cmp)(HNode *, HNode *)); // 删除节点
// 哈希大小
size_t hm_size(HMap *hamp);
// 删除哈希
void hm_destory(HMap *hmap);
