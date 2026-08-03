/* Self-checks for the pure plumbing in ihs.c: resolution lookup, PIN shape, and
 * the data-dir creation + steamlink-ihs adoption logic (the trickiest path — a
 * regression there silently costs users their pairing). Run via ctest. */
#define _GNU_SOURCE
#undef NDEBUG /* Release adds -DNDEBUG; a test whose asserts vanish tests nothing */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ihs.h"

static void WriteFileStr(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(s, f);
    fclose(f);
}

static bool FileHolds(const char *path, const char *want) {
    char buf[64] = {0};
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    return strcmp(buf, want) == 0;
}

int main(void) {
    /* -- PlumeResIndex: exact match, width disambiguates 480-line modes, and an
     * unknown size falls back to 1080p -- */
    assert(strcmp(PlumeResList[PlumeResIndex(854, 480)].label, "480p") == 0);
    assert(strcmp(PlumeResList[PlumeResIndex(720, 480)].label, "720x480") == 0);
    assert(PlumeResList[PlumeResIndex(1234, 999)].w == 1920);

    /* -- PlumeMakePin: exactly four digits, NUL-terminated -- */
    char pin[5];
    PlumeMakePin(pin);
    assert(strlen(pin) == 4);
    for (int i = 0; i < 4; i++) assert(pin[i] >= '0' && pin[i] <= '9');

    /* -- PlumeDataPath: creates the whole chain under a fresh HOME -- */
    char home[] = "/tmp/plume-test-XXXXXX";
    assert(mkdtemp(home) != NULL);
    setenv("HOME", home, 1);
    char path[512], dir[512];
    PlumeDataPath(path, sizeof(path), "creds.bin");
    snprintf(dir, sizeof(dir), "%s/.local/share/plume", home);
    struct stat st;
    assert(stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
    assert(strncmp(path, dir, strlen(dir)) == 0); /* the file lands inside it */

    /* -- PlumeDataPath: adopts a pre-rename steamlink-ihs dir, content intact -- */
    char home2[] = "/tmp/plume-test-XXXXXX";
    assert(mkdtemp(home2) != NULL);
    setenv("HOME", home2, 1);
    char old[512];
    int rc;
    snprintf(old, sizeof(old), "%s/.local", home2);
    rc = mkdir(old, 0755);
    assert(rc == 0);
    snprintf(old, sizeof(old), "%s/.local/share", home2);
    rc = mkdir(old, 0755);
    assert(rc == 0);
    snprintf(old, sizeof(old), "%s/.local/share/steamlink-ihs", home2);
    rc = mkdir(old, 0700);
    assert(rc == 0);
    char oldCreds[600];
    snprintf(oldCreds, sizeof(oldCreds), "%s/creds.bin", old);
    WriteFileStr(oldCreds, "identity");
    PlumeDataPath(path, sizeof(path), "creds.bin");
    assert(FileHolds(path, "identity")); /* adopted whole */
    assert(stat(old, &st) != 0);         /* old dir is gone */

    /* -- second call: plume dir exists, no adoption attempt, same path -- */
    char path2[512];
    PlumeDataPath(path2, sizeof(path2), "creds.bin");
    assert(strcmp(path, path2) == 0);
    assert(FileHolds(path, "identity"));

    printf("ok\n");
    return 0;
}
