#compdef plume
# zsh completions for plume

_plume() {
    local curcontext="$curcontext" state line
    local -a subcmds
    subcmds=(
        'ask:ask one question and print the answer'
        'export:write a conversation to stdout'
        'import:import conversations from a file'
        'themes:list installed themes'
        'doctor:check the environment'
        'config:manage configuration'
    )

    _arguments -C \
        '1: :->cmd' \
        '*:: :->args'

    case $state in
        cmd)
            _describe 'command' subcmds
            ;;
        args)
            case $words[1] in
                ask)
                    _arguments \
                        '--model=[model to use for this question]:model' \
                        '--json[print the full response as json]' \
                        '--no-stream[wait for the complete answer before printing]' \
                        '1:question'
                    ;;
                export)
                    _arguments \
                        '--format=[output format]:format:(md json html)' \
                        '1:conversation'
                    ;;
                import)
                    _arguments '1:file:_files'
                    ;;
                config)
                    _values 'action' 'edit[open the config in $EDITOR]'
                    ;;
            esac
            ;;
    esac
}

_plume "$@"
