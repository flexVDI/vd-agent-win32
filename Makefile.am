NULL =
MAINTAINERCLEANFILES =

DIST_SUBDIRS = spice-protocol

AM_CXXFLAGS = -flto -fwhole-program
AM_CPPFLAGS = 				\
	-DUNICODE 			\
	-D_UNICODE			\
	-DOLDMSVCRT			\
	-I$(top_srcdir)/common		\
	-I$(top_srcdir)/spice-protocol	\
	$(NULL)

AM_LDFLAGS = -static
if !ENABLE_DEBUG
AM_LDFLAGS += -s
endif

# -lversion is needed for the GetFileVersion* API which is used by vdlog.cpp
LIBS = -lversion

bin_PROGRAMS = vdagent vdservice

vdagent_LDADD = -lwtsapi32 $(CXIMAGE_LIBS) vdagent_rc.$(OBJEXT)
vdagent_CXXFLAGS = $(AM_CXXFLAGS) $(CXIMAGE_CFLAGS)
vdagent_LDFLAGS = $(AM_LDFLAGS) -Wl,--subsystem,windows
vdagent_SOURCES =			\
	common/vdcommon.cpp             \
	common/vdcommon.h		\
	common/vdlog.cpp		\
	common/vdlog.h			\
	vdagent/desktop_layout.cpp	\
	vdagent/desktop_layout.h	\
	vdagent/display_setting.cpp	\
	vdagent/display_setting.h	\
	vdagent/file_xfer.cpp		\
	vdagent/file_xfer.h		\
	vdagent/vdagent.cpp		\
	vdagent/as_user.cpp		\
	vdagent/as_user.h		\
	$(NULL)

vdagent_rc.$(OBJEXT): vdagent/vdagent.rc
	$(AM_V_GEN)$(WINDRES) -i $< -o $@

MAINTAINERCLEANFILES += vdagent_rc.$(OBJEXT)

vdservice_LDADD = -lwtsapi32 vdservice_rc.$(OBJEXT)
vdservice_SOURCES =			\
	common/stdint.h			\
	common/vdcommon.cpp             \
	common/vdcommon.h		\
	common/vdlog.cpp		\
	common/vdlog.h			\
	vdservice/vdservice.cpp		\
	$(NULL)

vdservice_rc.$(OBJEXT): vdservice/vdservice.rc
	$(AM_V_GEN)$(WINDRES) -i $< -o $@

MAINTAINERCLEANFILES += vdservice_rc.$(OBJEXT)


EXTRA_DIST =			\
	$(top_srcdir)/.version	\
	tests/clipboard.py	\
	vdagent.sln		\
	vdagent/resource.h	\
	vdagent/vdagent.rc	\
	vdagent/vdagent.vcproj	\
	vdservice/resource.h	\
	vdservice/vdservice.rc	\
	vdservice/vdservice.vcproj

# see git-version-gen
dist-hook:
	echo $(VERSION) > $(distdir)/.tarball-version

BUILT_SOURCES = $(top_srcdir)/.version
$(top_srcdir)/.version:
	echo $(VERSION) > $@-t && mv $@-t $@



-include $(top_srcdir)/git.mk
