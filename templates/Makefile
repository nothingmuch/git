# make and install sample templates

ifndef V
	QUIET = @
endif

INSTALL ?= install
TAR ?= tar
RM ?= rm -f
prefix ?= $(HOME)
template_instdir ?= $(prefix)/share/git-core/templates
# DESTDIR=

# Shell quote (do not use $(call) to accommodate ancient setups);
DESTDIR_SQ = $(subst ','\'',$(DESTDIR))
template_instdir_SQ = $(subst ','\'',$(template_instdir))

all: boilerplates.made custom

# Put templates that can be copied straight from the source
# in a file direc--tory--file in the source.  They will be
# just copied to the destination.

bpsrc = $(filter-out %~,$(wildcard *--*))
boilerplates.made : $(bpsrc)
	$(QUIET)umask 022 && ls *--* 2>/dev/null | \
	while read boilerplate; \
	do \
		case "$$boilerplate" in *~) continue ;; esac && \
		dst=`echo "$$boilerplate" | sed -e 's|^this|.|;s|--|/|g'` && \
		dir=`expr "$$dst" : '\(.*\)/'` && \
		mkdir -p blt/$$dir && \
		case "$$boilerplate" in \
		*--) continue;; \
		esac && \
		cp $$boilerplate blt/$$dst && \
		if test -x "blt/$$dst"; then rx=rx; else rx=r; fi && \
		chmod a+$$rx "blt/$$dst" || exit; \
	done && \
	date >$@

# If you need build-tailored templates, build them into blt/
# directory yourself here.
custom:
	$(QUIET): no custom templates yet

clean:
	$(RM) -r blt boilerplates.made

install: all
	$(INSTALL) -d -m 755 '$(DESTDIR_SQ)$(template_instdir_SQ)'
	(cd blt && $(TAR) cf - .) | \
	(cd '$(DESTDIR_SQ)$(template_instdir_SQ)' && umask 022 && $(TAR) xfo -)
