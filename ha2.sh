#!/bin/sh

POWERLED=/sys/class/leds/power

case "$1" in
services)
	echo "direct veluxg fancy abort poweroff"
	;;
direct)
	# main io switches to control outputs
	exec io hadirect -l unix:@ha2 \
		+led=oled \
		+zolder=ozolder izolder \
		+fan=ofan ibad1 \
		+lavabo=olavabo \
		+bad=obad ibad4 \
		+bluebad=oblubad \
		+main=omain \
		+blueled=obluslp \
		+hal=ohal igang1
	;;
veluxg)
	# velux gordijn
	exec io hamotor -lunix:@veluxg -i igang4 -i iwest2 -i izuid2 +hi=oveluxhg +lo=oveluxlg
	;;
fancy)
	exec io ha2addons -l unix:@ha2+
	;;
abort)
	exec io haspawn -d2 SW1 poweroff
	;;
*)
	echo "usage: $0 [`$0 services`]"
	exit 1
	;;
esac
