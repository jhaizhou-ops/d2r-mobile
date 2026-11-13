// Authored public probe: entry argument cannot be replaced by a casted slot load.
#include <metal_stdlib>
using namespace metal;
struct VectorStorage { float4 rows[8]; };
fragment float4 direct_buffer_vector_pointer(
    constant VectorStorage* data [[buffer(0)]],
    float4 position [[position]]) {
    constant int4* words = reinterpret_cast<constant int4*>(data);
    uint row = uint(position.x) & 7u;
    uint lane = uint(position.y) & 3u;
    int4 selected = words[row];
    int seed = words[0][lane];
    return float4(float(selected.x ^ seed), float(selected.y),
                  float(selected.z), float(selected.w));
}
