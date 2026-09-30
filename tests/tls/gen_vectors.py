#!/usr/bin/env python3
"""Generates known-answer vectors for the built-in TLS crypto (tests/tls_selftest.cpp)
using an independent implementation (python-cryptography / hashlib / hmac).
Usage: gen_vectors.py > vectors.txt"""
import hashlib, hmac, os, random
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.asymmetric import ec, rsa, padding, x25519, utils
from cryptography.hazmat.primitives import hashes, serialization

random.seed(1234)
def rb(n): return bytes(random.getrandbits(8) for _ in range(n))
def h(b): return b.hex()
out = []
def emit(kind, **kv): out.append(kind + " " + " ".join(f"{k}={v}" for k, v in kv.items()))

HASHES = {"sha256": hashlib.sha256, "sha384": hashlib.sha384, "sha512": hashlib.sha512}
for msg in [b"", b"abc", rb(55), rb(56), rb(63), rb(64), rb(111), rb(112), rb(128), rb(1000)]:
    for name, fn in HASHES.items():
        emit("hash", alg=name, msg=h(msg) or "-", digest=h(fn(msg).digest()))
for name, fn in HASHES.items():
    for klen in (5, 20, 64, 131):
        key, msg = rb(klen), rb(random.randint(0, 300))
        emit("hmac", alg=name, key=h(key), msg=h(msg) or "-", mac=h(hmac.new(key, msg, fn).digest()))
def hkdf(alg, salt, ikm, info, n):
    fn = HASHES[alg]
    prk = hmac.new(salt or b"\0" * fn().digest_size, ikm, fn).digest()
    t, okm = b"", b""
    i = 1
    while len(okm) < n:
        t = hmac.new(prk, t + info + bytes([i]), fn).digest(); okm += t; i += 1
    return okm[:n]
for alg in ("sha256", "sha384"):
    for _ in range(3):
        salt, ikm, info = rb(random.randint(0, 40)), rb(random.randint(1, 40)), rb(random.randint(0, 30))
        n = random.randint(1, 120)
        emit("hkdf", alg=alg, salt=h(salt) or "-", ikm=h(ikm), info=h(info) or "-", n=n, okm=h(hkdf(alg, salt, ikm, info, n)))
# TLS 1.2 PRF
def prf(alg, secret, label, seed, n):
    fn = HASHES[alg]; ls = label + seed; a = ls; out_ = b""
    while len(out_) < n:
        a = hmac.new(secret, a, fn).digest(); out_ += hmac.new(secret, a + ls, fn).digest()
    return out_[:n]
for alg in ("sha256", "sha384"):
    s, seed = rb(48), rb(64)
    emit("prf", alg=alg, secret=h(s), label=h(b"master secret"), seed=h(seed), n=100, out=h(prf(alg, s, b"master secret", seed, 100)))
for klen in (16, 32):
    for plen in (0, 1, 15, 16, 17, 31, 32, 33, 100, 1000):
        key, nonce, aad, pt = rb(klen), rb(12), rb(random.randint(0, 40)), rb(plen)
        emit("aesgcm", key=h(key), nonce=h(nonce), aad=h(aad) or "-", pt=h(pt) or "-", ct=h(AESGCM(key).encrypt(nonce, pt, aad or None)))
for plen in (0, 1, 15, 16, 17, 63, 64, 65, 100, 1000):
    key, nonce, aad, pt = rb(32), rb(12), rb(random.randint(0, 40)), rb(plen)
    emit("chacha", key=h(key), nonce=h(nonce), aad=h(aad) or "-", pt=h(pt) or "-", ct=h(ChaCha20Poly1305(key).encrypt(nonce, pt, aad or None)))
for _ in range(6):
    a, b = x25519.X25519PrivateKey.generate(), x25519.X25519PrivateKey.generate()
    ab = a.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    apub = a.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    bpub = b.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    emit("x25519", priv=h(ab), peer=h(bpub), pub=h(apub), shared=h(a.exchange(b.public_key())))
def uncompressed(pub): return pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
for cname, curve in (("p256", ec.SECP256R1()), ("p384", ec.SECP384R1())):
    for hname, halg in (("sha256", hashes.SHA256()), ("sha384", hashes.SHA384()), ("sha512", hashes.SHA512())):
        for _ in range(3):
            key = ec.generate_private_key(curve); msg = rb(random.randint(1, 200))
            sig = key.sign(msg, ec.ECDSA(halg))
            emit("ecdsa", curve=cname, alg=hname, pub=h(uncompressed(key.public_key())), digest=h(HASHES[hname](msg).digest()), sig=h(sig))
    for _ in range(3):
        a, b = ec.generate_private_key(curve), ec.generate_private_key(curve)
        d = a.private_numbers().private_value
        emit("ecmul", curve=cname, k=format(d, "x"), pub=h(uncompressed(b.public_key())),
             shared=h(a.exchange(ec.ECDH(), b.public_key())))
for bits in (2048, 3072):
    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    nums = key.public_key().public_numbers()
    n = nums.n.to_bytes((bits + 7) // 8, "big"); e = nums.e.to_bytes(3, "big")
    for hname, halg in (("sha256", hashes.SHA256()), ("sha384", hashes.SHA384()), ("sha512", hashes.SHA512())):
        msg = rb(100); dg = HASHES[hname](msg).digest()
        emit("rsa_pkcs1", n=h(n), e=h(e), alg=hname, digest=h(dg), sig=h(key.sign(msg, padding.PKCS1v15(), halg)))
        emit("rsa_pss", n=h(n), e=h(e), alg=hname, digest=h(dg),
             sig=h(key.sign(msg, padding.PSS(mgf=padding.MGF1(halg), salt_length=halg.digest_size), halg)))
print("\n".join(out))
