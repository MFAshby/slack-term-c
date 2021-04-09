#!/bin/bash

function cleanup() {
	pkill slack-term-c
	kill $tail_pid
}

trap cleanup EXIT

for (( ; ; ))
do
	./build.sh
	build_result=$?
	if [ $build_result -eq 0 ]; then
		true > dbg.log
		true > err.log

		pkill slack-term-c
		(alacritty -e ./slack-term-c && cat err.log)&

		kill $tail_pid
		tail -f ./dbg.log&
		tail_pid=$!
	fi

	# Wait for file changes, clear the terminal, and build
	inotifywait -q -r -e create,modify,move,delete *.h *.c && \
	  echo -ne "\033c"
done