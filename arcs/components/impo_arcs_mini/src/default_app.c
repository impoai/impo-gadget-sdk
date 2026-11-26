/* SPDX-License-Identifier: MIT */
#include "impo_arcs_mini.h"
const impo_arcs_mini_application_t *impo_arcs_mini_application(void)
{
    static const impo_arcs_mini_application_t app = {0};
    return &app;
}
