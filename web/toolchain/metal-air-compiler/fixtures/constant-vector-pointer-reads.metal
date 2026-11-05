// Authored public fixture. No game-source content.
#include <metal_stdlib>
using namespace metal;

struct VectorStorage { float4 rows[8]; };
struct VectorArguments { constant VectorStorage* data [[id(0)]]; };

fragment float4 constant_vector_pointer_reads(
    constant VectorArguments& args [[buffer(0)]],
    float4 position [[position]]) {
    constant int4* words = reinterpret_cast<constant int4*>(args.data);
    uint row = uint(position.x) & 7u;
    uint lane = uint(position.y) & 3u;
    int4 selected = words[row];
    int seed = words[0][lane];
    return float4(float(selected.x ^ seed), float(selected.y),
                  float(selected.z), float(selected.w));
}
