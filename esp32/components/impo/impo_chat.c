/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "impo_chat_priv.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "impo_link.h"
#include "impo_settings.h"
#include "impo_wifi.h"

/* Connection state as last reported by the session task (impo_chat_session.cpp). */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static impo_hatch_state_t s_result = IMPO_HATCH_UNTESTED;
static char s_detail[48];

void impo_hatch_report(impo_hatch_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    s_result = state;
    strlcpy(s_detail, detail, sizeof(s_detail));
    portEXIT_CRITICAL(&s_lock);
}

bool impo_hatch_configured(void)
{
    return impo_settings_hatch_token_len() > 0 || impo_link_hatch_linked();
}

void impo_hatch_test(void)
{
    if (!impo_hatch_configured() || !impo_wifi_connected()) {
        return;
    }
    impo_hatch_report(IMPO_HATCH_TESTING, "Connecting...");
    impo_hatch_chat_connect();
}

void impo_hatch_config_changed(void)
{
    impo_hatch_report(IMPO_HATCH_UNTESTED, "");
    impo_hatch_chat_forget();
}

void impo_hatch_status(impo_hatch_status_t *out)
{
    if (!impo_hatch_configured()) {
        out->state = IMPO_HATCH_NOT_SET;
        strlcpy(out->detail, "Pair in the Impo app", sizeof(out->detail));
        return;
    }
    portENTER_CRITICAL(&s_lock);
    out->state = s_result;
    strlcpy(out->detail, s_detail, sizeof(out->detail));
    portEXIT_CRITICAL(&s_lock);
    if (!impo_wifi_connected() && out->state != IMPO_HATCH_TESTING) {
        out->state = IMPO_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (out->state == IMPO_HATCH_UNTESTED && !out->detail[0]) {
        strlcpy(out->detail, "Connects when you talk", sizeof(out->detail));
    }
}

const char *impo_hatch_state_name(impo_hatch_state_t state)
{
    switch (state) {
    case IMPO_HATCH_NOT_SET: return "Not set up";
    case IMPO_HATCH_OFFLINE: return "Offline";
    case IMPO_HATCH_UNTESTED: return "Saved";
    case IMPO_HATCH_TESTING: return "Connecting";
    case IMPO_HATCH_REACHABLE: return "Connected";
    case IMPO_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}
