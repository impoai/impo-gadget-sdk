/* Host tests for gadget/gadget_params.c: commands refuse what their schema does not allow, and say why. */
#include <string.h>

#include "host_check.h"
#include "gadget_params.h"

static const char *REQ_MOVE = "{\"direction\":{\"type\":\"string\",\"enum\":[\"forward\",\"back\",\"stop\"],"
                              "\"description\":\"forward, back or stop.\"}}";
static const char *OPT_MOVE = "{\"duration_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000,"
                              "\"description\":\"How long.\"},\"fast\":{\"type\":\"boolean\"}}";

static bool check(const char *req, const char *opt, const char *json, char *err)
{
    cJSON *p = cJSON_Parse(json);
    bool ok = gadget_params_check(req, opt, p, err, 400);
    cJSON_Delete(p);
    return ok;
}

int main(void)
{
    char err[400];

    CHECK(check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"forward\",\"duration_ms\":3000,\"fast\":true}", err),
          "a good call passes");
    CHECK(check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"stop\"}", err), "optional ones may be left out");
    CHECK(check(NULL, NULL, "{}", err), "no parameters, none sent");

    CHECK(!check(REQ_MOVE, OPT_MOVE, "{}", err) && strstr(err, "missing parameter: \"direction\" must be a string, "
                                                                "one of \"forward\", \"back\", \"stop\"") != NULL,
          "missing required: says what it is and the choices");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"sideways\"}", err) && strstr(err, "got \"sideways\"") != NULL,
          "a value outside the enum is refused and echoed");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"back\",\"duration_ms\":0}", err) &&
              strstr(err, "\"duration_ms\" must be an integer from 1 to 10000 (How long.); got 0") != NULL,
          "below the minimum: the range in the error");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"back\",\"duration_ms\":99999}", err), "above the maximum");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"back\",\"duration_ms\":2.5}", err), "integer refuses 2.5");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"back\",\"duration_ms\":\"3000\"}", err) &&
              strstr(err, "got \"3000\"") != NULL,
          "a number sent as a string is refused, not defaulted");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"back\",\"fast\":1}", err), "boolean refuses 1");
    CHECK(!check(REQ_MOVE, OPT_MOVE, "{\"direction\":\"back\",\"speed\":5}", err) &&
              strstr(err, "unknown parameter \"speed\": this command takes direction, duration_ms, fast") != NULL,
          "an unknown parameter lists the ones it takes");
    CHECK(!check(NULL, NULL, "{\"x\":1}", err) && strstr(err, "takes no parameters") != NULL,
          "a command without parameters says so");

    const char *TEXT = "{\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":4}}";
    CHECK(check(TEXT, NULL, "{\"text\":\"你好世界\"}", err), "length counts characters, not bytes");
    CHECK(!check(TEXT, NULL, "{\"text\":\"\"}", err) && strstr(err, "of 1 to 4 characters") != NULL,
          "empty text is refused with the length range");
    CHECK(!check(TEXT, NULL, "{\"text\":\"hello\"}", err), "too long is refused");

    const char *HEX = "{\"color\":{\"type\":\"string\",\"minLength\":7,\"maxLength\":7}}";
    CHECK(!check(NULL, HEX, "{\"color\":\"red\"}", err) && strstr(err, "of exactly 7 characters") != NULL,
          "equal length bounds read as exactly");

    const char *NUM = "{\"turn\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1}}";
    CHECK(check(NULL, NUM, "{\"turn\":-0.5}", err), "number inside the range");
    CHECK(!check(NULL, NUM, "{\"turn\":3}", err) && strstr(err, "from -1 to 1") != NULL, "number outside the range");
    DONE();
}
