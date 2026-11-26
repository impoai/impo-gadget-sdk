/* SPDX-License-Identifier: MIT */
#include "impo_arcs_wake.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include "cJSON.h"
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static bool name_end(const uint8_t *fs, uint32_t n, uint32_t off, uint32_t *data)
{
    if (off >= n) return false;
    const uint8_t *end = memchr(fs+off, 0, n-off < 65 ? n-off : 65);
    if (!end) return false;
    *data = ((uint32_t)(end-fs)+16u)&~15u;
    return *data <= n;
}
bool impo_arcs_wake_resources(const uint8_t *fs, size_t capacity, const impo_arcs_wake_config_t *config, impo_arcs_wake_resources_t *out)
{
    if (!config || !config->enabled || !config->model_file || !config->wrap_file || !fs || !out || capacity < 32 || memcmp(fs,"-rom1fs-",8)) return false;
    uint32_t n=be32(fs+8), root;
    if (n>capacity || n<32 || !name_end(fs,n,16,&root) || root>n-16) return false;
    /* The SDK resources are regular files directly below the ROMFS root. */
    if ((be32(fs+root)&7)!=1) return false;
    uint32_t off=be32(fs+root+4);
    impo_arcs_wake_resources_t r={0};
    unsigned budget=128;
    while (off && budget--) {
        uint32_t data;
        if ((off&15) || off>n-16 || !name_end(fs,n,off+16,&data)) return false;
        uint32_t next=be32(fs+off), size=be32(fs+off+8);
        if ((next&7)==2) {
            if (size>n-data) return false;
            const char *name=(const char *)fs+off+16;
            if (!strcmp(name,config->model_file)) { r.model_offset=data; r.model_size=size; }
            if (!strcmp(name,config->wrap_file)) { r.wrap_offset=data; r.wrap_size=size; }
        }
        off=next&~15u;
    }
    if (off || r.model_size<16 || r.wrap_size<2 || r.wrap_size>4096 ||
        memcmp(fs+r.model_offset,"ALOG_RES_HV10001",15)) return false;
    uint8_t digest[32];
    if (mbedtls_sha256_ret(fs+r.model_offset,r.model_size,digest,0) ||
        memcmp(digest,config->model_sha256,sizeof(digest)) ||
        mbedtls_sha256_ret(fs+r.wrap_offset,r.wrap_size,digest,0) ||
        memcmp(digest,config->wrap_sha256,sizeof(digest))) return false;
    /* Bounded copy: the on-flash JSON need not have a terminator. */
    char *json=cJSON_malloc(r.wrap_size+1);
    if (!json) return false;
    memcpy(json,fs+r.wrap_offset,r.wrap_size); json[r.wrap_size]=0;
    cJSON *j=cJSON_Parse(json); cJSON_free(json);
    const cJSON *cae=cJSON_GetObjectItemCaseSensitive(j,"cae");
    const cJSON *chns=cJSON_GetObjectItemCaseSensitive(cae,"chns");
    const cJSON *mic=cJSON_GetObjectItemCaseSensitive(chns,"mic");
    const cJSON *ref=cJSON_GetObjectItemCaseSensitive(chns,"ref");
    bool ok=cJSON_IsNumber(mic)&&mic->valueint==1&&cJSON_IsNumber(ref)&&ref->valueint==1;
    cJSON_Delete(j);
    if (ok) *out=r;
    return ok;
}
bool impo_arcs_wake_result(const char *json, const impo_arcs_wake_config_t *config)
{
    if (!json || !config || !config->enabled || !config->keywords || !config->keyword_count) return false;
    cJSON *j=cJSON_Parse(json);
    const cJSON *rs=cJSON_GetObjectItemCaseSensitive(j,"rlt");
    bool hit=false;
    const cJSON *r;
    cJSON_ArrayForEach(r,rs) {
        const cJSON *main=cJSON_GetObjectItemCaseSensitive(r,"bMain");
        const cJSON *word=cJSON_GetObjectItemCaseSensitive(r,"keyword");
        if (cJSON_IsNumber(main)&&main->valueint==1&&cJSON_IsString(word))
            for (size_t i=0;i<config->keyword_count;i++)
                if (config->keywords[i] && !strcmp(word->valuestring,config->keywords[i])) hit=true;
    }
    cJSON_Delete(j);
    return hit;
}
