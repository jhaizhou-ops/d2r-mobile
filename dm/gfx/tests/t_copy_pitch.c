#include "../nvn/nvn_copy_pitch.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    size_t row, image, extent;
    assert(nvn_copy_pitch(9, 5, 3, 4, 4, 16, 0, 0, &row, &image, &extent));
    assert(row == 48 && image == 96 && extent == 288);
    assert(nvn_copy_pitch(9, 5, 3, 4, 4, 16, 64, 256, &row, &image, &extent));
    assert(row == 64 && image == 256 && extent == 768);
    assert(nvn_copy_pitch(1, 1, 0, 1, 1, 4, 0, 0, &row, &image, &extent));
    assert(extent == 4);
    assert(!nvn_copy_pitch(9, 5, 3, 4, 4, 16, 16, 0, &row, &image, &extent));
    assert(!nvn_copy_pitch(9, 5, 3, 4, 4, 16, 64, 64, &row, &image, &extent));
    assert(!nvn_copy_pitch(9, 5, 3, 4, 4, 16, -1, 0, &row, &image, &extent));
    assert(!nvn_copy_pitch(1, 1, 3, 1, 1, 4, 0, INT64_MAX, &row, &image, &extent));
    puts("Host texture copy: compressed blocks, padded layers, invalid pitch and overflow passed.");
}
