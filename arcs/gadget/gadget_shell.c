/*
 * Shell: run a gadget command locally, as link.invoke would. It runs on its own
 * task: the shell's 4 KB stack is too small for cJSON number printing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "shell.h"
#include "gadget_core.h"
#include "impo_link.h"
#include "gadget_sound.h"
#include "gadget_voice.h"
#include "platform_arcs.h"

#define TAG "gadget_sh"
#include "lisa_log.h"

typedef struct {
    char name[65];
    char params[400];
} job_t;

static void run_task(void *arg)
{
    job_t *j = arg;
    cJSON *params = j->params[0] ? cJSON_Parse(j->params) : NULL;
    cJSON *r = gadget_run_command(j->name, params);
    cJSON_Delete(params);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    LISA_LOGI(TAG, "%s: %s", j->name, s ? s : "?");
    cJSON_free(s);
    free(j);
    vTaskDelete(NULL);
}

static int gadget_cmd(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    if (argc < 2) {
        shellPrint(sh, "usage: gadget <command> [json params]\n");
        return -1;
    }
    job_t *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        return -1;
    }
    snprintf(j->name, sizeof(j->name), "%s", argv[1]);
    if (argc >= 3) {
        snprintf(j->params, sizeof(j->params), "%s", argv[2]);
    }
    if (xTaskCreate(run_task, "gadget_sh", 8192 / sizeof(StackType_t), j, 4, NULL) != pdPASS) {
        free(j);
        return -1;
    }
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 gadget, gadget_cmd, gadget <command> [json params]);

static int link_drop_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    impo_link_debug_drop();
    shellPrint(shellGetCurrent(), "link session will drop\n");
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 link_drop, link_drop_cmd, debug: drop the Impo link session);

/* Mic self-test: per-channel level (stereo capture) while quiet, then during a 1 s 1 kHz tone. */
static volatile uint64_t s_sq[2];
static volatile uint32_t s_frames, s_pk[2];

static void mic_level(const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        for (int c = 0; c < 2; c++) {
            int32_t v = pcm[i + c];
            s_sq[c] += (uint64_t)(v * v);
            uint32_t a = (uint32_t)(v < 0 ? -v : v);
            if (a > s_pk[c]) {
                s_pk[c] = a;
            }
        }
    }
    s_frames += n / 2;
}

static uint32_t isqrt64(uint64_t x)
{
    uint32_t r = 0;
    for (uint32_t bit = 1u << 31; bit != 0; bit >>= 1) {
        uint32_t t = r | bit;
        if ((uint64_t)t * t <= x) {
            r = t;
        }
    }
    return r;
}

static void mic_measure(const char *what, bool tone)
{
    s_sq[0] = s_sq[1] = 0;
    s_pk[0] = s_pk[1] = 0;
    s_frames = 0;
    if (tone) {
        static int16_t pcm[16000];
        for (int i = 0; i < 16000; i++) {
            int ph = i % 16;   /* 1 kHz at 16 kHz */
            static const int16_t sine16[16] = { 0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
                                                0, -6123, -11314, -14782, -16000, -14782, -11314, -6123 };
            pcm[i] = sine16[ph];
        }
        uint32_t gen = gadget_sound_begin("mic_test");
        gadget_sound_write(gen, pcm, 16000, 1000);
        gadget_sound_end(gen, NULL);
        vTaskDelay(pdMS_TO_TICKS(1300));
    } else {
        vTaskDelay(pdMS_TO_TICKS(1300));
    }
    uint32_t n = s_frames ? s_frames : 1;
    shellPrint(shellGetCurrent(), "%s: %u frames; L rms %u peak %u; R rms %u peak %u\n", what, (unsigned)s_frames,
               (unsigned)isqrt64(s_sq[0] / n), (unsigned)s_pk[0], (unsigned)isqrt64(s_sq[1] / n), (unsigned)s_pk[1]);
}

static volatile uint32_t s_rate_n, s_rate_calls, s_rate_maxn;
static volatile uint64_t s_rate_sq;

static void mic_count(const int16_t *pcm, size_t n)
{
    s_rate_n += n;
    s_rate_calls++;
    if (n > s_rate_maxn) {
        s_rate_maxn = n;
    }
    for (size_t i = 0; i < n; i++) {
        s_rate_sq += (uint64_t)((int32_t)pcm[i] * pcm[i]);
    }
}

/* Samples per real second (plat_uptime_ms), mono then stereo. */
static int mic_rate_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int stereo = 0; stereo < 2; stereo++) {
        s_rate_n = s_rate_calls = s_rate_maxn = 0;
        s_rate_sq = 0;
        plat_audio_mic_stereo(stereo);
        plat_audio_mic_start(mic_count);
        uint64_t t0 = plat_uptime_ms();
        while (plat_uptime_ms() - t0 < 3000) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        uint32_t ms = (uint32_t)(plat_uptime_ms() - t0);
        plat_audio_mic_stop();
        uint32_t n = s_rate_n ? s_rate_n : 1;
        shellPrint(shellGetCurrent(), "%s: %u samples in %u ms = %u/s, %u callbacks (max %u), rms %u\n",
                   stereo ? "stereo" : "mono", (unsigned)s_rate_n, (unsigned)ms, (unsigned)(s_rate_n * 1000ull / ms),
                   (unsigned)s_rate_calls, (unsigned)s_rate_maxn, (unsigned)isqrt64(s_rate_sq / n));
    }
    plat_audio_mic_stereo(false);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 mic_rate, mic_rate_cmd, debug: mic samples per second);

static int mic_test_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    plat_audio_mic_stereo(true);
    if (plat_audio_mic_start(mic_level) != 0) {
        shellPrint(shellGetCurrent(), "mic did not start\n");
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    mic_measure("quiet", false);
    mic_measure("tone", true);
    mic_measure("quiet", false);
    plat_audio_mic_stop();
    plat_audio_mic_stereo(false);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 mic_test, mic_test_cmd, debug: microphone level while quiet and during a tone);

static int talk_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    gadget_voice_trigger();
    shellPrint(shellGetCurrent(), "voice triggered\n");
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 talk, talk_cmd, debug: same as touching to talk);

/* Self-test of a whole voice turn: the gadget says the words into its own mic. */
static int talk_test_cmd(int argc, char **argv)
{
    char text[200] = "";
    for (int i = 1; i < argc; i++) {
        strncat(text, argv[i], sizeof(text) - strlen(text) - 2);
        if (i + 1 < argc) {
            strcat(text, " ");
        }
    }
    if (text[0] == '\0') {
        strcpy(text, "What is two plus three? Answer in one short sentence.");
    }
    int r = gadget_voice_test(text);
    shellPrint(shellGetCurrent(), r == 0 ? "voice self-test started: %s\n" : "voice busy\n", text);
    return r;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 talk_test, talk_test_cmd, debug: a voice turn spoken into our own mic);
