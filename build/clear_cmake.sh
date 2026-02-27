#!/bin/bash

# Get the script's own file name, to prevent suicide
SCRIPT_NAME=$(basename "$0")


find . -maxdepth 1 ! -path . ! -path "./_deps*" ! -name "$SCRIPT_NAME" -exec rm -rf {} +
