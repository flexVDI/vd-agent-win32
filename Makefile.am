INCLUDES =				\
	-I$(top_srcdir)/common		\
	$(SPICE_PROTOCOL_CFLAGS)

bin_PROGRAMS = vdagent vdservice

vdagent_SOURCES =			\
	common/vdcommon.h		\
	common/vdlog.cpp		\
	common/vdlog.h			\
	vdagent/desktop_layout.cpp	\
	vdagent/desktop_layout.h	\
	vdagent/display_setting.cpp	\
	vdagent/display_setting.h	\
	vdagent/vdagent.cpp

vdservice_SOURCES =			\
	common/stdint.h			\
	common/vdcommon.h		\
	common/vdlog.cpp		\
	common/vdlog.h			\
	vdservice/vdservice.cpp		\
	vdservice/pci_vdi_port.cpp	\
	vdservice/pci_vdi_port.h	\
	vdservice/vdi_port.h		\
	vdservice/vdi_port.cpp		\
	vdservice/virtio_vdi_port.h	\
	vdservice/virtio_vdi_port.cpp

# -lversion is needed for the GetFileVersion* API which is used by
# vdlog.cpp
LIBS = -lversion
vdservice_LDADD = -lwtsapi32
vdagent_LDADD = $(CXIMAGE_LIBS)
vdagent_CXXFLAGS = $(CXIMAGE_CFLAGS)

-include $(top_srcdir)/git.mk
