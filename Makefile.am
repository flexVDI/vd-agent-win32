NULL =
MAINTAINERCLEANFILES =

INCLUDES =				\
	-I$(top_srcdir)/common		\
	-I$(top_srcdir)/spice-protocol	\
	$(NULL)

AM_CXXFLAGS = -flto -fwhole-program
AM_CPPFLAGS = -DUNICODE -D_UNICODE -DOLDMSVCRT
AM_LDFLAGS = -static -s

# -lversion is needed for the GetFileVersion* API which is used by vdlog.cpp
LIBS = -lversion

bin_PROGRAMS = vdagent vdservice

vdagent_LDADD = $(CXIMAGE_LIBS) vdagent_rc.$(OBJEXT)
vdagent_CXXFLAGS = $(AM_CXXFLAGS) $(CXIMAGE_CFLAGS)
vdagent_LDFLAGS = $(AM_LDFLAGS) -Wl,--subsystem,windows
vdagent_SOURCES =			\
	common/vdcommon.h		\
	common/vdlog.cpp		\
	common/vdlog.h			\
	vdagent/desktop_layout.cpp	\
	vdagent/desktop_layout.h	\
	vdagent/display_setting.cpp	\
	vdagent/display_setting.h	\
	vdagent/vdagent.cpp		\
	$(NULL)

vdagent_rc.$(OBJEXT): vdagent/vdagent.rc
	$(AM_V_GEN)$(WINDRES) -i $< -o $@

MAINTAINERCLEANFILES += vdagent_rc.$(OBJEXT)

vdservice_LDADD = -lwtsapi32 vdservice_rc.$(OBJEXT)
vdservice_SOURCES =			\
	common/stdint.h			\
	common/vdcommon.h		\
	common/vdlog.cpp		\
	common/vdlog.h			\
	vdservice/pci_vdi_port.cpp	\
	vdservice/pci_vdi_port.h	\
	vdservice/vdi_port.cpp		\
	vdservice/vdi_port.h		\
	vdservice/vdservice.cpp		\
	vdservice/virtio_vdi_port.cpp	\
	vdservice/virtio_vdi_port.h	\
	$(NULL)

vdservice_rc.$(OBJEXT): vdservice/vdservice.rc
	$(AM_V_GEN)$(WINDRES) -i $< -o $@

MAINTAINERCLEANFILES += vdservice_rc.$(OBJEXT)


EXTRA_DIST =			\
	tests/clipboard.py	\
	vdagent.sln		\
	vdagent/resource.h	\
	vdagent/vdagent.rc	\
	vdagent/vdagent.vcproj	\
	vdservice/vdservice.rc	\
	vdservice/vdservice.vcproj


-include $(top_srcdir)/git.mk
