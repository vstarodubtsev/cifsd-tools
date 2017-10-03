KDIR=/lib/modules/$(shell uname -r)/build
ccflags-y += -DCONFIG_CIFS_SMB2_SERVER=y
ccflags-y += -DCONFIG_CIFS_SERVER=m

obj-m += cifsd.o

cifsd-y := 	export.o connect.o srv.o unicode.o encrypt.o auth.o \
		fh.o vfs.o misc.o smb1pdu.o smb1ops.o oplock.o netmisc.o \
		netlink.o cifsacl.o

cifsd-y += smb2pdu.o smb2ops.o asn1.o

all:
	make -C ${KDIR} M=$(PWD) modules

distclean: clean

clean:
	make -C ${KDIR} M=$(PWD) clean
