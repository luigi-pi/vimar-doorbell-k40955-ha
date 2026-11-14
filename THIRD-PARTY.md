# Third-party software in the ready-made programs

The kit zip on the Releases page contains three programs in `dist/`, built by
[`build.sh`](build.sh) (see [.github/workflows/build.yml](.github/workflows/build.yml)).
They include code from other projects:

| Program | Contains | Licence |
|---|---|---|
| `dropbearmulti` | [Dropbear SSH](https://matt.ucc.asn.au/dropbear/dropbear.html) 2025.89, with LibTomCrypt and LibTomMath | MIT-style (Dropbear `LICENSE`); public domain / Unlicense (LibTom) |
| `vimarbridge` | [BearSSL](https://bearssl.org/) 0.6 | MIT |
| all three | GNU C Library (glibc), linked statically | LGPL-2.1-or-later |

The exact sources are named, with their checksums, in `build.sh`; the two small changes made to
Dropbear are in the same file. Everything can be rebuilt (and relinked against another C library)
with `./build.sh` — see [docs/building.md](docs/building.md).

`home-assistant/quii_lan.py` is a reimplementation of the LAN path of
[quii-helper](https://github.com/xaer981/quii-helper) (MIT); its notice is kept in [LICENSE](LICENSE).
