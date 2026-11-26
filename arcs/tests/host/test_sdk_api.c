/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "impo_arcs_mini.h"
#include "gadget_voice.h"
#include "impo_link.h"
static impo_link_state_t link_state;
static gadget_voice_state_t voice_state;
static bool continuous, available;
static int starts, speech_calls;
impo_link_state_t impo_link_state(void) { return link_state; }
gadget_voice_state_t gadget_voice_state(void) { return voice_state; }
bool gadget_voice_continuous_active(void) { return continuous; }
bool gadget_voice_wakeup_available(void) { return available; }
void gadget_voice_wake(void) { starts++; }
int gadget_speech_say(const char *s) { assert(!strcmp(s,"hello")); speech_calls++; return 7; }
int arcs_mini_wake_set_enabled(bool on) { return on ? -1 : 0; }
int gadget_send_event(const char *name,cJSON *data) { assert(!strcmp(name,"test")); assert(!data); return 9; }
int main(int argc,char **argv)
{
    const impo_arcs_mini_product_t *p=impo_arcs_mini_product();
    assert(argc==3 && !strcmp(p->id,argv[1]));
    assert(p->wake.enabled==(!strcmp(argv[2],"on")));
    if (p->wake.enabled) assert(p->wake.keyword_count>0 && *p->wake.label && *p->wake.model_file);
    else assert(p->wake.keyword_count==0 && !*p->wake.label);
    assert(p && p->display_name && p->instructions && p->model_id);
    assert(impo_arcs_mini_begin_conversation()==-1);
    link_state=IMPO_LINK_ONLINE;
    voice_state=GADGET_VOICE_LISTENING;
    assert(impo_arcs_mini_begin_conversation()==-1);
    voice_state=GADGET_VOICE_IDLE; continuous=true;
    assert(impo_arcs_mini_begin_conversation()==-1);
    continuous=false; assert(impo_arcs_mini_begin_conversation()==0 && starts==1);
    assert(impo_arcs_mini_say(NULL)==-1 && impo_arcs_mini_say("")==-1);
    char huge[1002];memset(huge,'x',sizeof(huge)-1);huge[1001]=0;
    assert(impo_arcs_mini_say(huge)==-1 && !speech_calls);
    assert(impo_arcs_mini_say("hello")==7 && speech_calls==1);
    assert(impo_arcs_mini_set_wake_enabled(true)==-1);
    assert(impo_arcs_mini_set_wake_enabled(false)==0);
    assert(!impo_arcs_mini_wake_available()); available=true; assert(impo_arcs_mini_wake_available());
    assert(impo_arcs_mini_send_event("test",NULL)==9);
    printf("SDK API: %s profile, busy/offline handling and official delegation passed\n",p->id);
    return 0;
}
