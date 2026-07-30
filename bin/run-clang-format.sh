#!/bin/bash

if command -v clang-format-19 &> /dev/null; then
    CLANG_FORMAT=clang-format-19
elif command -v clang-format &> /dev/null; then
    CLANG_FORMAT=clang-format
else
    echo "Installing clang-format as a linter..."
    if [[ "$(uname)" == "Linux" ]]; then
        sudo apt install -y clang-format-19
    elif [[ "$(uname)" == "Darwin" ]]; then
        brew install clang-format
    else
        echo "WARNING: installing the linter is not yet supported outside of Linux and Mac."
    fi

    if command -v clang-format-19 &> /dev/null; then
        CLANG_FORMAT=clang-format-19
    elif command -v clang-format &> /dev/null; then
        CLANG_FORMAT=clang-format
    else
        echo "ERROR: clang-format installation failed"
        exit 1
    fi
fi

find ./src -type f \( -name \*.cpp -o -name \*.hpp \) | xargs "$CLANG_FORMAT" -i --style=file "$@"
