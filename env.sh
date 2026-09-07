#!/bin/bash

export PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export CODEGEN_DIR=test/compiler

# Prefer compiler and mapper sources from this checkout
export PYTHONPATH="$PROJECT_ROOT/voyager-compiler/src:$PROJECT_ROOT/interstellar/src${PYTHONPATH:+:$PYTHONPATH}"
