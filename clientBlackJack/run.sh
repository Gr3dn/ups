#!/usr/bin/env bash
# Папка скрипта
DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1 && pwd)"
CMD="java --module-path \"$DIR/target/lib\" --add-modules=javafx.controls -jar \"$DIR/target/blackjack-game-1.0-SNAPSHOT.jar\""
echo "$CMD"
eval $CMD
