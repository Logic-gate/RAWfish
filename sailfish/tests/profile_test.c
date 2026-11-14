/* Host test of the command line's device-profile readers: the integer keys,
 * the focus calibration, and the vendor_request / session_request lists,
 * from the user's override-hal.json over a shipped profile. The CLI is
 * included whole (its readers are static), its main renamed; the engine is
 * never loaded: the two lists go to stubs. Run by scripts/check-host.sh. */
#define main cli_main
#include "../rawfish-ir.c"
#undef main

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

static struct { uint32_t tag; int type; double value; int on; } got[16];
static int ngot;
static int vendor_stub(uint32_t tag, int type, double value, int on, char *out, size_t n)
{
    if (ngot < 16) { got[ngot].tag = tag; got[ngot].type = type; got[ngot].value = value; got[ngot].on = on; ngot++; }
    snprintf(out, n, "{\"ok\":true}");
    return 0;
}
static int session_stub(uint32_t tag, int type, double value)
{
    if (ngot < 16) { got[ngot].tag = tag; got[ngot].type = type; got[ngot].value = value; got[ngot].on = -1; ngot++; }
    return 0;
}
static void *dlsym_stub(void *handle, const char *name)
{
    (void)handle;
    return !strcmp(name, "indraw_set_session_request") ? (void *)session_stub : NULL;
}
static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(text, f);
    return fclose(f);
}

static const char k_user[] =
    "{ \"cameras\": [\n"
    "  { \"camera_id\": \"0\",\n"
    "    \"_comment\": \"a key named \\\"vendor_request\\\" inside a comment\",\n"
    "    \"overrides\": { \"peak_edge\": 40 },\n"
    "    \"focus_calibration\": [ { \"real_m\": 0.5, \"hal_diopters\": 2.5 } ],\n"
    "    \"session_request\": [ { \"tag\": \"0x80010003\", \"type\": \"u8\", \"value\": 2 },\n"
    "                           { \"tag\": \"0x80010004\", \"type\": \"double\", \"value\": 0.25 },\n"
    "                           { \"tag\": \"0x80010005\", \"type\":\"float\", \"value\": 1.5 } ],\n"
    "    \"vendor_request\": [\n"
    "      { \"tag\": \"0x80010000\", \"type\": \"i32\", \"value\": 7, \"on\": \"preview\" },\n"
    "      { \"tag\": \"0x00010001\", \"type\": \"i32\", \"value\": 3 },\n"
    "      { \"tag\": \"0x80030002\", \"type\": \"i64\", \"value\": 77, \"on\": \"all\" } ] },\n"
    "  { \"camera_id\": \"2\", \"overrides\": { \"zebra_raw\": 900 } } ] }\n";
static const char k_shipped[] =
    "{ \"match\": { \"make\": \"Test\" }, \"cameras\": [\n"
    "  { \"camera_id\": \"0\", \"overrides\": { \"peak_edge\": 20, \"capture_delay_ms\": 300 },\n"
    "    \"focus_calibration\": [ { \"real_m\": null, \"hal_diopters\": 2.0 },\n"
    "                             { \"real_m\": 0.5, \"hal_diopters\": 2.6 },\n"
    "                             { \"real_m\": 0.1, \"hal_diopters\": 14.0 } ] },\n"
    "  { \"camera_id\": \"2\", \"vendor_request\": [ { \"tag\": \"0x80090009\", \"type\": \"u8\", \"value\": 4, \"on\": \"photo\" } ] } ] }\n";

int main(void)
{
    static char out[OUT_SIZE];
    char home[] = "/tmp/indraw-profile-XXXXXX", dir[1100], path[1200];
    float real_d[16], hal_d[16];
    int n;
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    unsetenv("XDG_DATA_HOME");
    if (profile_dir(dir, sizeof dir) != 0 || mkdir_p(dir) != 0) { puts("profile: no folder"); return 1; }
    snprintf(path, sizeof path, "%s/override-hal.json", dir);
    if (write_file(path, k_user)) return 1;
    snprintf(g_ov_shipped, sizeof g_ov_shipped, "%s/shipped.json", home);
    if (write_file(g_ov_shipped, k_shipped)) return 1;
    a_dlsym = (android_dlsym_fn)dlsym_stub;
    g_bridge_for_vendor = NULL;

    /* 1. integer keys: the user's first, then the shipped profile's */
    CHECK(read_override_checked("0", "peak_edge") == 40, "peak_edge: the user's 40 over the shipped 20");
    CHECK(read_override_checked("0", "capture_delay_ms") == 300, "capture_delay_ms: the shipped profile's, absent from the user's");
    CHECK(read_override_checked("2", "zebra_raw") == 900 && read_override_checked("1", "peak_edge") == 0,
          "another camera's own block; a camera listed nowhere: 0");

    /* 2. the focus calibration: shipped, then the user's point replacing the same distance */
    n = read_focus_calibration("0", real_d, hal_d, 16);
    CHECK(n == 3 && real_d[0] == 0.0f && hal_d[0] == 2.0f && fabsf(real_d[1] - 2.0f) < 1e-4f && hal_d[1] == 2.5f &&
          fabsf(real_d[2] - 10.0f) < 1e-4f && hal_d[2] == 14.0f,
          "calibration: 3 points sorted, the user's 0.5 m replacing the shipped one");

    /* 3. session_request: each entry with its own type (they all went as i32) */
    ngot = 0;
    apply_session_requests(NULL, "0");
    CHECK(ngot == 3 && got[0].tag == 0x80010003u && got[0].type == 0 && got[0].value == 2 &&
          got[1].type == 4 && got[1].value == 0.25 && got[2].type == 2 && got[2].value == 1.5,
          "session_request: u8, double and float (no space after the colon) read as such");

    /* 4. vendor_request: the entry outside 0x8000xxxx refused, the rest in order; read twice (the text kept intact) */
    ngot = 0;
    apply_vendor_requests(vendor_stub, "0", out);
    apply_vendor_requests(vendor_stub, "0", out);
    CHECK(ngot == 4 && got[0].tag == 0x80010000u && got[0].type == 1 && got[0].value == 7 && got[0].on == 0 &&
          got[1].tag == 0x80030002u && got[1].type == 3 && got[1].on == 2 && got[2].tag == got[0].tag && got[3].tag == got[1].tag,
          "vendor_request: two entries kept of three, the same on a second read");
    ngot = 0;
    apply_vendor_requests(vendor_stub, "2", out);
    CHECK(ngot == 1 && got[0].tag == 0x80090009u && got[0].type == 0 && got[0].on == 1,
          "vendor_request: from the shipped profile when the user's block has none");

    /* 5. a broken user file: left out, the shipped profile still read */
    if (write_file(path, "{ \"cameras\": [ { \"camera_id\": \"0\", \"overrides\": { \"peak_edge\": 40 } ")) return 1;
    find_shipped_profile(NULL, "0");                 /* (no engine: only drops the cached texts) */
    snprintf(g_ov_shipped, sizeof g_ov_shipped, "%s/shipped.json", home);
    g_ov_user_ok = check_override("0");
    CHECK(!g_ov_user_ok && read_override_checked("0", "peak_edge") == 20, "a broken override-hal.json: ignored, the shipped 20");

    unlink(path); unlink(g_ov_shipped);
    {   /* the folders made, from the deepest up to the temporary HOME */
        char *slash;
        while (strlen(dir) > strlen(home) && rmdir(dir) == 0 && (slash = strrchr(dir, '/'))) *slash = '\0';
        rmdir(home);
    }
    printf(fails ? "profile: FAILED (%d)\n" : "profile: OK (8 rules)\n", fails);
    return fails ? 1 : 0;
}
