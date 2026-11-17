PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := heatline.elf

# Single source of the version number: the VERSION file (e.g. 0.0.1).
# The output file name stays heatline.elf; the version is compiled in.
VERSION := $(strip $(shell cat VERSION))
ifeq ($(VERSION),)
    $(error VERSION file is missing or empty)
endif

CFLAGS := -Wall -Werror -O2 -DHEATLINE_VERSION='"$(VERSION)"'
LDLIBS := -lkernel_sys -lSceSystemService -lSceAppInstUtil -lSceNetCtl

all: $(ELF)

# index.html is generated from web/ (template + embedded fonts and favicon).
# The generated file is shipped too, so python3 is only needed after editing web/.
index.html: web/index.tpl.html web/build_page.py web/favicon.b64 web/fonts/Geist-Variable-subset.woff2 web/fonts/GeistMono-Variable-subset.woff2
	python3 web/build_page.py $@.tmp && test -s $@.tmp && mv $@.tmp $@

# Embed the web page. Uses xxd if available, otherwise python3; writes to a
# temp file first so a failure never leaves an empty header behind.
page_html.h: index.html
	@if command -v xxd >/dev/null 2>&1; then \
	  xxd -i $< > $@.tmp; \
	else \
	  python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); print("unsigned char index_html[] = {"); [print("  "+", ".join("0x%02x"%b for b in d[i:i+12])+",") for i in range(0,len(d),12)]; print("};"); print("unsigned int index_html_len = %d;"%len(d))' $< > $@.tmp; \
	fi && test -s $@.tmp && mv $@.tmp $@

# On-TV overlay engine (vendored SimpleFPS / Common-FPS), built as a static
# library and linked into heatline.elf. See overlay/build_overlay.sh.
OVERLAY_LIB := overlay/libheatline_overlay.a
OVERLAY_SRCS := $(shell find overlay/engine/src overlay/engine/include overlay/engine/ps5/CMakeLists.txt overlay/shsrv -type f 2>/dev/null)

# C++ runtime and extra system libs the overlay engine needs, on top of LDLIBS.
OVERLAY_LDLIBS := -lSceNet -lSceSysmodule -lSceUserService -lSceLibcInternal -lc++ -lc++abi

$(OVERLAY_LIB): $(OVERLAY_SRCS) overlay/build_overlay.sh
	PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) bash overlay/build_overlay.sh

# every .c / .h here is a dependency, so a change to any of them rebuilds
SRCS := main.c fps.c osd.c cpuload.c context.c storage.c sysinfo.c ntp.c fan.c selfstat.c app.c netlat.c display.c raw.c
$(ELF): $(SRCS) $(wildcard *.h) page_html.h assets/param.json assets/icon0.png VERSION $(OVERLAY_LIB)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(OVERLAY_LIB) $(LDLIBS) $(OVERLAY_LDLIBS)

clean:
	rm -f $(ELF)

clean-overlay:
	rm -rf overlay/build overlay/libheatline_overlay.a

# Lists what still has to be filled in before a public release (developer
# name, GitHub link, download URL) and checks that payloads.json has the
# version and checksum of the current heatline.elf. Exits with an error if
# anything is left.
release-check: $(ELF)
	@status=0; \
	for spec in "README.md:(your name)" "README.md:GITHUB_URL" \
	            "repo/payloads.json:(your name)" "repo/payloads.json:YOUR-HOST" \
	            "web/index.tpl.html:const DEV={name:''"; do \
	  f=$${spec%%:*}; pat=$${spec#*:}; \
	  if grep -qF "$$pat" "$$f"; then grep -nF "$$pat" "$$f" | sed "s|^|  TODO $$f:|"; status=1; fi; \
	done; \
	sum=$$(sha256sum $(ELF) | cut -d' ' -f1); \
	grep -q "\"checksum\": \"$$sum\"" repo/payloads.json || { echo "  TODO repo/payloads.json: checksum is not the SHA-256 of $(ELF) ($$sum)"; status=1; }; \
	grep -q "\"version\": \"v$(VERSION)\"" repo/payloads.json || { echo "  TODO repo/payloads.json: version is not v$(VERSION)"; status=1; }; \
	if [ $$status -eq 0 ]; then echo "release-check: OK, ready to publish v$(VERSION)"; \
	else echo "release-check: fill in the TODO items above (see README, Release checklist)"; fi; \
	exit $$status

.PHONY: all clean clean-overlay test release-check

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)
