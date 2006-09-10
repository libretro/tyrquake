-----------
 Tyr-Quake
-----------

Date:		2006-02-12
Version: 	0.49
Author:		Kevin Shanahan (aka. Tyrann)
Webpage:	http://disenchant.net
email:		tyrann@disenchant.net

Why?
----
This is meant to be a very conservative branch of the Quake source code. I
intend to support Quake and Quakeworld in both software and GL versions, as
well as Quakeworld Server; all on both MS Windows and Linux. I don't intend on
adding lots of rendering enhancements, but rather fixing little bugs that I've
come across over the years. I'll be adding small enhancements, but I don't
want to completely overhaul the engine.

Building:
---------
The build process is a little ugly, but pretty straight forward. Just open up
the Makefile in the tyrquake directory and set the TARGET_APP, TARGET_RENDERER
and TARGET_OS variables. Then type "make". It works for me under Linux and
Windows (using MinGW with MSYS - http://www.mingw.org). I only have x86
machines to test on, but I'd like to hear if anyone gets it working on
something else...


Version History:

v0.49
=====
- Better fix for glXGetProcAddress ABI issues on Linux
- Add "maplist" command - lists maps in the current path(s)
- Enable command completion after ';' on a line
- Fix problem with really long GL extension strings (e.g. NVidia/Linux)

v0.48
=====
- Save mlook state to config.cfg
- Make mousewheel work in Linux
- Make CD volume control work in Linux
- Make gamma controls work in Linux/Windows GLQuake
- Thanks to Stephen A for supplying the patches used as a basis for the above
  (and Ozkan for some of the original work)

v0.47
=====
- Add fullscreen modes to software quake in Linux
- Added r_drawflat to glquake, glqwcl
- Fixed r_waterwarp in glquake (though it still looks crap)
- Multitexture improvements (sky, also usable with gl_texsort 1)
- Add rendering of collision hulls (via cvar _gl_drawhull for now)

v0.46
=====
- Fixed default vidmodes in windows, software NQ/QW (broken in v0.0.39 I think)
- Fixed sound channel selection broken in v0.45
- Fixed scaling of non-default sized console backgrounds

v0.45
=====
- Changed to a simpler version numbering system (fits the console better too!)
- Makefile tweaks; can optionally build with no intel asm files now.
- Started moving around bits of the net code. No behaviour changes intended.
- Con_Printf only triggers screen updates with "\n" now.
- Various other aimless code cleanups (comments, preprocessor bits)

v0.0.44
=======
- Fix the previous SV_TouchLinks fix (oops!)
- Make AllocBlock more efficient for huge maps

v0.0.43
=======
- Fixed a rare crash in SV_TouchLinks

v0.0.42
=======
- Increased max verticies/triangles on mdls

v0.0.41
=======
- fixed marksurfaces overflow in bsp loading code (fixes visual corruption on
  some very large maps)

v0.0.40
=======
- added the high-res modes to the QW software renderer as well
- fixed a rendering bug when cl_bobcycle was set to zero

v0.0.39
=======
- Hacked in support for higher res windowed modes in software renderer. Only in
  NQ for now, add to QW later.
- gl_model.c now a shared file
- Random cleanups

v0.0.38
=======
- Fixed a corruption/crash bug in model.c/gl_model.c bsp loading code.

v0.0.37
=======
- Cleaned up the tab-completion code a bit

v0.0.36 (and earlier)
=======
- Re-indent code to my liking
- Make changes to compile using gcc/mingw/msys
- Fix hundreds of warnings spit out by the compiler
- Lots of work on eliminating duplication of code (much more to do too!)
- Tried to reduce the enormous number of exported variables/functions.
- Fixed some of the input handling under Linux...
- Fixed initialisation order of OSS sound driver
- Hacked a max texture size detection fix in (should be using proxy textures?)
- Replaced SGIS multitexturing with ARB multitexture
- Added cvars "r_lockpvs" and "r_lockfrustum"
- Enhanced the console tab completion
- Bumped the edict limit up to 2048; various other limits bumped also...
- lots of other trivial things I've probably completely forgotten in the many
  months I've been picking over the code trying to learn more about it
