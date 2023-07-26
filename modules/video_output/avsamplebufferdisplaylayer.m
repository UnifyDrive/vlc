//
//  avsamplebufferdisplaylayer.m
//  Pseudo-VLC
//
//  Created by tongzhijie on 2023/3/27.
//

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_atomic.h>
#include "../codec/vt_utils.h"
#include <vlc_filter.h>

#import <QuartzCore/QuartzCore.h>
#import <dlfcn.h>
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

#if TARGET_OS_OSX
#include <AVKit/AVKit.h>
#define TDXView NSView
#define TDXImage NSImage
#define TDXColor NSColor
#else
#import <UIKit/UIKit.h>
#define TDXView UIView
#define TDXImage UIImage
#define TDXColor UIColor
#endif

#define SUPPORT_MULTI_SUBTITLE_PICTURE 1
#define  ZS_DEBUG      (0)

static const vlc_fourcc_t subpicture_chromas[] =
{
    VLC_CODEC_RGBA,
    0
};
/*****************************************************************************
 * Vout interface
 *****************************************************************************/
static int  Open   (vlc_object_t *);
static void Close  (vlc_object_t *);

static picture_pool_t *Pool (vout_display_t *vd, unsigned requested_count);
static void PicturePrepare  (vout_display_t *vd, picture_t *pic, subpicture_t *subpicture);
static void PictureDisplay  (vout_display_t *vd, picture_t *pic, subpicture_t *subpicture);
static int Control          (vout_display_t *vd, int query, va_list ap);

@interface TDXVideoView : TDXView
{
    vout_display_t *vd;
}
- (void)setVoutDisplay:(vout_display_t *)vd;
@end


@protocol VLCOpenGLVideoViewEmbedding <NSObject>
- (void)addVoutSubview:(TDXView *)view;
- (void)removeVoutSubview:(TDXView *)view;
@end

struct vout_display_sys_t
{
    vout_window_t *embed;
    TDXVideoView *videoView;
    CALayer *displayLayer;
    CALayer *subtitleLayer;
    Rect rect;
    Rect sub_last_region;
    int64_t i_sub_last_order;
    id<VLCOpenGLVideoViewEmbedding> container;
    picture_pool_t *pool;
    picture_t *p_sub_pic;
    filter_t *p_spu_blend;
    vout_display_place_t place;
    TDXImage * uiImage;
#if TARGET_OS_OSX
    CGImageRef cgImage;
#endif
};

#define VLCAssertMainThread() assert([[NSThread currentThread] isMainThread])
#pragma mark -
#pragma mark Module functions

static int Open(vlc_object_t *this)
{
    
    @autoreleasepool {
        vout_display_t *vd = (vout_display_t *)this;
        vout_display_sys_t *sys;
        msg_Dbg(vd, "[%s:%s:%d]=zspace=: Enter", __FILE__ , __FUNCTION__, __LINE__);
        vd->sys = sys = vlc_obj_calloc(this, 1, sizeof(*sys));
        if (sys == NULL)
            return VLC_ENOMEM;
    
        /* Obtain container NSObject */
        id container = var_CreateGetAddress(vd, "drawable-nsobject");
        if (container) {
            vout_display_DeleteWindow(vd, NULL);
        } else {
            sys->embed = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_NSOBJECT);
            if (sys->embed)
                container = sys->embed->handle.nsobject;

            if (!container) {
                msg_Err(vd, "[%s:%s:%d]=zspace=: Failed to get container", __FILE__ , __FUNCTION__, __LINE__);
                goto error;
            }
        }
        
        sys->container = [container retain];

        dispatch_sync(dispatch_get_main_queue(), ^{
            /* Create video view */
            sys->videoView = [[TDXVideoView alloc] init];
#if TARGET_OS_OSX
            sys->videoView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            sys->videoView.wantsLayer = YES;
#else
            sys->videoView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
#endif
            [sys->videoView setVoutDisplay:vd];
            
            if (@available(macOS 10.8, iOS 8.0, tvOS 10.2, *)) {
                AVSampleBufferDisplayLayer * displayLayer = [[AVSampleBufferDisplayLayer alloc] init];
                sys->displayLayer = (CALayer*)displayLayer;
                sys->displayLayer.backgroundColor = [TDXColor blackColor].CGColor;
                CGFloat angle = 0;
                msg_Dbg(vd, "[%s:%s:%d]=zspace=: orientation %d", __FILE__ , __FUNCTION__, __LINE__, vd->fmt.orientation);
                switch(vd->fmt.orientation)
                {
                    case ORIENT_ROTATED_90:
#if TARGET_OS_OSX
                        angle = M_PI * 1.5;
#else
                        angle = M_PI/2;
#endif
                        break;
                    default :
                        break;
                }
                sys->displayLayer.transform = CATransform3DMakeRotation(angle, 0, 0, 1);
                [sys->videoView.layer addSublayer:sys->displayLayer];
            }
            
            /* Add video view to container */
            if ([container respondsToSelector:@selector(addVoutSubview:)]) {
                [container addVoutSubview:sys->videoView];
            } else if ([container isKindOfClass:[TDXView class]]) {
                TDXView *containerView = container;
                [containerView addSubview:sys->videoView];
                [sys->videoView setFrame:containerView.bounds];
                sys->displayLayer.frame = sys->videoView.layer.bounds;
                msg_Dbg(vd, "[%s:%s:%d]=zspace=: videoView width %f height %f", __FILE__ , __FUNCTION__, __LINE__,sys->videoView.layer.bounds.size.width,sys->videoView.layer.bounds.size.height);
            } else {
                sys->videoView = nil;
            }
        });

        vd->pool    = Pool;
        vd->prepare = PicturePrepare;
        vd->display = PictureDisplay;
        vd->control = Control;
        sys->subtitleLayer = nil;
        sys->p_sub_pic = nil;
        sys->p_spu_blend = nil;
        vd->info.subpicture_chromas = subpicture_chromas;
        memset(&sys->sub_last_region, 0x00, sizeof(Rect));
        sys->i_sub_last_order = -1;
        
        msg_Dbg(vd, "[%s:%s:%d]=zspace=: Exit", __FILE__ , __FUNCTION__, __LINE__);
        return VLC_SUCCESS;
    
    error:
        Close(this);
        return VLC_EGENERIC;
    }
}

static void Close(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys = vd->sys;
    msg_Dbg(vd, "[%s:%s:%d]=zspace=: Enter", __FILE__ , __FUNCTION__, __LINE__);
    @autoreleasepool {
        if (sys->p_sub_pic)
        {
            picture_Release(sys->p_sub_pic);
            sys->p_sub_pic = nil;
        }
        if (sys->p_spu_blend)
            filter_DeleteBlend(sys->p_spu_blend);
        if (sys->embed)
            vout_display_DeleteWindow(vd, sys->embed);
        id container = sys->container;
        TDXVideoView *videoView = sys->videoView;
        CALayer *videoLayer = sys->displayLayer;
    
        dispatch_sync(dispatch_get_main_queue(), ^{
            /* Remove vout subview from container */
            if ([container respondsToSelector:@selector(removeVoutSubview:)]) {
                [container removeVoutSubview:videoView];
            }
            [videoView removeFromSuperview];
            [videoView release];
            [container release];
            [videoLayer release];
        });
    }
    msg_Dbg(vd, "[%s:%s:%d]=zspace=: Exit", __FILE__ , __FUNCTION__, __LINE__);
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned count)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->pool == NULL)
        sys->pool = picture_pool_NewFromFormat(&vd->source,count);
    return sys->pool;
}

TDXImage *convertRGBAToImage(vout_display_t *vd, unsigned char *rgba, int width, int height)
{
    vout_display_sys_t *sys = vd->sys;
    int bytes_per_pix = 4;
    
    /* remove white point in srt subtitle*/
    for(int i=3; i<width*height*4;)
    {
        if (rgba[i] <= 50)
        {
            rgba[i-1] = 0x00;
            rgba[i-2] = 0x00;
            rgba[i-3] = 0x00;
            rgba[i]   = 0x00;
        }
        i += 4;
    }

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef newContext = CGBitmapContextCreate(rgba,
                                                    width, height, 8,
                                                    width * bytes_per_pix,
                                                    colorSpace, kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGImageRef frame = CGBitmapContextCreateImage(newContext);
#if TARGET_OS_OSX
    TDXImage *image = [[TDXImage alloc] initWithCGImage:frame size:(NSSize){width,height}];
    sys->cgImage = frame;
#else
    TDXImage *image = [[TDXImage alloc] initWithCGImage:frame];
#endif
    CGImageRelease(frame);
    CGContextRelease(newContext);
    CGColorSpaceRelease(colorSpace);

    return image;
}

static picture_t *PictureAlloc(vout_display_sys_t *sys, video_format_t *fmt)
{
    picture_t *p_pic;
    picture_resource_t rsc;

    memset(&rsc, 0, sizeof(picture_resource_t));
    rsc.p_sys = nil;

    p_pic = picture_NewFromFormat(fmt);
    if (!p_pic)
    {
        return NULL;
    }
    return p_pic;
}

static void SetRGBMask(video_format_t *p_fmt)
{
    switch(p_fmt->i_chroma) {
        case VLC_CODEC_RGB16:
            p_fmt->i_bmask = 0x0000001f;
            p_fmt->i_gmask = 0x000007e0;
            p_fmt->i_rmask = 0x0000f800;
            break;

        case VLC_CODEC_RGB32:
        case VLC_CODEC_RGBA:
            p_fmt->i_rmask = 0x000000ff;
            p_fmt->i_gmask = 0x0000ff00;
            p_fmt->i_bmask = 0x00ff0000;
            break;
    }
}

static void SubtitleRegionToBounds(subpicture_t *subpicture,
                                   Rect *p_out_bounds)
{
    if (subpicture) {
#ifdef SUPPORT_MULTI_SUBTITLE_PICTURE
        for (subpicture_region_t *r = subpicture->p_region; r != NULL; r = r->p_next)
#else
        subpicture_region_t *r = subpicture->p_region;
        if (r)
#endif
        {
            Rect new_bounds;
            new_bounds.left = r->i_x;
            new_bounds.top = r->i_y;
            if (new_bounds.left < 0)
                new_bounds.left = 0;
            if (new_bounds.top < 0)
                new_bounds.top = 0;
            new_bounds.right = r->fmt.i_visible_width + r->i_x;
            new_bounds.bottom = r->fmt.i_visible_height + r->i_y;
            if (r == &subpicture->p_region[0])
                *p_out_bounds = new_bounds;
            else {
                if (p_out_bounds->left > new_bounds.left)
                    p_out_bounds->left = new_bounds.left;
                if (p_out_bounds->right < new_bounds.right)
                    p_out_bounds->right = new_bounds.right;
                if (p_out_bounds->top > new_bounds.top)
                    p_out_bounds->top = new_bounds.top;
                if (p_out_bounds->bottom < new_bounds.bottom)
                    p_out_bounds->bottom = new_bounds.bottom;
            }

        }
    } else {
        p_out_bounds->left = p_out_bounds->top = 0;
        p_out_bounds->right = p_out_bounds->bottom = 0;
    }
    
    if (p_out_bounds->left < 0
        || p_out_bounds->right < 0
        || p_out_bounds->top < 0
        || p_out_bounds->bottom < 0
        || (p_out_bounds->right - p_out_bounds->left) <= 0
        || (p_out_bounds->bottom - p_out_bounds->top) <= 0 )
    {
        p_out_bounds->left = p_out_bounds->top = 0;
        p_out_bounds->right = p_out_bounds->bottom = 0;
    }

}

static void SubtitleGetDirtyBounds(vout_display_t *vd,
                                   subpicture_t *subpicture,
                                   Rect *p_out_bounds)
{
    vout_display_sys_t *sys = vd->sys;
    if (!p_out_bounds)
        return ;
    
    memcpy(p_out_bounds, &sys->sub_last_region, sizeof(Rect));
    if(p_out_bounds->left < 0
       || p_out_bounds->right < 0
       || p_out_bounds->top < 0
       || p_out_bounds->bottom < 0
       || p_out_bounds->right > sys->p_sub_pic->format.i_width
       || p_out_bounds->bottom > sys->p_sub_pic->format.i_height
       || (p_out_bounds->right - p_out_bounds->left <= 0)
       || (p_out_bounds->right - p_out_bounds->left)  > sys->p_sub_pic->format.i_width
       || (p_out_bounds->bottom - p_out_bounds->top) <= 0
       || (p_out_bounds->bottom - p_out_bounds->top) > sys->p_sub_pic->format.i_height)
    {
        p_out_bounds->left = 0;
        p_out_bounds->top = 0;
        p_out_bounds->right = sys->p_sub_pic->format.i_width;
        p_out_bounds->bottom = sys->p_sub_pic->format.i_height;
        
    }

}

static void clearLastRegion(vout_display_t *vd, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    Rect dirty_bounds;

    SubtitleGetDirtyBounds(vd, subpicture, &dirty_bounds);

    int x_pixels_offset = dirty_bounds.left * sys->p_sub_pic->p[0].i_pixel_pitch;
    int i_line_size = (dirty_bounds.right - dirty_bounds.left) * sys->p_sub_pic->p->i_pixel_pitch;
    if (ZS_DEBUG)
        msg_Dbg(vd, "[%s:%s:%d]=zspace=: dirty_bounds [%d,%d,  %d,%d],sys->i_sub_last_order=%lld x_pixels_offset %d i_line_size %d", __FILE__ , __FUNCTION__, __LINE__,dirty_bounds.left, dirty_bounds.top, dirty_bounds.right, dirty_bounds.bottom, sys->i_sub_last_order, x_pixels_offset, i_line_size);

    for (int y = dirty_bounds.top; y < dirty_bounds.bottom; y++)
        memset(&sys->p_sub_pic->p[0].p_pixels[y * sys->p_sub_pic->p[0].i_pitch + x_pixels_offset], 0, i_line_size);
}

static void GetDisplayRect(vout_display_t *vd, Rect memset_bounds, Rect *p_out_bounds)
{
    vout_display_sys_t *sys = vd->sys;
    CGFloat scale_width = 0.0;
    CGFloat scale_height = 0.0;
    CGFloat black_height = 0.0;
    CGFloat black_width = 0.0;
    if (vd->source.i_width/vd->source.i_height > sys->videoView.layer.bounds.size.width/sys->videoView.layer.bounds.size.height)
    {
        scale_width = vd->source.i_width/(sys->videoView.layer.bounds.size.width);
        CGFloat height = vd->source.i_height *sys->videoView.layer.bounds.size.width/vd->source.i_width;
        scale_height = vd->source.i_height/height;
        //msg_Dbg(vd, "[%s:%s:%d]=zspace=: scale_width %f scale_height %f", __FILE__ , __FUNCTION__, __LINE__,scale_width, scale_height);
        black_height = (sys->videoView.layer.bounds.size.height - height)/2 ;
    }
    else
    {
        scale_height = vd->source.i_height/(sys->videoView.layer.bounds.size.height);
        CGFloat width = vd->source.i_width * sys->videoView.layer.bounds.size.height/vd->source.i_height;
        scale_width = vd->source.i_width/width;
        //msg_Dbg(vd, "[%s:%s:%d]=zspace=: scale_width %f scale_height %f", __FILE__ , __FUNCTION__, __LINE__,scale_width, scale_height);
        black_width = (sys->videoView.layer.bounds.size.width - width)/2;

    }

    p_out_bounds->left = memset_bounds.left/scale_width + black_width;
    p_out_bounds->right = memset_bounds.right/scale_width + black_width;
    
#if TARGET_OS_OSX
    p_out_bounds->top = sys->videoView.layer.bounds.size.height - (memset_bounds.top/scale_height+black_height);
    p_out_bounds->bottom = sys->videoView.layer.bounds.size.height - (memset_bounds.bottom/scale_height+black_height);
#else
    p_out_bounds->top = memset_bounds.top/scale_height+black_height;
    p_out_bounds->bottom = memset_bounds.bottom/scale_height+black_height;
#endif
}

static void PicturePrepare(vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    Rect memset_bounds;
    SubtitleRegionToBounds(subpicture, &memset_bounds);

    if (subpicture)
    {
        if( subpicture->i_order == sys->i_sub_last_order
           && memcmp( &memset_bounds, &sys->sub_last_region, sizeof(Rect) ) == 0 )
            return;

        if(ZS_DEBUG)
            msg_Dbg(vd, "[%s:%s:%d]=zspace=: memset_bounds [%d,%d,  %d,%d],sys->i_sub_last_order=%lld", __FILE__ , __FUNCTION__, __LINE__, memset_bounds.left, memset_bounds.top, memset_bounds.right, memset_bounds.bottom, sys->i_sub_last_order);
            
        if (!sys->p_sub_pic)
        {
            video_format_t sub_fmt;
            video_format_ApplyRotation(&sub_fmt, &vd->fmt);
            sub_fmt.i_chroma = subpicture_chromas[0];
            SetRGBMask(&sub_fmt);
            video_format_FixRgb(&sub_fmt);
            sys->p_sub_pic = PictureAlloc(sys, &sub_fmt);
            msg_Dbg(vd, "[%s:%s:%d]=zspace=: PictureAlloc", __FILE__ , __FUNCTION__, __LINE__);
        }
        
        if (!sys->p_spu_blend && sys->p_sub_pic)
        {
            sys->p_spu_blend = filter_NewBlend(VLC_OBJECT(vd),&sys->p_sub_pic->format);
            msg_Dbg(vd, "[%s:%s:%d]=zspace=: Run filter_NewBlend(), return=%p ", __FILE__ , __FUNCTION__, __LINE__, sys->p_spu_blend);
        }
        
#ifdef SUPPORT_MULTI_SUBTITLE_PICTURE
        clearLastRegion(vd, subpicture);
#endif
        
        memcpy(&sys->sub_last_region, &memset_bounds, sizeof(Rect));
        unsigned char * buffer = nil;
        int img_width = 0;
        int img_height = 0;

#ifdef SUPPORT_MULTI_SUBTITLE_PICTURE
        picture_BlendSubpicture(sys->p_sub_pic, sys->p_spu_blend, subpicture);

        /* copy subpicture to a small region */
        int x_pixels_offset = memset_bounds.left * sys->p_sub_pic->p[0].i_pixel_pitch;
        img_width = (memset_bounds.right - memset_bounds.left) * sys->p_sub_pic->p->i_pixel_pitch/4;
        img_height = memset_bounds.bottom - memset_bounds.top;
        if (ZS_DEBUG)
            msg_Dbg(vd, "[%s:%s:%d]=zspace=: width %d img_height %d", __FILE__ , __FUNCTION__, __LINE__,img_width, img_height);
        buffer = (unsigned char *)malloc(img_width*img_height*4);
        if (!buffer)
        {
            msg_Err(vd, "[%s:%s:%d]=zspace=: Failed to allocate memory", __FILE__ , __FUNCTION__, __LINE__);
            return;
        }
        memset(buffer, 0x00, img_width*img_height*4);
        for (int y = memset_bounds.top; y < memset_bounds.bottom; y++)
            memcpy(&buffer[(y-memset_bounds.top)*img_width*4], &sys->p_sub_pic->p[0].p_pixels[y * sys->p_sub_pic->p[0].i_pitch
                                                    + x_pixels_offset], img_width*4);
#else
        subpicture_region_t *r = subpicture->p_region;
        if(r)
        {
            plane_t * plane = &r->p_picture->p[0];
            img_height = r->fmt.i_visible_height;
            img_width = plane->i_pitch/4;
            buffer = &plane->p_pixels[0];
            msg_Dbg(vd, "r->fmt.i_visible_width %d width %d i_lines %d i_visible_line% d i_pitch %d",r->fmt.i_visible_width,img_width,plane->i_lines,plane->i_visible_lines,plane->i_pitch);
        }
#endif
        if (sys->uiImage)
        {
#if TARGET_OS_OSX
            if (sys->cgImage)
            {
                dispatch_sync(dispatch_get_main_queue(), ^{
                    CGImageRelease(sys->cgImage);
                    sys->cgImage = nil;
                });
            }
#else
            if (sys->uiImage.CGImage && [sys->uiImage.CGImage retainCount] > 0)
            {
                CGImageRelease(sys->uiImage.CGImage);
            }
#endif
            sys->uiImage = nil;
        }
        sys->i_sub_last_order = subpicture->i_order;
        sys->uiImage = convertRGBAToImage(vd, buffer, img_width, img_height);
#ifdef SUPPORT_MULTI_SUBTITLE_PICTURE
        free(buffer);
        buffer = nil;
#endif
    }

}

static void PictureDisplay(vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    if (!pic || !pic->context){
        msg_Err(vd, "pic is null");
        return ;
    }
    Rect memset_bounds;
    SubtitleRegionToBounds(subpicture, &memset_bounds);
    if (subpicture)
    {
        dispatch_sync(dispatch_get_main_queue(), ^{
            if (!sys->subtitleLayer)
            {
                sys->subtitleLayer = [[CALayer alloc] init];
                [sys->videoView.layer addSublayer:sys->subtitleLayer];
            }
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
    #if TARGET_OS_OSX
            sys->subtitleLayer.contents = sys->uiImage;
    #else
            sys->subtitleLayer.contents = sys->uiImage.CGImage;
    #endif
            /* make scale for display screen */
            Rect display_bounds;
            GetDisplayRect(vd, memset_bounds, &display_bounds);
            if (ZS_DEBUG)
                msg_Dbg(vd, "[%s:%s:%d]=zspace=: display_bounds [%d,%d,  %d,%d],sys->i_sub_last_order=%lld", __FILE__ , __FUNCTION__, __LINE__,
                    display_bounds.left, display_bounds.top, display_bounds.right, display_bounds.bottom, sys->i_sub_last_order);
            sys->subtitleLayer.frame = CGRectMake(display_bounds.left, display_bounds.top, display_bounds.right-display_bounds.left, display_bounds.bottom-display_bounds.top);
                
            if (sys->subtitleLayer.isHidden)
                sys->subtitleLayer.hidden = NO;
            [CATransaction commit];
            memcpy(&sys->rect, &display_bounds, sizeof(Rect));

        });
    }
    else
    {
        dispatch_sync(dispatch_get_main_queue(), ^{
            if (!sys->subtitleLayer.isHidden)
            {
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                sys->subtitleLayer.contents = nil;
                sys->subtitleLayer.hidden = YES;
                [CATransaction commit];
            }
        });
    }
    
    CVPixelBufferRef pixelBuffer = cvpxpic_get_ref(pic);
    if (pixelBuffer == NULL) {
        msg_Err(vd, "pixelBuffer is null");
        return;
    }


    CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
    CMVideoFormatDescriptionRef videoInfo = NULL;
    OSStatus result = CMVideoFormatDescriptionCreateForImageBuffer(NULL, pixelBuffer, &videoInfo);
    //NSParameterAssert(result == 0 && videoInfo != NULL);
    if (videoInfo == NULL){
        msg_Err(vd, "PictureRender videoInfo == null");
        return ;
    }
    if (result != 0){
        return ;
    }

    CMSampleBufferRef sampleBuffer = NULL;
    result = CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault,pixelBuffer, true, NULL, NULL, videoInfo, &timing, &sampleBuffer);
    if (sampleBuffer == NULL){
        return ;
    }
    if (result != 0){
        return ;
    }
    CFRelease(videoInfo);
    
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, YES);
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    if (@available(macOS 10.8, iOS 8.0, tvOS 10.2, *)) {
        [(AVSampleBufferDisplayLayer*)sys->displayLayer enqueueSampleBuffer:sampleBuffer];
    }
    CFRelease(sampleBuffer);
    
    if (@available(macOS 10.10, iOS 8.0, tvOS 10.2, *)) {
        if(((AVSampleBufferDisplayLayer*)sys->displayLayer).status == AVQueuedSampleBufferRenderingStatusFailed)
        {
            [((AVSampleBufferDisplayLayer*)sys->displayLayer) flush];
        }
    }
    
    picture_Release(pic);
    if (subpicture)
        subpicture_Delete(subpicture);

}

static int Control(vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

    if (!vd->sys)
        return VLC_EGENERIC;
    msg_Dbg (vd, "request %d", query);

    switch (query)
    {
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        {
            const vout_display_cfg_t *cfg;
            cfg = (const vout_display_cfg_t*)va_arg (ap, const vout_display_cfg_t *);
            msg_Dbg(vd, "[%s:%s:%d]=zspace=: display size: %dx%d", __FILE__ , __FUNCTION__, __LINE__, cfg->display.width,
                    cfg->display.height);
            return VLC_SUCCESS;
        }
        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        {
            const vout_display_cfg_t *cfg;
            cfg = vd->cfg;
            msg_Dbg(vd, "[%s:%s:%d]=zspace=: aspect size: %dx%d", __FILE__ , __FUNCTION__, __LINE__, cfg->display.width,
                    cfg->display.height);
            return VLC_SUCCESS;
        }

        default:
            msg_Err (vd, "Unhandled request %d", query);
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

#pragma mark -
#pragma mark TDXVideoView
@implementation TDXVideoView
#if TARGET_OS_OSX
- (void)layout {
    [super layout];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    vout_display_sys_t *sys = vd->sys;
    if (sys->displayLayer.frame.size.height != self.layer.bounds.size.height
        || sys->displayLayer.frame.size.width != self.layer.bounds.size.width)
    {
        msg_Dbg(vd, "[%s:%s:%d]=zspace=: displayLayer width %f height %f videoView width %f height %f", __FILE__ , __FUNCTION__, __LINE__,sys->displayLayer.frame.size.width, sys->displayLayer.frame.size.height, self.layer.bounds.size.width, self.layer.bounds.size.height);
        sys->displayLayer.frame = self.layer.bounds;
    }
//    if (sys->subtitleLayer)
//    {
//        if ((sys->rect.right-sys->rect.left > 0) && (sys->rect.bottom-sys->rect.top > 0))
//        {
//            sys->subtitleLayer.frame = CGRectMake(sys->rect.left, sys->rect.top, sys->rect.right-sys->rect.left, sys->rect.bottom-sys->rect.top);
//        }
//        msg_Dbg(vd, "[%s:%s:%d]=zspace=: left %d top %d right %d right %d", __FILE__ , __FUNCTION__, __LINE__,sys->rect.left, sys->rect.top, sys->rect.right, sys->rect.bottom);
//    }
    [CATransaction commit];
}
#else
- (void)layoutSubviews
{
    [super layoutSubviews];
    vout_display_sys_t *sys = vd->sys;
    
    if (sys->displayLayer.frame.size.height != self.layer.bounds.size.height
        || sys->displayLayer.frame.size.width != self.layer.bounds.size.width)
    {
        msg_Dbg(vd, "[%s:%s:%d]=zspace=: displayLayer width %f height %f videoView width %f height %f", __FILE__ , __FUNCTION__, __LINE__,sys->displayLayer.frame.size.width, sys->displayLayer.frame.size.height, self.layer.bounds.size.width, self.layer.bounds.size.height);
        sys->displayLayer.frame = self.layer.bounds;
    }
}
#endif

- (void)setVoutDisplay:(vout_display_t *)voutDisplay
{
    @synchronized(self) {
        vd = voutDisplay;
    }
}

@end

/*
 * Module descriptor
 */

vlc_module_begin()
    set_description(N_("AVSampleBufferDisplayLayer"))
    set_capability("vout display", 250)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_callbacks(Open, Close)
    add_shortcut("AVSampleBufferDisplayLayer", "avsamplebufferdisplaylayer")
vlc_module_end()
