/*****************************************************************************
 * dts_header.c: parse DTS audio headers info
 *****************************************************************************
 * Copyright (C) 2004-2016 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Gildas Bazin <gbazin@netcourrier.com>
 *          Laurent Aimar
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#define VLC_DTS_HEADER_SIZE 14

#define PROFILE_DTS_INVALID -1
#define PROFILE_DTS 0
#define PROFILE_DTS_HD 1
#define PROFILE_DTS_EXPRESS 2
#define PROFILE_DTS_HD_MA 3



#define FF_PROFILE_DTS         20
#define FF_PROFILE_DTS_ES      30
#define FF_PROFILE_DTS_96_24   40
#define FF_PROFILE_DTS_HD_HRA  50
#define FF_PROFILE_DTS_HD_MA   60
#define FF_PROFILE_DTS_EXPRESS 70

enum vlc_dts_syncword_e
{
    DTS_SYNC_NONE = 0,
    DTS_SYNC_CORE_BE,
    DTS_SYNC_CORE_LE,
    DTS_SYNC_CORE_14BITS_BE,
    DTS_SYNC_CORE_14BITS_LE,
    DTS_SYNC_SUBSTREAM,
    /* Substreams internal syncs */
    DTS_SYNC_SUBSTREAM_LBR,
    DTS_SYNC_SUBSTREAM_XLL,
    DTS_SYNC_SUBSTREAM_XCH,
    DTS_SYNC_SUBSTREAM_XXCH,
    DTS_SYNC_SUBSTREAM_X96K,
    DTS_SYNC_SUBSTREAM_XBR,
};

typedef struct
{
    enum vlc_dts_syncword_e syncword;
    unsigned int    i_rate;
    unsigned int    i_bitrate;
    unsigned int    i_frame_size;
    unsigned int    i_frame_length;
    uint32_t        i_substream_header_size;
    uint16_t        i_physical_channels;
    uint16_t        i_chan_mode;
} vlc_dts_header_t;

int     vlc_dts_header_Parse( vlc_dts_header_t *p_header,
                              const void *p_buffer, size_t i_buffer);

bool    vlc_dts_header_IsSync( const void *p_buffer, size_t i_buffer );

ssize_t vlc_dts_header_Convert14b16b( void *p_dst, size_t i_dst,
                                      const void *p_src, size_t i_src,
                                      bool b_out_le );
