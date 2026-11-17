#!/bin/sh
# usage: summarize_errors.sh <target> [N]  - builds a target and groups MSVC errors
cd "$(dirname "$0")"
cmd //c "build_target.cmd $1 > build_$1.log 2>&1"
grep -E "error C|error LNK" build_$1.log | sed -E 's/ \[[^]]*vcxproj\]//' | sort -u > build_$1.errors
echo "errors: $(wc -l < build_$1.errors)"
awk -F': (fatal )?error ' '{print $2}' build_$1.errors | sort | uniq -c | sort -rn | head -${2:-50}
