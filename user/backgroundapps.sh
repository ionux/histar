#!/bin/sh
if [ "$1" == "" ]; then
	echo "usage: $0 uW"
	exit 1
fi
energywrap $1 /bin/rsstest.sh &
energywrap $1 /bin/mmtest.sh
