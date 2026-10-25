# Built inside the Sailfish SDK engine by ../scripts/build-sailfish.sh.
all: indraw

indraw: indraw.c controller.c controller.h ctl_exposure.c ctl_exposure.h ctl_focus.c ctl_focus.h ctl_view.c ctl_view.h ctl_shoot.c ctl_shoot.h ../indraw_version.h
	$(CC) -std=c11 -O2 -g -Wall -Wextra -o $@ indraw.c controller.c ctl_exposure.c ctl_focus.c ctl_view.c ctl_shoot.c -ldl -lpthread -lm

clean:
	rm -f indraw

.PHONY: all clean
