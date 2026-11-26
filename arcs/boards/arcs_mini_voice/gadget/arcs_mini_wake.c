/* SPDX-License-Identifier: MIT
 * Local keyword detection uses the ListenAI AP algorithm and preserved model.
 * Only idle audio reaches it. Conversation audio stays on the official Impo SDK.
 */
#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stream_buffer.h"
#include "acomp.h"
#include "acomp_wakeup.h"
#include "platform_arcs.h"
#include "gadget_voice.h"
#include "gadget_sound.h"
#include "shell.h"
#include "arcs_mini_wake.h"
#include "impo_arcs_mini.h"
#define TAG "arcs_mini_wake"
#include "lisa_log.h"
#define FLASH_ROMFS ((const uint8_t *)0x30200000u)
#define ROMFS_CAPACITY 0x400000u
#define SAMPLES 256
static StreamBufferHandle_t s_audio;
static QueueHandle_t s_events;
static volatile bool s_ready, s_enabled=true, s_listening;
static volatile uint32_t s_frames, s_drops, s_hits;
static volatile int s_error;
typedef struct { char json[512]; } wake_event_t;
static bool quiet(void)
{
    gadget_sound_state_t sound=gadget_sound_state();
    return s_enabled && gadget_voice_state()==GADGET_VOICE_IDLE &&
        !gadget_voice_continuous_active() && (sound==GADGET_SOUND_IDLE || sound==GADGET_SOUND_FAILED);
}
static void idle_mic(const int16_t *pcm, size_t n)
{
    if (!s_listening || !quiet()) return;
    size_t bytes=n*sizeof(*pcm), sent=xStreamBufferSend(s_audio,pcm,bytes,0);
    if (sent<bytes) s_drops+=(uint32_t)(bytes-sent);
}
static void event_cb(uint32_t event, void *data, uint32_t len, void *priv)
{
    (void)priv;
    if ((event&WAKEUP_CB_EVENT_ENGINE_RLT) && data && len>0 && len<512 && s_listening && quiet()) {
        wake_event_t ev;
        memcpy(ev.json,data,len); ev.json[len]=0;
        xQueueSend(s_events,&ev,0);
    }
}
static int engine_init(void)
{
    impo_arcs_wake_resources_t r;
    if (!impo_arcs_wake_resources(FLASH_ROMFS,ROMFS_CAPACITY,&impo_arcs_mini_product()->wake,&r)) return -100;
    LISA_LOGI(TAG,"preserved resources: model +%x %u bytes, wrap +%x %u bytes",r.model_offset,r.model_size,r.wrap_offset,r.wrap_size);
    acomp_init();
    int e=acomp_wakeup_init();
    if (e) return e;
    e=acomp_wakeup_add_callback(WAKEUP_CB_EVENT_ENGINE_RLT,event_cb,NULL);
    if (e) return e;
    acomp_wakeup_resource_config_t cfg={
        .mlp={.storage=ACOMP_WAKEUP_RES_STORAGE_FLASH,.addr=(uintptr_t)(FLASH_ROMFS+r.model_offset),.size=r.model_size},
        .wrap={.storage=ACOMP_WAKEUP_RES_STORAGE_FLASH,.addr=(uintptr_t)(FLASH_ROMFS+r.wrap_offset),.size=r.wrap_size}
    };
    if ((e=acomp_wakeup_prepare_with_resources(&cfg))) return e;
    if ((e=acomp_wakeup_start())) return e;
    if ((e=acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_WAKEUP))) return e;
    /* Remain a wake engine: command recognition belongs to Impo. */
    if ((e=acomp_wakeup_set_timeout(0))) return e;
    acomp_stream_chn_create_desc_t tx={.cname="stream.mix2ch",.direction=ACOMP_STREAM_DIRECTION_M2R,
        .index=0,.buffer_size=SAMPLES*sizeof(acomp_wakeup_audio_in_t),.num_descs=4,.kick_policy=1};
    acomp_stream_chn_create_desc_t rx={.cname="stream.tocloud",.direction=ACOMP_STREAM_DIRECTION_R2M,
        .index=1,.buffer_size=ACOMP_WAKEUP_AUDIO_OUTPUT_MAX_LEN_ONCE_FRAME,.num_descs=4,.kick_policy=1};
    if ((e=acomp_wakeup_stream_ch_enable(1,&rx))) return e;
    return acomp_wakeup_stream_ch_enable(0,&tx);
}
static void wake_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000)); /* AP IPC and foreground audio startup */
    int e=engine_init();
    if (!e) e=plat_audio_wake_start(idle_mic);
    if (e) {
        s_error=e;
        LISA_LOGE(TAG,"wake unavailable: %d; button voice retained",e);
        vTaskDelete(NULL); return;
    }
    s_ready=true;
    gadget_voice_set_wakeup_available(true);
    LISA_LOGI(TAG,"ready: %s, continuous dialogue=%u",impo_arcs_mini_product()->wake.label,(unsigned)impo_arcs_mini_product()->continuous_dialogue);
    uint32_t quiet_at=(uint32_t)plat_uptime_ms();
    bool was_quiet=false, attached=true;
    int16_t frame[SAMPLES];
    size_t have=0;
    for (;;) {
        /* Always return processed output descriptors to the AP, including during dialogue. */
        uint32_t n; uint16_t idx; void *p;
        while ((p=acomp_wakeup_stream_rx_buffer_get(1,&n,&idx)))
            acomp_wakeup_stream_rx_buffer_release(1,idx,n,p);
        if (!s_enabled && attached) {
            s_listening=false; plat_audio_wake_stop(); attached=false;
            gadget_voice_set_wakeup_available(false);
        } else if (s_enabled && !attached) {
            s_error=plat_audio_wake_start(idle_mic);
            attached=s_error==0;
            gadget_voice_set_wakeup_available(attached);
        }
        bool q=attached&&quiet();
        uint32_t now=(uint32_t)plat_uptime_ms();
        if (!q || !was_quiet) {
            s_listening=false; have=0;
            /* Consumer drains; it never resets a stream buffer under its writer. */
            while (xStreamBufferReceive(s_audio,frame,sizeof(frame),0)) {}
            quiet_at=now;
            if (q) acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_WAKEUP);
        }
        was_quiet=q;
        s_listening=q && now-quiet_at>=500;
        wake_event_t ev;
        while (xQueueReceive(s_events,&ev,0)==pdTRUE) {
            if (s_listening && quiet() && impo_arcs_wake_result(ev.json,&impo_arcs_mini_product()->wake)) {
                s_hits++; s_listening=false; was_quiet=false;
                LISA_LOGI(TAG,"wake accepted #%u",(unsigned)s_hits);
                gadget_voice_wake();
            }
        }
        if (s_listening) {
            have+=xStreamBufferReceive(s_audio,(uint8_t *)frame+have,sizeof(frame)-have,0);
            if (have==sizeof(frame)) {
                uint32_t capacity; uint16_t desc;
                acomp_wakeup_audio_in_t *out=acomp_wakeup_stream_tx_buffer_alloc(0,&capacity,&desc);
                if (out) {
                    if (capacity>=SAMPLES*sizeof(*out)) {
                        for (unsigned i=0;i<SAMPLES;i++) out[i]=(acomp_wakeup_audio_in_t){.mic0=frame[i],.ref0=0};
                        e=acomp_wakeup_stream_tx_buffer_submit(0,out,SAMPLES*sizeof(*out),desc);
                        if (e) s_error=e; else s_frames++;
                    } else { s_error=-101; acomp_wakeup_stream_tx_buffer_submit(0,out,0,desc); }
                } else s_drops+=sizeof(frame);
                have=0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
int arcs_mini_wake_start(void)
{
    if (!impo_arcs_mini_product()->wake.enabled) { s_enabled=false; return 0; }
    s_audio=xStreamBufferCreate(8192,2);
    s_events=xQueueCreate(2,sizeof(wake_event_t));
    if (!s_audio || !s_events || xTaskCreate(wake_task,"arcs_mini_wake",4096/sizeof(StackType_t),NULL,3,NULL)!=pdPASS) {
        s_error=-102; return -1;
    }
    return 0;
}
int arcs_mini_wake_set_enabled(bool enabled)
{
    if (enabled && (!impo_arcs_mini_product()->wake.enabled || !s_ready)) return -1;
    s_enabled=enabled; return 0;
}
static int wake_cmd(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"off")) arcs_mini_wake_set_enabled(false);
    if (argc==2 && !strcmp(argv[1],"on")) arcs_mini_wake_set_enabled(true);
    shellPrint(shellGetCurrent(),"wake ready=%u enabled=%u listening=%u frames=%u dropped=%u hits=%u error=%d dialogue=%u word=%s\n",
        (unsigned)s_ready,(unsigned)s_enabled,(unsigned)s_listening,(unsigned)s_frames,(unsigned)s_drops,
        (unsigned)s_hits,s_error,(unsigned)gadget_voice_continuous_active(),impo_arcs_mini_product()->wake.label);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN,
    wake,wake_cmd,local wake status or on or off);
