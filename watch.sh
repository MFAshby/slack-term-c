#!/bin/bash

function cleanup() {
	kill $st_pid
	kill $tail_pid
}

trap cleanup EXIT

for (( ; ; ))
do
	./build.sh 2>&1
	true > dbg.log
	true > err.log

	kill $st_pid
	(alacritty -e ./slack-term-c && cat err.log)&
	st_pid=$!

	kill $tail_pid
	tail -f ./dbg.log&
	tail_pid=$!

	# Wait for file changes, clear the terminal, and build
	inotifywait -q -r -e create,modify,move,delete *.h *.c && \
	  echo -ne "\033c"
done

# Re-exec ourselves for the next edit
exec $0 $@
