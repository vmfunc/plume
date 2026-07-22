# bash completions for plume

_plume() {
    local cur prev words cword
    if type _init_completion >/dev/null 2>&1; then
        _init_completion || return
    else
        cur=${COMP_WORDS[COMP_CWORD]}
        prev=${COMP_WORDS[COMP_CWORD-1]}
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    fi

    local subcmds="ask export import themes doctor config"

    if ((cword == 1)); then
        COMPREPLY=($(compgen -W "$subcmds" -- "$cur"))
        return
    fi

    case ${words[1]} in
        ask)
            # --model takes a free-form value; nothing sensible to offer
            [[ $prev == --model ]] && return
            COMPREPLY=($(compgen -W "--model --json --no-stream" -- "$cur"))
            ;;
        export)
            if [[ $prev == --format ]]; then
                COMPREPLY=($(compgen -W "md json html" -- "$cur"))
                return
            fi
            COMPREPLY=($(compgen -W "--format" -- "$cur"))
            ;;
        import)
            COMPREPLY=($(compgen -f -- "$cur"))
            ;;
        config)
            ((cword == 2)) && COMPREPLY=($(compgen -W "edit" -- "$cur"))
            ;;
    esac
}

complete -F _plume plume
