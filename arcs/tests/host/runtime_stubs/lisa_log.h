#pragma once
void test_log(const char *tag, const char *fmt, ...);
#define LISA_LOGI test_log
#define LISA_LOGW test_log
#define LISA_LOGE test_log
