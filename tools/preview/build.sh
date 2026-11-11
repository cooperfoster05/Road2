#!/bin/bash
# compile changed plugin sources to obj/
cd /home/claude/h
DIRS=$(find sdk/sdkfix -type d | sed 's#^#-I#' | tr '\n' ' ')
FL="-std=c++20 -O1 -w -fpermissive -include sdk/shim/shim.h -Isdk/shim -Isrc -Isdk/sdkfix $DIRS -Isrc/IMGUI -Isrc/third_party/RocketSim/src -Isrc/third_party/miniz -Isrc/third_party"
cc1() { f=$1; o=obj/$(basename ${f%.cpp}).o; if [ ! -e $o ] || [ $f -nt $o ] || [ -n "$FORCE" ]; then g++ $FL -c $f -o $o 2> obj/$(basename $f).err || { echo "FAIL $f"; grep -m6 error obj/$(basename $f).err; }; fi; }
export -f cc1; export FL
ls "$@" | xargs -P 2 -I{} bash -c 'cc1 {}'
