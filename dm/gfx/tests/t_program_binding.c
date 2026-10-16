#include "../nvn/nvn_program_binding.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

uint64_t nvn_new_serial(void) { static _Atomic uint64_t serial = 100; return ++serial; }

int main(void) {
    prog_t vs = {.magic = MAGIC('P','R','O','G'), .serial = 1};
    prog_t ps = {.magic = MAGIC('P','R','O','G'), .serial = 2};
    prog_t cs = {.magic = MAGIC('P','R','O','G'), .serial = 3};
    const prog_t *state = nvn_program_bind(NULL, &vs, 1);
    state = nvn_program_bind(state, &ps, 2);
    const prog_t *graphics = state;
    state = nvn_program_bind(state, NULL, 32);
    assert(state == graphics);
    const nvn_program_binding *b = (const nvn_program_binding *)state;
    assert(b->source[0] == &vs && b->source[1] == &ps && !b->source[5]);
    state = nvn_program_bind(state, &cs, 32);
    assert(((const nvn_program_binding *)state)->source[0] == &vs);
    assert(nvn_program_bind(state, NULL, 32) == graphics);
    state = nvn_program_bind(state, NULL, 2);
    assert(!((const nvn_program_binding *)state)->source[1]);
    assert(((const nvn_program_binding *)state)->source[5] == &cs);
    assert(nvn_program_bind(state, NULL, 63) == NULL);
    vs.serial = 4;
    state = nvn_program_bind(graphics, &vs, 1);
    assert(state != graphics);
    assert(((const nvn_program_binding *)state)->source_serial[0] == 4);
    assert(((const nvn_program_binding *)graphics)->source_serial[0] == 1);
    puts("Host program binding: independent stages, masked unbind, reuse and source generations passed.");
}
