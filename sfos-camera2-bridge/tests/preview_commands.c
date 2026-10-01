// Exercises the production parser without Android/Qt dependencies.
// Build separately when permitted: cc -std=c11 -Wall -Wextra -Werror
// tests/preview_commands.c -o /tmp/test-preview-commands
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include "../android/preview_commands.h"

static struct preview_command command;
static void invalid(const char *text) {
    memset(&command, 0, sizeof(command));
    assert(preview_parse_capture_command(text, &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_INVALID);
}
int main(void) {
    assert(preview_parse_capture_command("capture-raw /tmp/a.raw /tmp/a.json", &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW);
    assert(!strcmp(command.path, "/tmp/a.raw"));
    assert(!strcmp(command.metadata_path, "/tmp/a.json"));
    assert(preview_parse_capture_command("capture-jpeg /tmp/a.jpg", &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_JPEG);
    // Regression: previously sscanf(capture-raw ...) consumed this as
    // raw_path="-bracket", metadata_path="2" before the bracket branch ran.
    const char *pair="capture-raw-bracket 2 /tmp/l.raw /tmp/l.json 1600 40000000 /tmp/s.raw /tmp/s.json 1600 10000000";
    assert(preview_parse_capture_command(pair, &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET);
    assert(command.bracket_count == 2);
    assert(!strcmp(command.bracket_paths[0], "/tmp/l.raw"));
    assert(!strcmp(command.bracket_metadata_paths[1], "/tmp/s.json"));
    assert(command.bracket_sensor_sensitivity[1] == 1600);
    assert(command.bracket_exposure_time_ns[0] == 40000000);
    assert(command.bracket_exposure_time_ns[1] == 10000000);
    assert(preview_parse_capture_command(" capture-raw\t/tmp/a.raw\t/tmp/a.json\r", &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW);
    invalid("capture-raw-bracket");
    invalid("capture-raw-bracket 2");
    invalid("capture-raw-bracket 3 a b 100 40 c d 100 10 e f 100 5");
    invalid("capture-raw-bracket 2junk a b 100 40 c d 100 10");
    invalid("capture-raw-bracket 2 a b 100x 40 c d 100 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 10x");
    invalid("capture-raw-bracket 2 a b 2147483648 40 c d 2147483648 10");
    invalid("capture-raw-bracket 2 a b 100 9223372036854775808 c d 100 10");
    invalid("capture-raw-bracket 2 a b 0 40 c d 0 10");
    invalid("capture-raw-bracket 2 a b -100 40 c d 100 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 0");
    invalid("capture-raw-bracket 2 a b 100 40 c d 200 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 40");
    invalid("capture-raw-bracket 2 a b 100 10 c d 100 40");
    invalid("capture-raw-bracket 2 a b 100 40 a d 100 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 10 extra");
    invalid("capture-raw");
    invalid("capture-raw a");
    invalid("capture-raw a b extra");
    invalid("capture-raw-suffix a b");
    invalid("capture-jpeg-suffix a");
    invalid("capture-jpeg a extra");
    char oversized[9000];memset(oversized,'x',sizeof(oversized));
    memcpy(oversized,"capture-raw ",12);oversized[sizeof(oversized)-1]=0;
    invalid(oversized);
    assert(!preview_parse_capture_command("zoom 2", &command));
    puts("Capture parser regression tests passed");
}
