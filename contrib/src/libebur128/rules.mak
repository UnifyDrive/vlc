# EBU R128 standard for loudness normalisation

LIBEBUR128_VERSION := 1.2.4
LIBEBUR128_URL := https://github.com/jiixyj/libebur128/archive/v$(LIBEBUR128_VERSION).tar.gz

PKGS += libebur128
ifeq ($(call need_pkg,"libebur128"),)
PKGS_FOUND += libebur128
endif

$(TARBALLS)/libebur128-$(LIBEBUR128_VERSION).tar.gz:
	$(call download_pkg,$(LIBEBUR128_URL),libebur128)

.sum-libebur128: libebur128-$(LIBEBUR128_VERSION).tar.gz

libebur128: libebur128-$(LIBEBUR128_VERSION).tar.gz .sum-libebur128
	$(UNPACK)
	$(MOVE)

.libebur128: libebur128 toolchain.cmake
	cd $< && $(HOSTVARS_PIC) $(CMAKE)
	cd $< && $(MAKE) install
	touch $@
