# ------------
# QW Makefile - to be called from the main makefile
# ------------

# Not quite ready for buildirs, etc.
# More seperation required:
# - Core engine
# - Renderer
# - System

.PHONY:	default \
	qwcl-sw-win32 qwcl-gl-win32 qwcl-sw-linux qwcl-gl-linux

# Rules to compile stuff from the common dir...
%.o:	../../common/%.S
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../../common/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../common/%.S
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../common/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

# Rule to make the resource file
%.res:	%.rc
	windres -i $^ -O coff -o $@

# Don't build anything by default
default:

qwcl-sw-win32:	$(QW_W32_SW_OBJS)

qwcl-gl-win32:	$(QW_W32_GL_OBJS)

qwcl-sw-linux:	$(QW_LINUX_SW_OBJS)

qwcl-gl-linux:	$(QW_LINUX_GL_OBJS)
