MODULE_NAME := app
# util.c is the Alerts wapp's: strings, time, JSON and the section 5
# identifier, with no libc. One copy, built into both.
MODULE_SRCS := main.c ci.c ../alerts/util.c

include ../sdk/Makefile.common

CFLAGS += -I../alerts
