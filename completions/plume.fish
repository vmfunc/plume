# fish completions for plume

set -l cmds ask export import themes doctor config

# only import completes files
complete -c plume -f

complete -c plume -n "not __fish_seen_subcommand_from $cmds" -a ask -d 'ask one question and print the answer'
complete -c plume -n "not __fish_seen_subcommand_from $cmds" -a export -d 'write a conversation to stdout'
complete -c plume -n "not __fish_seen_subcommand_from $cmds" -a import -d 'import conversations from a file'
complete -c plume -n "not __fish_seen_subcommand_from $cmds" -a themes -d 'list installed themes'
complete -c plume -n "not __fish_seen_subcommand_from $cmds" -a doctor -d 'check the environment'
complete -c plume -n "not __fish_seen_subcommand_from $cmds" -a config -d 'manage configuration'

# ask
complete -c plume -n '__fish_seen_subcommand_from ask' -l model -x -d 'model to use for this question'
complete -c plume -n '__fish_seen_subcommand_from ask' -l json -d 'print the full response as json'
complete -c plume -n '__fish_seen_subcommand_from ask' -l no-stream -d 'wait for the complete answer before printing'

# export
complete -c plume -n '__fish_seen_subcommand_from export' -l format -x -a 'md json html' -d 'output format'

# import
complete -c plume -n '__fish_seen_subcommand_from import' -F

# config
complete -c plume -n '__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from edit' -a edit -d 'open the config in $EDITOR'
