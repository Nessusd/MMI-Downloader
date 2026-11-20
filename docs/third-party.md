# Third-party source

The project vendors small C libraries as source and builds them into the executable statically. No zlib/bzip2 runtime DLLs are required by `MMIDownloader.exe`.

## Recorded provenance

### zlib

- Tree: `third_party/zlib`
- In-tree declared version: `1.3.2` (`ZLIB_VERSION` in `zlib.h`)
- Upstream tag: `v1.3.2`
- Verified commit: `216c70c020aa53f0c40920d155f808b6b59c9acb`
- Verification archive: `https://github.com/madler/zlib/archive/refs/tags/v1.3.2.zip`
- Verification archive SHA-256: `31fd9fee98812abcf147d0e103bc4d2f983c35a8d7a807a328a299f3a74e0050`
- License: `third_party/zlib/LICENSE`

On 2026-07-12 all 49 files vendored at the root of `third_party/zlib`
matched the same-name files from that verification archive byte-for-byte. The
digest above is for the archive downloaded during this verification; the
original historical import archive was not retained.

### bzip2/libbzip2

- Tree: `third_party/bzlib`
- In-tree declared version: `1.0.8` of 13 July 2019 (`third_party/bzlib/README`)
- Canonical project/release reference: `https://sourceware.org/bzip2/` (`1.0.8`)
- Verified mirror commit: `https://github.com/ImageMagick/bzip2/commit/abffe764f875f71d051efb19d4c83139375f82d7`
- Verification archive: `https://codeload.github.com/ImageMagick/bzip2/zip/abffe764f875f71d051efb19d4c83139375f82d7`
- Verification archive SHA-256: `7edce89fdcab36e334cc4c6fc89027bf914f47fe8706cf85a96638386beb0aed`
- License: `third_party/bzlib/LICENSE`

On 2026-07-12 all 56 files vendored at the root of `third_party/bzlib`
matched the same-name files from the pinned mirror commit byte-for-byte. The
former `ImageMagick/bzlib` repository name redirects to
`ImageMagick/bzip2`; no mutable branch URL is used as provenance. The digest
above is for the archive downloaded during this verification; the original
historical import archive was not retained.

## Update requirements

For every future third-party refresh, record all of the following in the same change:

1. canonical upstream project and download URL;
2. immutable tag or commit ID;
3. SHA-256 of the downloaded archive;
4. declared library version and license-file review;
5. clean x64 and x86 Release build results plus GUI smoke checks.

Do not refresh from a mutable branch without pinning the resolved commit and archive digest.
