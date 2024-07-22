Release Notes for v0.30

## What's Changed
* Avoid breakages for WITH_LLVM builds with libprotobuf < 3.19.0 by @dhoekwater in https://github.com/google/autofdo/pull/200
* Fixed testcase failures when build directory is not under the source directory by @shenhanc78 in https://github.com/google/autofdo/pull/206
* Fix compilation error when building using github build bot. by @shenhanc78 in https://github.com/google/autofdo/pull/210
* Removed WITH_LLVM and replaced protobuf_generate_cpp with more modern protobuf_generate by @shenhanc78 in https://github.com/google/autofdo/pull/199
* Fix format/indention error in README. by @shenhanc78 in https://github.com/google/autofdo/pull/212
* Add a config to build and run on PRs. by @snehasish in https://github.com/google/autofdo/pull/213
* Update README.md by @snehasish in https://github.com/google/autofdo/pull/214
* Add a workflow to create release build. by @shenhanc78 in https://github.com/google/autofdo/pull/217
* Build full-static LLVM tools binaries by @shenhanc78 in https://github.com/google/autofdo/pull/216

**Full Changelog**: https://github.com/google/autofdo/compare/v0.20.1...v0.30
