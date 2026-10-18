// SPDX-License-Identifier: MIT
/* Parameter checks against a command's published schema; see command_params.h. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "command_params.h"

/* "an integer from 1 to 10000", "a string, one of "a", "b"", ... */
static void expected(const cJSON *spec, char *out, size_t cap)
{
    const cJSON *type = cJSON_GetObjectItem(spec, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "value";
    const char *article = strchr("aeiou", t[0]) != NULL ? "an" : "a";
    int n = snprintf(out, cap, "%s %s", article, t);
    const cJSON *min = cJSON_GetObjectItem(spec, "minimum");
    const cJSON *max = cJSON_GetObjectItem(spec, "maximum");
    if (cJSON_IsNumber(min) && cJSON_IsNumber(max)) {
        n += snprintf(out + n, cap - n, " from %g to %g", min->valuedouble, max->valuedouble);
    } else if (cJSON_IsNumber(min)) {
        n += snprintf(out + n, cap - n, " of at least %g", min->valuedouble);
    } else if (cJSON_IsNumber(max)) {
        n += snprintf(out + n, cap - n, " of at most %g", max->valuedouble);
    }
    const cJSON *lo = cJSON_GetObjectItem(spec, "minLength");
    const cJSON *len = cJSON_GetObjectItem(spec, "maxLength");
    if (cJSON_IsNumber(lo) && cJSON_IsNumber(len) && lo->valueint == len->valueint) {
        n += snprintf(out + n, cap - n, " of exactly %d characters", len->valueint);
    } else if (cJSON_IsNumber(lo) && cJSON_IsNumber(len)) {
        n += snprintf(out + n, cap - n, " of %d to %d characters", lo->valueint, len->valueint);
    } else if (cJSON_IsNumber(len)) {
        n += snprintf(out + n, cap - n, " of at most %d characters", len->valueint);
    } else if (cJSON_IsNumber(lo)) {
        n += snprintf(out + n, cap - n, " of at least %d characters", lo->valueint);
    }
    const cJSON *en = cJSON_GetObjectItem(spec, "enum");
    if (cJSON_IsArray(en)) {
        n += snprintf(out + n, cap - n, ", one of");
        const cJSON *e;
        bool first = true;
        cJSON_ArrayForEach(e, en) {
            if (cJSON_IsString(e) && (size_t)n < cap) {
                n += snprintf(out + n, cap - n, "%s \"%s\"", first ? "" : ",", e->valuestring);
                first = false;
            }
        }
    }
}

/* The value as the agent sent it, short: "abc", 12, true, an object. */
static void shown(const cJSON *v, char *out, size_t cap)
{
    if (cJSON_IsString(v)) {
        snprintf(out, cap, "\"%.40s%s\"", v->valuestring, strlen(v->valuestring) > 40 ? "..." : "");
    } else if (cJSON_IsNumber(v)) {
        snprintf(out, cap, "%g", v->valuedouble);
    } else if (cJSON_IsBool(v)) {
        snprintf(out, cap, "%s", cJSON_IsTrue(v) ? "true" : "false");
    } else if (cJSON_IsNull(v)) {
        snprintf(out, cap, "null");
    } else {
        snprintf(out, cap, "%s", cJSON_IsArray(v) ? "a list" : "an object");
    }
}

/* Characters, not bytes: a Chinese sentence is three bytes a character. */
static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        n += ((unsigned char)*s & 0xC0) != 0x80;
    }
    return n;
}

static bool fits(const cJSON *spec, const cJSON *v)
{
    const cJSON *type = cJSON_GetObjectItem(spec, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : NULL;
    if (t == NULL) {
        return true;
    }
    if (strcmp(t, "string") == 0) {
        if (!cJSON_IsString(v)) {
            return false;
        }
        const cJSON *lo = cJSON_GetObjectItem(spec, "minLength");
        const cJSON *len = cJSON_GetObjectItem(spec, "maxLength");
        size_t chars = utf8_chars(v->valuestring);
        if ((cJSON_IsNumber(len) && chars > (size_t)len->valueint) ||
            (cJSON_IsNumber(lo) && chars < (size_t)lo->valueint)) {
            return false;
        }
        const cJSON *en = cJSON_GetObjectItem(spec, "enum");
        if (cJSON_IsArray(en)) {
            const cJSON *e;
            cJSON_ArrayForEach(e, en) {
                if (cJSON_IsString(e) && strcmp(e->valuestring, v->valuestring) == 0) {
                    return true;
                }
            }
            return false;
        }
        return true;
    }
    if (strcmp(t, "integer") == 0 || strcmp(t, "number") == 0) {
        if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
            return false;
        }
        if (t[0] == 'i' && v->valuedouble != floor(v->valuedouble)) {
            return false;
        }
        const cJSON *min = cJSON_GetObjectItem(spec, "minimum");
        const cJSON *max = cJSON_GetObjectItem(spec, "maximum");
        return !(cJSON_IsNumber(min) && v->valuedouble < min->valuedouble) &&
               !(cJSON_IsNumber(max) && v->valuedouble > max->valuedouble);
    }
    if (strcmp(t, "boolean") == 0) {
        return cJSON_IsBool(v);
    }
    if (strcmp(t, "object") == 0) {
        return cJSON_IsObject(v);
    }
    if (strcmp(t, "array") == 0) {
        return cJSON_IsArray(v);
    }
    return true;
}

static void names(const cJSON *req, const cJSON *opt, char *out, size_t cap)
{
    int n = 0;
    out[0] = '\0';
    const cJSON *schemas[2] = { req, opt };
    for (int s = 0; s < 2; s++) {
        const cJSON *p;
        cJSON_ArrayForEach(p, schemas[s]) {
            if ((size_t)n < cap) {
                n += snprintf(out + n, cap - n, "%s%s", n ? ", " : "", p->string);
            }
        }
    }
}

static void describe_one(const char *name, const cJSON *spec, char *out, size_t cap)
{
    char want[160];
    expected(spec, want, sizeof(want));
    const cJSON *d = cJSON_GetObjectItem(spec, "description");
    if (cJSON_IsString(d)) {
        snprintf(out, cap, "\"%s\" must be %s (%s)", name, want, d->valuestring);
    } else {
        snprintf(out, cap, "\"%s\" must be %s", name, want);
    }
}

bool command_params_check(const char *required, const char *optional, const cJSON *params, char *err, size_t cap)
{
    cJSON *req = required ? cJSON_Parse(required) : NULL;
    cJSON *opt = optional ? cJSON_Parse(optional) : NULL;
    bool ok = true;
    char line[320];
    err[0] = '\0';

    /* Every parameter sent must be one the command takes, of the published kind. */
    const cJSON *v;
    cJSON_ArrayForEach(v, params) {
        const cJSON *spec = req ? cJSON_GetObjectItem(req, v->string) : NULL;
        if (spec == NULL && opt != NULL) {
            spec = cJSON_GetObjectItem(opt, v->string);
        }
        if (spec == NULL) {
            char all[200];
            names(req, opt, all, sizeof(all));
            if (all[0] == '\0') {
                snprintf(err, cap, "unknown parameter \"%s\": this command takes no parameters", v->string);
            } else {
                snprintf(err, cap, "unknown parameter \"%s\": this command takes %s", v->string, all);
            }
            ok = false;
            break;
        }
        if (!fits(spec, v)) {
            char got[64];
            shown(v, got, sizeof(got));
            describe_one(v->string, spec, line, sizeof(line));
            snprintf(err, cap, "%s; got %s", line, got);
            ok = false;
            break;
        }
    }
    /* And every required one must be there. */
    const cJSON *spec;
    cJSON_ArrayForEach(spec, req) {
        if (!ok) {
            break;
        }
        if (cJSON_GetObjectItem(params, spec->string) == NULL) {
            describe_one(spec->string, spec, line, sizeof(line));
            snprintf(err, cap, "missing parameter: %s", line);
            ok = false;
        }
    }
    cJSON_Delete(req);
    cJSON_Delete(opt);
    return ok;
}
