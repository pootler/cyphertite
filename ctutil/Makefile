
.include "${.CURDIR}/../config/Makefile.common"
SYSTEM != uname -s
.if exists(${.CURDIR}/../config/Makefile.$(SYSTEM:L))
.  include "${.CURDIR}/../config/Makefile.$(SYSTEM:L)"
.endif

LOCALBASE?=/usr/local
BINDIR?=${LOCALBASE}/bin
LIBDIR?=${LOCALBASE}/lib
INCDIR=${LOCALBASE}/include

#WANTLINT=
LIB= ctutil
SRCS= ctutil.c ct_fileops.c ct_socket.c ct_core.c ct_ssl.c ct_compress.c
SRCS+= ct_xml.c ct_certs.c ct_update.c
HDRS= ctutil.h ct_socket.h ct_threads.h ct_xml.h

INCFLAGS+= -I${.CURDIR} -I${LOCALBASE}/include
CFLAGS+=${INCFLAGS} ${WARNFLAGS}

afterinstall:
	mkdir -p  ${DESTDIR}${INCDIR}/cyphertite
	@cd ${.CURDIR}; for i in ${HDRS}; do \
	cmp -s $$i ${DESTDIR}${INCDIR}/cyphertite/$$i || \
	${INSTALL} ${INSTALL_COPY} -m 444 -o $(BINOWN) -g $(BINGRP) $$i ${DESTDIR}${INCDIR}/cyphertite/; \
	echo ${INSTALL} ${INSTALL_COPY} -m 444 -o $(BINOWN) -g $(BINGRP) $$i ${DESTDIR}${INCDIR}/cyphertite/; \
	done

uninstall:
	@for i in ${HDRS}; do \
	echo rm -f ${INCDIR}/cyphertite/$$i; \
	rm -f ${INCDIR}/cyphertite/$$i; \
	done
	rmdir ${INCDIR}/cyphertite

	@for i in ${_LIBS}; do \
	echo rm -f ${LIBDIR}/$$i; \
	rm -f ${LIBDIR}/$$i; \
	done

.include <bsd.lib.mk>
