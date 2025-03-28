/*
* Copyright (C) 2016 Fredrik Skogman, skogman - at - gmail.com.
* This file is part of libeds.
*
* The contents of this file are subject to the terms of the Common
* Development and Distribution License (the "License"). You may not use this
* file except in compliance with the License. You can obtain a copy of the
* License at http://opensource.org/licenses/CDDL-1.0. See the License for the
* specific language governing permissions and limitations under the License.
* When distributing the software, include this License Header Notice in each
* file and include the License file at http://opensource.org/licenses/CDDL-1.0.
*/

#include <stdlib.h>
#include <string.h>

#include "hmap.h"

#define MAX_KEY_LEN 128
#define STEP_SIZE 1
#define FLAG_OCCUPIED 0x1
#define FLAG_DELETED  0x2

struct hmap
{
        hmap_hash         hfn;
        hmap_cmp          cfn;
        struct hmap_node* elems;
        size_t            cap;
        size_t            size;
        float             lfactor;
};

struct hmap_node
{
        const void* key;
        void*       data;
#ifdef HMAP_USE_TS
        time_t      acc_ts;
#endif
        uint32_t    hash;
        int         flags;
};

static uint32_t hmap_default_hash(const void* key)
{
        /* Jenkin's one at a time hash */
        const char* k = (const char*) key;
        uint32_t hash = 0;
#ifdef __EXTENSIONS__
        /* strnlen(3C) became availible SUSv4/Posix 2008 */
        size_t len = strnlen(key, MAX_KEY_LEN);
#else
        size_t len = strlen(key);

        if (len > MAX_KEY_LEN)
        {
                len = MAX_KEY_LEN;
        }
#endif

        for (size_t i = 0; i < len; i++)
        {
                hash += k[i];
                hash += (hash <<10);
                hash ^= (hash >> 6);
        }

        hash += (hash << 3);
        hash ^= (hash >> 11);
        hash += (hash << 15);

        return hash;
}

static int hmap_default_cmp(const void* a, const void* b)
{
        int r;

        if (a == NULL)
        {
                return -1;
        }
        if (b == NULL)
        {
                return 1;
        }

        r = strncmp((const char*)a, (const char*)b, MAX_KEY_LEN);

        return r;
}

struct hmap* hmap_create(hmap_hash hfn, hmap_cmp cfn, size_t cap, float lf)
{
        struct hmap* h = malloc(sizeof(struct hmap));

        if (hfn == NULL)
        {
                hfn = &hmap_default_hash;
        }
        if (cfn == NULL)
        {
                cfn = &hmap_default_cmp;
        }

        h->hfn = hfn;
        h->cfn = cfn;
        h->cap = cap;
        h->elems = malloc(cap * sizeof(struct hmap_node));
        h->size = 0;
        h->lfactor = lf;
        memset(h->elems, 0, cap * sizeof(struct hmap_node));
        return h;
}

void hmap_clear(struct hmap* h)
{
        h->size = 0;
        memset(h->elems, 0, h->cap * sizeof(struct hmap_node));
}

void hmap_destroy(struct hmap* h)
{
        free(h->elems);
        free(h);
}

void hmap_set(struct hmap* h, const void* key, void* data)
{
        uint32_t k = h->hfn(key);
        size_t spos = k % h->cap;
        size_t pos = spos;
        int added = 1;

        while (h->elems[pos].flags & FLAG_OCCUPIED)
        {
                if (h->elems[pos].hash == k &&
                    h->cfn(h->elems[pos].key, key) == 0)
                {
                        /* Replace value */
                        added = 0;
                        break;
                }

                /* Use linear probing */
                pos = (pos + 1) % h->cap;

                if (pos == spos)
                {
                        /* We are back at the beginning,
                           should NEVER happen */
                        abort();
                }
        }

        /* Only add key if the k-v pair was added.
           Always adding it can cause unexpected behaviour when
           updating an existing value and the key is not dynamically
           allocated. */

        h->elems[pos].data = data;
        h->elems[pos].hash = k;
        h->elems[pos].flags = FLAG_OCCUPIED;

        if (added)
        {
                float lfactor;

                h->elems[pos].key = key;
                h->size++;
                lfactor = ((float)h->size) / (float)h->cap;

                if (lfactor > h->lfactor)
                {
                        size_t ocap = h->cap;

                        /* Extend capacity by two */
                        h->cap *= 2;
                        h->size = 0;

                        /* allocate more data and re-hash */
                        struct hmap_node* new;
                        struct hmap_node* old;
                        size_t new_s = h->cap * sizeof(struct hmap_node);

                        new = malloc(new_s);
                        memset(new, 0, new_s);

                        old = h->elems;
                        h->elems = new;

                        for (size_t i = 0; i < ocap; i++)
                        {
                                if (old[i].flags & FLAG_OCCUPIED)
                                {
                                        hmap_set(h, old[i].key, old[i].data);
                                }
                        }

                        free(old);
                }
        }
}

void* hmap_get(const struct hmap* h, const void* key)
{
        uint32_t k = h->hfn(key);
        size_t spos = k % h->cap;
        size_t pos = spos;

        if (h->cfn(h->elems[pos].key, key) == 0)
        {
                if (h->elems[pos].flags & FLAG_OCCUPIED)
                {
                        return h->elems[pos].data;
                }
        }

        /* Do a linear probe, need to scan over deleted entries too */
        pos = (pos + 1) % h->cap;
        while (h->elems[pos].flags)
        {
                if (h->elems[pos].flags & FLAG_OCCUPIED)
                {
                        if (h->elems[pos].hash == k &&
                            h->cfn(h->elems[pos].key, key) == 0)
                        {
                                return h->elems[pos].data;
                        }
                }

                pos = (pos + 1) % h->cap;

                if (pos == spos)
                {
                        /* We are back at the beginning */
                        break;
                }
        }

        return NULL;
}

struct hmap_entry hmap_del(struct hmap* h, const void* key)
{
        struct hmap_entry ret = {.key = NULL, .data = NULL};
        uint32_t k = h->hfn(key);
        size_t spos = k % h->cap;
        size_t pos = spos;

        if (h->cfn(h->elems[pos].key, key) == 0)
        {
                ret.key = h->elems[pos].key;
                ret.data = h->elems[pos].data;

                h->elems[pos].flags = FLAG_DELETED;
                h->elems[pos].key = NULL;
                h->elems[pos].data = NULL;

                h->size--;

                return ret;
        }

        /* Do a linear probe */
        pos = (pos + 1) % h->cap;
        while (h->elems[pos].flags & FLAG_OCCUPIED)
        {
                if (h->elems[pos].hash == k &&
                    h->cfn(h->elems[pos].key, key) == 0)
                {
                        ret.key = h->elems[pos].key;
                        ret.data = h->elems[pos].data;

                        h->elems[pos].flags = FLAG_DELETED;
                        h->elems[pos].key = NULL;
                        h->elems[pos].data = NULL;

                        h->size--;

                        return ret;
                }
                pos = (pos + 1) % h->cap;

                if (pos == spos)
                {
                        /* We are back at the beginning */
                        break;
                }
        }

        return ret;
}

size_t hmap_size(const struct hmap* h)
{
        return h->size;
}

size_t hmap_cap(const struct hmap* h)
{
        return h->cap;
}

struct hmap_entry* hmap_iter(const struct hmap* h, size_t* size)
{
        struct hmap_entry* e = malloc(h->size * sizeof(struct hmap_entry));
        size_t p = 0;

        for (size_t i = 0; i < h->cap; i++)
        {
                if (h->elems[i].flags & FLAG_OCCUPIED)
                {
                        e[p].key = h->elems[i].key;
                        e[p].data = h->elems[i].data;
                        p++;
                }
        }

        *size = h->size;

        return e;
}
