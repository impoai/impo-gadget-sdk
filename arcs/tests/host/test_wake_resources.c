/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "impo_arcs_wake.h"
#include "mbedtls/sha256.h"
static unsigned char fs[4096];
static const char *const words[]={"hello mini","Hello Mini"};
static impo_arcs_wake_config_t cfg={.enabled=true,.label="Hello Mini",.keywords=words,.keyword_count=2,
    .model_file="cae_esr.bin",.wrap_file="wrap.json"};
static void put(unsigned at,unsigned n) { for (int i=3;i>=0;i--) { fs[at+i]=n&255;n>>=8; } }
static void make(void)
{
    memset(fs,0,sizeof(fs)); memcpy(fs,"-rom1fs-",8); put(8,sizeof(fs));
    put(32,1); put(36,64); /* root */
    put(64,128|2); put(72,16); strcpy((char *)fs+80,"cae_esr.bin");
    memcpy(fs+96,"ALOG_RES_HV10001",15);
    put(128,2); const char *wrap="{\"cae\":{\"chns\":{\"mic\":1,\"ref\":1}}}";
    put(136,strlen(wrap)); strcpy((char *)fs+144,"wrap.json"); memcpy(fs+160,wrap,strlen(wrap));
    assert(!mbedtls_sha256_ret(fs+96,16,cfg.model_sha256,0));
    assert(!mbedtls_sha256_ret(fs+160,strlen(wrap),cfg.wrap_sha256,0));
}
int main(void)
{
    impo_arcs_wake_resources_t r;
    make(); assert(impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    assert(r.model_offset==96 && r.wrap_offset==160);
    assert(!impo_arcs_wake_resources(fs,31,&cfg,&r));
    put(8,8192); assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); put(64,64|2); assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); put(72,4096); assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); memset(fs+80,'x',65); assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); fs[96]='?'; assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); fs[160]='?'; assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    assert(impo_arcs_wake_result("{\"rlt\":[{\"bMain\":1,\"keyword\":\"hello mini\"}]}",&cfg));
    assert(!impo_arcs_wake_result("{\"rlt\":[{\"bMain\":0,\"keyword\":\"hello mini\"}]}",&cfg));
    assert(!impo_arcs_wake_result("{\"rlt\":[{\"bMain\":1,\"keyword\":\"other\"}]}",&cfg));
    assert(!impo_arcs_wake_result("{}",&cfg)); assert(!impo_arcs_wake_result("{",&cfg));
    make(); fs[111]^=1; assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); cfg.wrap_sha256[0]^=1; assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r));
    make(); cfg.enabled=false; assert(!impo_arcs_wake_resources(fs,sizeof(fs),&cfg,&r)); cfg.enabled=true;
    const char *const custom[]={"hey companion"};
    cfg.keywords=custom;cfg.keyword_count=1;
    assert(impo_arcs_wake_result("{\"rlt\":[{\"bMain\":1,\"keyword\":\"hey companion\"}]}",&cfg));
    assert(!impo_arcs_wake_result("{\"rlt\":[{\"bMain\":1,\"keyword\":\"Hello Mini\"}]}",&cfg));
    assert(!impo_arcs_wake_result(NULL,&cfg));
    puts("Wake resources: bounds, cycles, bad headers, JSON and keyword filtering passed");
}
