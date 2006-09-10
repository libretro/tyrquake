# --------------
# QWSV Makefile - to be called from the main makefile
# --------------

# Not quite ready for buildirs, etc.
# More seperation required:
# - Core engine
# - Renderer
# - System

.PHONY:	default \
	qwsv-win32 qwsv-linux

# Rules to compile stuff from the common dir...
%.o:	../../common/%.S
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../../common/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../common/%.S
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../common/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

# Don't build anything by default
default:

qwsv-win32:	$(QWSV_W32_OBJS)

qwsv-linux:	$(QWSV_LINUX_OBJS)
