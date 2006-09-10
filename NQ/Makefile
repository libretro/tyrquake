# ------------
# NQ Makefile - to be called from the main makefile
# ------------

# Not quite ready for buildirs, etc.
# More seperation required:
# - Core engine
# - Renderer
# - System

.PHONY:	default \
	quake-sw-win32 quake-gl-win32 quake-sw-linux quake-gl-linux

# Rules to compile stuff from the common dir...
%.o:	../common/%.S
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	../common/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

# A special rule for the glx file due to compatibility problems
# with the Nvidia's libGL.so
#gl_vidlinuxglx.o:	../common/gl_vidlinuxglx.c
#	$(CC) -c $(CPPFLAGS) $(CFLAGS) -DglXGetProcAddress=glXGetProcAddressARB -o $@ $^

# Rule to make the resource file
%.res:	%.rc
	windres -i $^ -O coff -o $@

# Don't build anything by default
default:

quake-sw-win32:	$(NQ_W32_SW_OBJS)

quake-gl-win32:	$(NQ_W32_GL_OBJS)

quake-sw-linux:	$(NQ_LINUX_SW_OBJS)

quake-gl-linux: $(NQ_LINUX_GL_OBJS)
