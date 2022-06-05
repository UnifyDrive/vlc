#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <stdlib.h>
#include <vlc/libvlc.h>
#include <vlc/libvlc_tdx.h>

#ifdef HAVE_FONTCONFIG_FONTCONFIG_H
#include <fontconfig/fontconfig.h>

int libvlc_tdx_init_fontconfig(const char* filename) {
	int rc;
    setenv("FONTCONFIG_FILE", filename, 1);
    FcConfig* config = NULL;
    config = FcConfigCreate();

    rc = FcConfigParseAndLoad(config, filename, FcTrue);
    if (!rc) {
        goto error;
    }

    rc = FcConfigBuildFonts(config);
    FcConfigDestroy(config);

    return rc;

    error:
    FcConfigDestroy(config);
    return -1;
}

#else

int libvlc_tdx_init_fontconfig(const char* filename) {
	return -1;
}

#endif
