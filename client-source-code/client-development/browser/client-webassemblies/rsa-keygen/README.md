# how rsa works

rsa is the public key encryption system that lemon chat identities are built on. the wasm in
this folder generates the keys and does the raw rsa math; this file explains what all of that
actually is, for someone who has never touched rsa. it explains rsa three times - briefly,
then the mechanics, then the reasons behind them - and ends with rsa's known weaknesses and
what could be done better.

## the brief version

every user owns two keys that belong together:

- a **public key**, which everyone is allowed to see
- a **private key**, which never leaves its owner

whatever is encrypted with the public key can only be decrypted with the matching private key.
so anyone can take your public key and encrypt a message that only you can read - without you
and the sender ever having to agree on a shared secret first.

the two keys are linked through one huge number `n`, the product of two big prime numbers `p`
and `q`. the public key contains `n`; the private key is computed from `p` and `q`. security
rests on one assumption: recovering `p` and `q` from `n` alone - called factoring - has no
known practical method at these sizes. the last section covers how solid that assumption is.

## more in depth

making a keypair:

1. pick two random primes `p` and `q`, each half the wanted key size (a 2048 bit key uses two
   1024 bit primes)
2. multiply them: `n = p * q`. `n` is called the **modulus** and is public
3. compute the helper number `phi = (p-1) * (q-1)`. it must stay secret; computing it from
   `n` alone would be the same problem as finding `p` and `q`
4. pick the **public exponent** `e`. lemon chat uses `e = 3`. it must share no divisor with
   `phi`, which is why the key generator rejects any prime where `p-1` divides by 3
5. compute the **private exponent** `d`: the number for which `(e * d) mod phi = 1`

the public key is `(n, e)`. the private key is `d`, plus some speed helpers explained below.

using it:

- the message is first turned into a number `m` smaller than `n`
- **encrypt** with the public key: `c = m^e mod n` - raise `m` to the e-th power, keep only
  the remainder after dividing by `n`. `c` is the ciphertext
- **decrypt** with the private key: `m = c^d mod n`. the original number comes back exactly

the same with numbers small enough to check by hand:

- `p = 5`, `q = 11` gives `n = 55` and `phi = 4 * 10 = 40`
- `e = 3`, and `d = 27`, because `3 * 27 = 81` and `81 mod 40 = 1`
- take the message `m = 7`. encrypt: `7^3 = 343`, and `343 mod 55 = 13`. the ciphertext is 13
- decrypt: `13^27 mod 55 = 7`. the message is back

both directions are this one operation, just with numbers hundreds of digits long. the
`rsa_keygen__modpow` export of this wasm is exactly this `base^exponent mod n` calculation.

two practical notes that surprise newcomers:

- rsa can only encrypt a number smaller than `n`, so nobody encrypts actual messages with it.
  the client encrypts a random aes key with rsa and the real payload with aes - that is the
  envelope rsa-crypto.js builds
- encrypting the same message twice must not produce the same ciphertext, or an observer could
  spot repeats. a padding step (pkcs#1) mixes fresh random bytes into `m` before every
  encryption, so the numbers going into rsa never repeat

## in depth

**why the private key undoes the public key.** the reason is fermat's little theorem (stated
by fermat in 1640, first published proof by euler): if `p` is prime and `m` is not a multiple
of `p`, then `m^(p-1) mod p = 1`.

`d` was chosen so that `e * d = k * phi + 1` for some whole number `k` - that is what
`(e * d) mod phi = 1` means. decryption computes:

```
c^d = m^(e*d) = m^(k*phi + 1) = (m^(p-1))^(k*(q-1)) * m
```

looked at modulo `p`, the bracket is 1 by fermat's theorem, so the whole thing equals
`m mod p`. the same argument works modulo `q`. a number below `n` that matches `m` both
modulo `p` and modulo `q` can only be `m` itself (the chinese remainder theorem). the rare
`m` that is a multiple of `p` or `q` works out too: both sides are then 0 modulo that prime.

**why the private key stays private.** everyone knows `n` and `e`, and `d` is easy to compute
WITH `phi`. but computing `phi` means knowing `p` and `q`, and recovering those from `n` is
the factoring problem. no published algorithm factors numbers of this size in practical time:
the largest rsa-style number publicly factored had 829 bits (rsa-250, in 2020, at a cost of
thousands of processor-years), and 2048 bits is far past that. two honest limits of this
statement:

- it is unproven. nobody has shown that factoring must be slow, and nobody has shown that
  breaking rsa requires factoring at all. rsa security is an assumption that has held since
  its publication in 1977, not a theorem
- it is about ordinary computers. a large quantum computer running shor's algorithm would
  factor `n` quickly; no such machine is known to exist, but this is the reason successor
  systems to rsa are being standardized

when rsa fails in practice, it is usually not the math that was attacked but the way it was
used - the weaknesses section below covers what actually goes wrong.

**why there are eight key parts.** the wasm outputs `n, e, d, p, q, dmp1, dmq1, coeff`:

- `n, e` are the public key, `d` is the private exponent, `p, q` are the primes themselves
- `dmp1 = d mod (p-1)`, `dmq1 = d mod (q-1)` and `coeff = q^-1 mod p` are pure speed helpers:
  instead of one huge `c^d mod n`, the owner of the primes computes `m1 = c^dmp1 mod p` and
  `m2 = c^dmq1 mod q` (half the size, much cheaper) and combines them with `coeff`. this
  chinese remainder trick makes decryption about 4x faster in this client

**how the primes are found.** the generator does not construct primes directly - it guesses
and tests:

1. draw a random odd number of the right size from the seeded random stream
2. test it: first divide by all 168 primes below 1000 (cheap, removes most composites), then
   run miller-rabin rounds
3. if it fails, add 2 and test the next odd number - the "+2 walk" in `src/rsa_keygen.c`
4. repeat until a number passes

about 1 in every 355 odd 1024 bit numbers is prime, so a walk typically tests a few hundred
candidates before it finds one. miller-rabin is a statistical test: it cannot PROVE a number
prime, but a composite number fools one round with a randomly picked witness at most 1 time
in 4, so a handful of rounds pushes the chance of a wrong verdict very close to zero. the
walk tests candidates at certainty 1 and re-checks the finalist at certainty 10, exactly the
verdicts jsbn produced.

**why the walk must be exact here.** in lemon chat the random stream is seeded with the
sha256 of the passphrase, so the whole search is deterministic: same passphrase, same primes,
same identity, on every machine, forever. that is why this wasm clones the old js generator
byte for byte, and why `test/README.md` is all about proving equality against recorded js
outputs.

## weaknesses

rsa breaks far more often through the way it is used than through its math. every item below
states whether it is a weakness of rsa in general or a weakness of this chat.

common, seen in real deployments:

- **weak or repeated randomness at key generation.** *general rsa - and this chat has its
  own form of it.* the general form: two keys that happen to share one prime break each
  other, because `gcd(n1, n2)` reveals the shared prime, and internet-wide scans in 2012
  found tens of thousands of such keys, mostly devices with poor randomness at first boot.
  that form does not apply here, since keys do not come from boot-time randomness at all.
  this chat's form: everything is derived from the passphrase, so key strength equals
  passphrase strength - a guessable passphrase is a weak key at any bit size
- **missing padding.** *general rsa; not present in this chat.* raw rsa is deterministic and
  malleable: equal messages give equal ciphertexts, and multiplying a ciphertext by `r^e`
  multiplies the hidden message by `r`. with `e = 3`, a short unpadded message is recovered
  by a plain cube root, and the same unpadded message sent to three recipients can be
  recovered too. this chat always applies pkcs#1 padding, which prevents all of these
- **padding oracles.** *general weakness of the pkcs#1 v1.5 padding this chat uses; no known
  opening here.* if the decrypting side reveals, even through timing or error style, whether
  a crafted ciphertext had valid padding, the ciphertext can be decrypted probe by probe
  (bleichenbacher 1998; found again in live servers as drown 2016 and robot 2017). the
  attack needs a victim that answers many probes with an observable verdict; this chat has
  no obvious such channel, but that property has not been audited
- **side channels.** *general rsa; concretely present in this chat's code.* the time, power
  or cache behavior of decryption can leak the private key to someone able to measure it
  (timing attacks published 1996, remote versions 2003). the wasm in this folder is not
  constant-time, so this chat relies on the attacker having no way to measure it - the leak
  is only usable by code running measurements on your machine while it decrypts
- **keys that are too small.** *general rsa; handled in this chat.* 512 bit keys can be
  factored for a small cloud-computing bill, a 768 bit key was publicly factored in 2009,
  and 1024 bits is widely considered end-of-life. this client only generates 2048 to 8192
  bit keys and the server can enforce a minimum size

less common, but documented:

- **primes too close together.** *general rsa; not this chat.* if `p` and `q` are nearly
  equal, fermat's factoring method splits `n` quickly. it shows up when a generator is
  buggy; here both primes are drawn independently from the random stream, and the walk is
  verified byte for byte against the reference implementation
- **structured primes.** *general rsa; not this chat.* the roca flaw (2017) came from a
  library that built primes with a special shape to save generation time; the shape made
  millions of smartcard keys factorable. this chat's walk takes no such shortcut - it is
  the plain jsbn draw-and-test, cloned exactly
- **small private exponent.** *general rsa; not this chat.* picking a small `d` to speed up
  decryption breaks the key (wiener's attack and its refinements). here `d` falls out of
  the phi arithmetic and is always on the order of `n` itself
- **partial key leakage.** *general rsa; applies to any client that holds keys in memory,
  including this one.* enough leaked bits of `p`, `q` or `d` - a memory dump, a swap file,
  a side channel - let lattice methods reconstruct the rest
- **fault attacks on crt decryption.** *general rsa, mainly a smartcard threat; no channel
  in this chat.* the fast `dmp1/dmq1/coeff` path has a known fragility: if one of its two
  half-computations goes wrong (voltage glitch, hardware fault) and the bad result is
  released, that output reveals a prime factor. this chat uses the crt path, but decryption
  output stays on the local machine and the client never signs, so a faulty result has no
  obvious way to reach an attacker
- **quantum computers.** *general rsa; hits this chat like everything else.* shor's
  algorithm would end rsa at any practical key size, if a large enough machine is ever
  built

## possible improvements

general rsa practice first:

- **oaep padding** for encryption (and pss for signatures) instead of pkcs#1 v1.5: designed
  so that a padding verdict leaks nothing useful, closing the bleichenbacher class
- **e = 65537** instead of `e = 3`: makes the small-exponent mistakes impossible rather than
  merely avoided. padded `e = 3` is not broken - this is margin, not repair. considered for
  this chat and rejected: the wire format carries only the modulus and every peer hardcodes
  "03" when encrypting (`setPublic(N, "03")` in rsa-crypto.js), so nobody could encrypt to a
  65537 key - and a different `e` changes which primes the walk accepts, so every existing
  passphrase would derive a different keypair. possible only as part of a versioned format
- **constant-time math with blinding**, and **checking crt results** before releasing them,
  against side channels and fault attacks
- **bigger keys** for identities that should last decades: 3072 bits and up
- **leaving rsa**: elliptic curves give equal security from far smaller keys with faster
  math, and post-quantum schemes (ml-kem, standardized 2024) answer shor. hybrid designs
  run a classical and a post-quantum scheme side by side

for this client specifically: the padding, `e = 3` and the key format are inherited from
cryptico and are load-bearing - every existing identity and stored message depends on them,
so changing them means a versioned wire format, not a quiet swap. what can change cheaply is
everything the wire never sees: stronger randomness for generated passphrases, enforced
minimum key sizes, and speed work inside the wasm's montgomery internals, which is free to
change as long as the verdicts and the count of random draws stay identical.
