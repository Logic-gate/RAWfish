/*
 * conftable_test.c -- host test of app/conftable.h (scripts/check-host.sh):
 * the table of rawfish-ir.conf's keys is coherent, and the readme written
 * from it reads back as the table.
 *   1. every check is well formed (a regular expression compiles, a range
 *      has its two ends) and every default passes its own check;
 *   2. no key twice in one section, every key in a section the table names
 *      (or "" for the old keys at the top);
 *   3. the readme (conf_write_readme): no line starts with ';' (it is text
 *      to read, 0.3.11-553), and its section and key lines -- "[name]"
 *      alone, "key=..." from the line's start -- are, in order, each
 *      section's "[name]" and its keys ("key=default", or "key=...   (written
 *      by the app; absent until then)" for a key with no default), then the
 *      [camera_2] example with the camera keys; no text line looks like one.
 */
#define _POSIX_C_SOURCE 200809L
#include "../conftable.h"

static int fails;
#define FAIL(...) do { fprintf(stderr, "conftable_test: " __VA_ARGS__); fputc('\n', stderr); ++fails; } while (0)

static int section_known(const char *s)
{
    int i;
    if (!*s) return 1;
    for (i = 0; k_conf_sections[i].name; ++i) if (!strcmp(k_conf_sections[i].name, s)) return 1;
    return 0;
}

int main(void)
{
    int i, j, nkeys = 0;
    char path[] = "/tmp/conftable_test_XXXXXX", line[1024], want[512];
    FILE *f;
    int fd;
    /* 1, 2: the table */
    for (i = 0; k_conf_keys[i].key; ++i, ++nkeys) {
        const conf_key_t *k = &k_conf_keys[i];
        if (!section_known(k->section)) FAIL("%s: section \"%s\" not in k_conf_sections", k->key, k->section);
        if (!strncmp(k->check, "re:", 3)) {
            regex_t re;
            if (regcomp(&re, k->check + 3, REG_EXTENDED | REG_NOSUB) != 0) FAIL("%s: bad regular expression %s", k->key, k->check);
            else regfree(&re);
        } else if (!strncmp(k->check, "range:", 6)) {
            if (!strstr(k->check, "..")) FAIL("%s: bad range %s", k->key, k->check);
        } else if (strncmp(k->check, "list:", 5) && strcmp(k->check, "any")) FAIL("%s: unknown check %s", k->key, k->check);
        if (k->def && !conf_value_ok(k->check, k->def)) FAIL("%s: its default \"%s\" fails its check %s", k->key, k->def, k->check);
        for (j = 0; j < i; ++j)
            if (!strcmp(k_conf_keys[j].section, k->section) && !strcmp(k_conf_keys[j].key, k->key)) FAIL("%s/%s twice", k->section, k->key);
    }
    /* the checks themselves, on a few values */
    if (!conf_value_ok("list:,4:3,16:9", "") || conf_value_ok("list:,4:3,16:9", "3:2")) FAIL("list check");
    if (!conf_value_ok("range:-20..20", "-20") || conf_value_ok("range:-20..20", "21") || conf_value_ok("range:0..5", "") || conf_value_ok("range:0..5", "2x")) FAIL("range check");
    if (!conf_value_ok("re:^([1-9][0-9]{2,4}x[1-9][0-9]{2,4})?$", "4096x3072") || conf_value_ok("re:^([1-9][0-9]{2,4}x[1-9][0-9]{2,4})?$", "4096x3072 ")) FAIL("re check");
    /* 3: the readme */
    fd = mkstemp(path);
    if (fd < 0 || !(f = fdopen(fd, "w+"))) { FAIL("no temporary file"); return 1; }
    if (conf_write_readme(f) != 0) FAIL("conf_write_readme failed");
    rewind(f);
    {
        int s = 0, k = -1, cam = 0;   /* the section expected next, the key in it, in [camera_2] */
        const char *cur = NULL;
        regex_t sec, key;
        if (regcomp(&sec, "^\\[[a-z0-9_]+\\]$", REG_EXTENDED | REG_NOSUB) || regcomp(&key, "^[A-Za-z][A-Za-z0-9_]*=", REG_EXTENDED | REG_NOSUB)) {
            FAIL("regcomp"); return 1;
        }
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = '\0';
            if (line[0] == ';') { FAIL("a ';' line in the readme: %s", line); continue; }
            if (regexec(&sec, line, 0, NULL, 0) == 0) {
                if (k_conf_sections[s].name && !strcmp(k_conf_sections[s].name, "camera")) {
                    if (strcmp(line, "[camera_2]")) FAIL("section %s where [camera_2] was expected", line);
                    cur = "camera"; ++s; k = -1; cam = 1;
                    continue;
                }
                if (!k_conf_sections[s].name) { FAIL("an extra section: %s", line); continue; }
                snprintf(want, sizeof want, "[%s]", k_conf_sections[s].name);
                if (strcmp(line, want)) FAIL("section %s where %s was expected", line, want);
                cur = k_conf_sections[s++].name; k = -1;
                continue;
            }
            if (regexec(&key, line, 0, NULL, 0) != 0) continue;   /* text */
            /* the next key of this section */
            for (++k; k_conf_keys[k].key && (!cur || strcmp(k_conf_keys[k].section, cur)); ++k) {}
            if (!k_conf_keys[k].key) { FAIL("an extra line: %s", line); k = -1; continue; }
            if (cam) snprintf(want, sizeof want, "%s=%s", k_conf_keys[k].key, !strcmp(k_conf_keys[k].key, "jpgSharpen") ? "1" : k_conf_keys[k].def);
            else if (k_conf_keys[k].def) snprintf(want, sizeof want, "%s=%s", k_conf_keys[k].key, k_conf_keys[k].def);
            else snprintf(want, sizeof want, "%s=...   (written by the app; absent until then)", k_conf_keys[k].key);
            if (strcmp(line, want)) FAIL("\"%s\" where \"%s\" was expected", line, want);
        }
        if (k_conf_sections[s].name || !cam) FAIL("the readme ends before section %s", k_conf_sections[s].name ? k_conf_sections[s].name : "camera");
        regfree(&sec); regfree(&key);
    }
    fclose(f);
    remove(path);
    if (fails) return 1;
    printf("conftable: %d keys coherent, the readme reads back as the table (OK)\n", nkeys);
    return 0;
}
