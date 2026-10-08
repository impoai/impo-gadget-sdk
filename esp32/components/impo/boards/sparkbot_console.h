/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/* Local USB diagnostics: ">chassis=<direction>" (a 300 ms pulse), ">imu" and ">touch". */
bool impo_sparkbot_console(const char *line, bool whole);
