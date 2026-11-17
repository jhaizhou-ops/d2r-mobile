// Independently authored public probe for a live loop-carried pointer merge.
#include <metal_stdlib>
using namespace metal;
struct VectorStorage { float4 rows[8]; };
struct VectorArguments {
    constant VectorStorage* first [[id(0)]];
    constant VectorStorage* second [[id(1)]];
};
fragment float4 loop_carried_buffer_pointer(
    constant VectorArguments& args [[buffer(0)]], float4 position [[position]]) {
    constant VectorStorage* first = args.first;
    constant VectorStorage* second = args.second;
    float native = first->rows[1].x + second->rows[2].y;
    constant int4* cursor = reinterpret_cast<constant int4*>(first);
    uint row = uint(position.x) & 7u;
    uint iterations = (uint(position.w) & 7u) + 1u;
    int total = 0;
    for (uint i = 0; i < iterations; ++i) {
        if (position.z + float(i) > native) {
            cursor = reinterpret_cast<constant int4*>(&second->rows[(row + i) & 7u]);
        }
        total ^= (*cursor)[i & 3u];
    }
    int4 last = *cursor;
    return float4(float(total), native, float(last.x), float(last.w));
}
