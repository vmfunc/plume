# plume workflows. recipes work inside the dev shell and out (they wrap
# themselves in `nix develop` when a tool is missing). `just` alone lists them.

_dev := if env_var_or_default("IN_NIX_SHELL", "") != "" { "" } else { "nix develop -c " }

# list recipes
default:
    @just --list

# enter the dev shell and install the git hooks
setup:
    nix develop -c bash -c 'git config core.hooksPath .githooks 2>/dev/null || true; echo "ready. run: just build"'

# configure + compile (debug)
build:
    {{_dev}}bash -c 'cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build'

# optimized build
release:
    {{_dev}}bash -c 'cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release'

# run the tui against your real config
run *ARGS:
    {{_dev}}bash -c 'cmake --build build >/dev/null && ./build/plume {{ARGS}}'

# unit + fixture + golden tests
test:
    {{_dev}}bash -c 'cmake --build build --target plume_tests && ./build/plume_tests'

# asan + ubsan profile
test-san:
    {{_dev}}bash -c 'cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" && cmake --build build-san --target plume_tests && ./build-san/plume_tests'

# static analysis
lint:
    {{_dev}}bash -c 'cmake --build build >/dev/null && run-clang-tidy -p build src'

# format the tree
fmt:
    {{_dev}}bash -c 'git ls-files "*.cpp" "*.hpp" | xargs clang-format -i'

# check formatting without touching files
fmt-check:
    {{_dev}}bash -c 'git ls-files "*.cpp" "*.hpp" | xargs clang-format --dry-run -Werror'

# the pre-commit gate: format, lint, test, build
check: fmt-check lint test build

# wipe build dirs
clean:
    rm -rf build build-release build-san

# install into the nix profile
install:
    nix profile install .

# scaffold a plugin
plugin-new name:
    {{_dev}}bash -c 'scripts/plugin-new.sh {{name}}'

# scripted conversation against the mock provider, for screenshots
demo:
    {{_dev}}bash -c 'cmake --build build >/dev/null && PLUME_MOCK=1 ./build/plume demo'

# start fresh: drop the local database
db-reset:
    rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/plume/plume.sqlite"*
