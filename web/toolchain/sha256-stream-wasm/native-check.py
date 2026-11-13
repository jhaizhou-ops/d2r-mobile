"""以 hashlib 和已知向量检查固定原生 ABI；不执行多 GiB 测试。 | Check native ABI against hashlib/KATs; no multi-GiB run."""

import ctypes
import hashlib
import json
import random
import sys

lib = ctypes.CDLL(sys.argv[1])
lib.sha_input.restype = ctypes.c_void_p
lib.sha_output.restype = ctypes.c_void_p
lib.sha_begin.restype = ctypes.c_uint32
lib.sha_update.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
lib.sha_finish.argtypes = [ctypes.c_uint32]
lib.sha_abort.argtypes = [ctypes.c_uint32]


def encode(raw, step):
    owner = lib.sha_begin()
    assert owner and lib.sha_begin() == 0
    assert lib.sha_update(owner, 0) == 0
    for at in range(0, len(raw), step):
        part = raw[at : at + step]
        ctypes.memmove(lib.sha_input(), part, len(part))
        assert lib.sha_update(owner, len(part)) == 0
        assert ctypes.string_at(lib.sha_input(), 65536) == bytes(65536)
    assert lib.sha_finish(owner) == 0
    assert lib.sha_finish(owner) == -1
    assert ctypes.string_at(lib.sha_input(), 65536) == bytes(65536)
    return ctypes.string_at(lib.sha_output(), 32).hex()


vectors = {
    b"": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    b"abc": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq": "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
}
for raw, expected in vectors.items():
    assert encode(raw, 17) == expected
rng = random.Random(113)
cases = 0
for length in [1, 55, 56, 63, 64, 65, 127, 128, 129, 65535, 65536, 65537, 131089]:
    raw = rng.randbytes(length)
    for step in [17, 4093, 65536]:
        assert encode(raw, step) == hashlib.sha256(raw).hexdigest()
        cases += 1
old = lib.sha_begin()
assert old and lib.sha_abort(old) == 0
owner = lib.sha_begin()
assert owner != old
assert lib.sha_abort(old) == -1
assert lib.sha_update(old, 1) == -1
assert lib.sha_finish(old) == -1
ctypes.memmove(lib.sha_input(), b"abc", 3)
assert lib.sha_update(owner, 3) == 0
assert lib.sha_finish(owner) == 0
assert ctypes.string_at(lib.sha_output(), 32).hex() == vectors[b"abc"]
owner = lib.sha_begin()
ctypes.memmove(lib.sha_input(), b"x", 1)
assert lib.sha_update(owner, 65537) == -2
assert lib.sha_finish(owner) == -1
assert ctypes.string_at(lib.sha_input(), 65536) == bytes(65536)
owner = lib.sha_begin()
assert owner and lib.sha_abort(owner) == 0
assert ctypes.string_at(lib.sha_input(), 65536) == bytes(65536)
assert ctypes.string_at(lib.sha_output(), 32) == bytes(32)
print(
    json.dumps(
        {
            "knownAnswerVectors": len(vectors),
            "partitionCases": cases,
            "lifecycleAndWiping": True,
            "greaterThan4GiBCarryVerified": False,
        }
    )
)
