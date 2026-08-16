# Changelog

All notable changes to MMIDownloader are recorded in this file. Versions follow
[Semantic Versioning](https://semver.org/).

## [0.1.1] - 2026-08-16

- Added `VERSION` as the single version source for CMake and release packaging.
- Fixed clean-checkout builds that previously depended on an ignored zlib
  `zconf.h` file.
- Excluded generated releases, local configuration, IDE state, and build
  artifacts from Git.
- Removed stale internal notes and documentation for the separate Release
  Indexer project.
- Expanded build, configuration, licensing, and release documentation.

## [0.1.0] - 2026-07-30

- Published the initial native Windows GUI release for x64 and x86.
- Added local, HTTP/HTTPS, WebDAV, and anonymous FTP catalog sources.
- Added resumable downloads, package checksum validation, archive extraction,
  folder output, and removable-drive preparation.
- Added Apache License 2.0 and third-party notices for zlib and bzip2.
