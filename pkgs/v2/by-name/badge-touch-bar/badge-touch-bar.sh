#!/usr/bin/env bash
# Entries are label=command, one per line. Sway joins status_command arguments.
set -u
labels=() commands=() deadline=()
while IFS= read -r entry; do
    labels+=("${entry%%=*}") commands+=("${entry#*=}") deadline+=(0)
done <"$2"
((${#labels[@]})) || exit 1
selected=0 w=0 h=0 scale=1000000 turn=normal
while IFS= read -r line; do
    if [[ $line == Output* ]]; then
        selected=0
        [[ $line == "Output $1 "* ]] && selected=1
    fi
    ((selected)) || continue
    if [[ $line =~ Current\ mode:\ ([0-9]+)x([0-9]+) ]]; then
        w=${BASH_REMATCH[1]} h=${BASH_REMATCH[2]}
    elif [[ $line =~ Scale\ factor:\ ([0-9]+)\.([0-9]+) ]]; then
        fraction=${BASH_REMATCH[2]}000000
        scale=$((10#${BASH_REMATCH[1]} * 1000000 + 10#${fraction:0:6}))
    elif [[ $line =~ Transform:\ (.*) ]]; then
        turn=${BASH_REMATCH[1]}
    fi
done < <(swaymsg -p -t get_outputs)
[[ $turn == *90 || $turn == *270 ]] && w=$h
((w > 0 && scale > 0)) || exit 1
width=$(((w * 1000000 / scale - 6) / ${#labels[@]}))
((width > 0)) || exit 1
render() {
    printf '['
    local i label sep=
    for i in "${!labels[@]}"; do
        label=${labels[i]}
        ((deadline[i] > SECONDS)) && label='Starting…'
        label=${label//\\/\\\\}; label=${label//\"/\\\"}
        printf '%s{"name":"touch","instance":"%d","full_text":"%s","min_width":%d,"align":"center","separator":false,"separator_block_width":0}' "$sep" "$i" "$label" "$width"
        sep=,
    done
    printf '],\n'
}
printf '{"version":1,"click_events":true}\n[\n'
render
while :; do
    if IFS= read -r -t 0.2 click; then
        if [[ $click =~ \"instance\"[[:space:]]*:[[:space:]]*\"([0-9]{1,4})\" ]]; then
            i=$((10#${BASH_REMATCH[1]}))
            if ((i < ${#labels[@]} && deadline[i] <= SECONDS)); then
                deadline[i]=$((SECONDS + 5))
                render
                swaymsg exec -- "${commands[i]}" >/dev/null || true
            fi
        fi
    else
        (($? > 128)) || break
    fi
    changed=0
    for i in "${!labels[@]}"; do
        if ((deadline[i] > 0 && deadline[i] <= SECONDS)); then
            deadline[i]=0 changed=1
        fi
    done
    ((changed)) && render
done
