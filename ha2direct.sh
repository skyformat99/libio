#!/bin/sh

exec io hadirect -l unix:@ha2 \
	+led=oled \
	+zolder=ozolder izolder \
	+fan=ofan ibad1 \
	+lavabo=olavabo ihead2 \
	+bad=obad ibad4 \
	+bluebad=oblubad \
	+main=omain \
	+blueled=obluslp \
	+hal=ohal igang1
