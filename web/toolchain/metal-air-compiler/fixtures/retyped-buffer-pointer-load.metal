// Independently authored public probe: only one buffer keeps a native float view.
#include <metal_stdlib>
using namespace metal;
struct VectorStorage { float4 rows[8]; };
struct VectorArguments {
    constant VectorStorage* first [[id(0)]];
    constant VectorStorage* second [[id(1)]];
};
fragment float4 retyped_buffer_pointer_load(
    constant VectorArguments& args [[buffer(0)]], float4 position [[position]]) {
    constant VectorStorage* first = args.first;
    constant int4* second = reinterpret_cast<constant int4*>(args.second);
    uint row = uint(position.x) & 7u;
    uint lane = uint(position.y) & 3u;
    float native = first->rows[1].x;
    int4 selected;
    if (position.z > native) selected = as_type<int4>(first->rows[0]);
    else selected = second[row];
    return float4(float(selected[lane]), native, float(selected.y), 1.0f);
}
