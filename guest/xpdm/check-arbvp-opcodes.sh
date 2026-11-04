#!/bin/sh

set -eu

source_file=${1:-qxpmp/qxpmp.c}

check_program()
{
    program=$1
    source=$(sed -n "/static const UCHAR ${program}\\[\\]/,/^[[:space:]]*\"END\\\\n\";/p" "$source_file")
    tokens=$(printf '%s\n' "$source" |
        sed -n 's/^[[:space:]]*"\([A-Z][A-Z0-9]*\)[[:space:]].*/\1/p')
    result_sources=$(printf '%s\n' "$source" |
        sed -n '/^[[:space:]]*"[A-Z][A-Z0-9]*[[:space:]]/ {
            s/^[^,]*,//
            /result\./p
        }')
    count=0

    if [ -z "$tokens" ]; then
        echo "ARBvp check: could not find $program in $source_file" >&2
        return 1
    fi
    if [ -n "$result_sources" ]; then
        echo "ARBvp check: $program reads a write-only result register" >&2
        printf '%s\n' "$result_sources" >&2
        return 1
    fi
    for token in $tokens; do
        case "$token" in
            OPTION|PARAM|ATTRIB|OUTPUT|ALIAS|TEMP|ADDRESS)
                ;;
            ABS|ADD|ARL|DP3|DP4|DPH|DST|EX2|EXP|FLR|FRC|LG2|LIT|LOG|MAD|MAX|MIN|MOV|MUL|POW|RCP|RSQ|SGE|SLT|SUB|SWZ|XPD)
                count=$((count + 1))
                ;;
            *)
                echo "ARBvp check: $program uses unsupported opcode $token" >&2
                return 1
                ;;
        esac
    done
    if [ "$count" -gt 128 ]; then
        echo "ARBvp check: $program has $count instructions (portable limit 128)" >&2
        return 1
    fi
    echo "ARBvp check: $program $count/128 instructions"
}

check_program qxp_d3d_texture_q_program
check_program qxp_d3d_compact_transformed_program
check_program qxp_d3d_positional_range_program
check_program qxp_d3d_dual_point_specular_program
check_program qxp_d3d_quad_point_range_program
check_program qxp_d3d_multi_point_range_program
