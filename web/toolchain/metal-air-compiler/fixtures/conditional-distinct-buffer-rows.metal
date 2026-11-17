// Authored public probe: conditional loads have unlike original pointee shapes.
#include <metal_stdlib>
using namespace metal;
struct VectorStorage { float4 rows[8]; };
struct VectorArguments {
    constant VectorStorage* first [[id(0)]];
    constant VectorStorage* second [[id(1)]];
};
fragment float4 conditional_distinct_buffer_rows(
    constant VectorArguments& args [[buffer(0)]], float4 position [[position]]) {
    uint row = uint(position.x) & 7u;
    uint lane = uint(position.y) & 3u;
    constant VectorStorage* first = args.first;
    constant VectorStorage* second = args.second;
    float native = first->rows[1].x + second->rows[2].y;
    int4 selected;
    if (position.z > native) {
        selected = as_type<int4>(first->rows[0]);
    } else {
        selected = as_type<int4>(second->rows[row]);
    }
    return float4(float(selected[lane]), native, float(selected.y), 1.0f);
}
