#!/bin/sh
# Requires a host C++ compiler; no devkitARM or libctru installation is needed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/daedalus-ctr-host.XXXXXXXX")
# Only remove the directory created above, never an unchecked path.
case "$build_dir" in
    /*/daedalus-ctr-host.*) ;;
    *) echo "Unexpected temporary directory: $build_dir" >&2; exit 1 ;;
esac
trap 'rm -rf -- "$build_dir"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++11 -Wall -Wextra ${CXXFLAGS:-} \
    -I"$root/tests/ctr_host/include" -I"$root/Source/SysCTR/Include" -I"$root/Source" -I"$root/Source/Config/Release" \
    "$root/tests/ctr_host/test_ctr_host.cpp" \
    "$root/Source/SysCTR/Utility/ThreadCTR.cpp" \
    "$root/Source/SysCTR/Utility/TimingCTR.cpp" \
    "$root/Source/SysCTR/UI/UITheme.cpp" \
    "$root/Source/SysCTR/UI/UITouchInput.cpp" \
    "$root/Source/SysCTR/UI/TouchControllerCTR.cpp" \
    "$root/Source/SysCTR/UI/CoverScraperCTR.cpp" \
    ${LDFLAGS:-} -o "$build_dir/ctr_host_tests"
"$build_dir/ctr_host_tests"
