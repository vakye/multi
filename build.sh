#!/bin/bash

if [ ! -d build ]; then
    mkdir -p build;
fi

SourceFile="code/linux_main.c"
OutputFile="build/multi"

Compiler="clang"

CompileFlags=" \
    -g \
    -O0 \
    -ffreestanding \
    -fpie \
    -fno-stack-protector \
    -nostdlib \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-unused-function \
    -Wno-unused-but-set-variable \
    -o $OutputFile"

LinkFlags=" \
    -fuse-ld=lld \
    -Wl,-nostdlib \
    -Wl,--entry,EntryPoint"

$Compiler $CompileFlags $SourceFile $LinkFlags

