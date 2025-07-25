/*
 * Copyright (C) 2007 Google (Evan Stade)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <assert.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"

#define COBJMACROS
#include "objbase.h"
#include "ocidl.h"
#include "olectl.h"
#include "ole2.h"

#include "winreg.h"
#include "shlwapi.h"

#include "mlang.h"
#include "gdiplus.h"
#include "gdiplus_private.h"
#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdiplus);

/* looks-right constants */
#define ANCHOR_WIDTH (2.0)
#define MAX_ITERS (50)

GpStatus gdi_dc_acquire(GpGraphics *graphics, HDC *hdc)
{
    if (graphics->hdc != NULL)
    {
        *hdc = graphics->hdc;
        graphics->hdc_refs++;
        return Ok;
    }
    else if (graphics->owndc)
    {
        *hdc = graphics->hdc = GetDC(graphics->hwnd);
        if (!graphics->hdc)
            return OutOfMemory;
        graphics->hdc_refs++;
        return Ok;
    }

    *hdc = NULL;
    return InvalidParameter;
}

void gdi_dc_release(GpGraphics *graphics, HDC hdc)
{
    assert(graphics->hdc_refs > 0);
    graphics->hdc_refs--;

    if (graphics->owndc && !graphics->hdc_refs)
    {
        assert(graphics->hdc == hdc);
        graphics->hdc = NULL;
        ReleaseDC(graphics->hwnd, hdc);
    }
}

static GpStatus draw_driver_string(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                   GDIPCONST GpFont *font, GDIPCONST GpStringFormat *format,
                                   GDIPCONST GpBrush *brush, GDIPCONST PointF *positions,
                                   INT flags, GDIPCONST GpMatrix *matrix);

/* Converts from gdiplus path point type to gdi path point type. */
static BYTE convert_path_point_type(BYTE type)
{
    BYTE ret;

    switch(type & PathPointTypePathTypeMask){
        case PathPointTypeBezier:
            ret = PT_BEZIERTO;
            break;
        case PathPointTypeLine:
            ret = PT_LINETO;
            break;
        case PathPointTypeStart:
            ret = PT_MOVETO;
            break;
        default:
            ERR("Bad point type\n");
            return 0;
    }

    if(type & PathPointTypeCloseSubpath)
        ret |= PT_CLOSEFIGURE;

    return ret;
}

static COLORREF get_gdi_brush_color(const GpBrush *brush)
{
    ARGB argb;

    switch (brush->bt)
    {
        case BrushTypeSolidColor:
        {
            const GpSolidFill *sf = (const GpSolidFill *)brush;
            argb = sf->color;
            break;
        }
        case BrushTypeHatchFill:
        {
            const GpHatch *hatch = (const GpHatch *)brush;
            argb = hatch->forecol;
            break;
        }
        case BrushTypeLinearGradient:
        {
            const GpLineGradient *line = (const GpLineGradient *)brush;
            argb = line->startcolor;
            break;
        }
        case BrushTypePathGradient:
        {
            const GpPathGradient *grad = (const GpPathGradient *)brush;
            argb = grad->centercolor;
            break;
        }
        default:
            FIXME("unhandled brush type %d\n", brush->bt);
            argb = 0;
            break;
    }
    return ARGB2COLORREF(argb);
}

static BOOL is_metafile_graphics(const GpGraphics *graphics)
{
    return graphics->image && graphics->image_type == ImageTypeMetafile;
}

static ARGB blend_colors(ARGB start, ARGB end, REAL position);

static void init_hatch_palette(ARGB *hatch_palette, ARGB fore_color, ARGB back_color)
{
    /* Pass the center of a 45-degree diagonal line with width of one unit through the
     * center of a unit square, and the portion of the square that will be covered will
     * equal sqrt(2) - 1/2.  The covered portion for adjacent squares will be 1/4. */
    hatch_palette[0] = back_color;
    hatch_palette[1] = blend_colors(back_color, fore_color, 0.25);
    hatch_palette[2] = blend_colors(back_color, fore_color, sqrt(2.0) - 0.5);
    hatch_palette[3] = fore_color;
}

static HBITMAP create_hatch_bitmap(const GpHatch *hatch, INT origin_x, INT origin_y)
{
    HBITMAP hbmp;
    BITMAPINFOHEADER bmih;
    DWORD *bits;
    int x, y;

    bmih.biSize = sizeof(bmih);
    bmih.biWidth = 8;
    bmih.biHeight = 8;
    bmih.biPlanes = 1;
    bmih.biBitCount = 32;
    bmih.biCompression = BI_RGB;
    bmih.biSizeImage = 0;

    hbmp = CreateDIBSection(0, (BITMAPINFO *)&bmih, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (hbmp)
    {
        const unsigned char *hatch_data;

        if (get_hatch_data(hatch->hatchstyle, &hatch_data) == Ok)
        {
            ARGB hatch_palette[4];
            init_hatch_palette(hatch_palette, hatch->forecol, hatch->backcol);

            /* Anti-aliasing is only specified for diagonal hatch patterns.
             * This implementation repeats the pattern, shifts as needed,
             * then uses bitmask 1 to check the pixel value, and the 0x82
             * bitmask to check the adjacent pixel values, to determine the
             * degree of shading needed. */
            for (y = 0; y < 8; y++)
            {
                const int hy = (y + origin_y) & 7;
                const int hx = origin_x & 7;
                unsigned int row = (0x10101 * hatch_data[hy]) >> hx;

                for (x = 0; x < 8; x++, row >>= 1)
                {
                    int index;
                    if (hatch_data[8])
                        index = (row & 1) ? 2 : (row & 0x82) ? 1 : 0;
                    else
                        index = (row & 1) ? 3 : 0;
                    bits[y * 8 + 7 - x] = hatch_palette[index];
                }
            }
        }
        else
        {
            FIXME("Unimplemented hatch style %d\n", hatch->hatchstyle);

            for (y = 0; y < 64; y++)
                bits[y] = hatch->forecol;
        }
    }

    return hbmp;
}

static GpStatus create_gdi_logbrush(const GpBrush *brush, LOGBRUSH *lb, INT origin_x, INT origin_y)
{
    switch (brush->bt)
    {
        case BrushTypeSolidColor:
        {
            const GpSolidFill *sf = (const GpSolidFill *)brush;
            lb->lbStyle = BS_SOLID;
            lb->lbColor = ARGB2COLORREF(sf->color);
            lb->lbHatch = 0;
            return Ok;
        }

        case BrushTypeHatchFill:
        {
            const GpHatch *hatch = (const GpHatch *)brush;
            HBITMAP hbmp;

            hbmp = create_hatch_bitmap(hatch, origin_x, origin_y);
            if (!hbmp) return OutOfMemory;

            lb->lbStyle = BS_PATTERN;
            lb->lbColor = 0;
            lb->lbHatch = (ULONG_PTR)hbmp;
            return Ok;
        }

        default:
            FIXME("unhandled brush type %d\n", brush->bt);
            lb->lbStyle = BS_SOLID;
            lb->lbColor = get_gdi_brush_color(brush);
            lb->lbHatch = 0;
            return Ok;
    }
}

static GpStatus free_gdi_logbrush(LOGBRUSH *lb)
{
    switch (lb->lbStyle)
    {
        case BS_PATTERN:
            DeleteObject((HGDIOBJ)(ULONG_PTR)lb->lbHatch);
            break;
    }
    return Ok;
}

static HBRUSH create_gdi_brush(const GpBrush *brush, INT origin_x, INT origin_y)
{
    LOGBRUSH lb;
    HBRUSH gdibrush;

    if (create_gdi_logbrush(brush, &lb, origin_x, origin_y) != Ok) return 0;

    gdibrush = CreateBrushIndirect(&lb);
    free_gdi_logbrush(&lb);

    return gdibrush;
}

static INT prepare_dc(GpGraphics *graphics, HDC hdc, GpPen *pen)
{
    LOGBRUSH lb;
    HPEN gdipen;
    REAL width;
    INT save_state, i, numdashes;
    GpPointF pt[2];
    DWORD dash_array[MAX_DASHLEN];

    save_state = SaveDC(hdc);

    EndPath(hdc);

    if(pen->unit == UnitPixel){
        width = pen->width;
    }
    else{
        REAL scale_x, scale_y;
        /* Get an estimate for the amount the pen width is affected by the world
         * transform. (This is similar to what some of the wine drivers do.) */
        scale_x = graphics->worldtrans.matrix[0] + graphics->worldtrans.matrix[2];
        scale_y = graphics->worldtrans.matrix[1] + graphics->worldtrans.matrix[3];

        width = hypotf(scale_x, scale_y) / sqrt(2.0);

        width *= units_to_pixels(pen->width, pen->unit == UnitWorld ? graphics->unit : pen->unit,
                                 graphics->xres, graphics->printer_display);
        width *= graphics->scale;

        pt[0].X = 0.0;
        pt[0].Y = 0.0;
        pt[1].X = 1.0;
        pt[1].Y = 1.0;
        gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceDevice, pt, 2);
        width *= hypotf(pt[1].X - pt[0].X, pt[1].Y - pt[0].Y) / sqrt(2.0);
    }

    if(pen->dash == DashStyleCustom){
        numdashes = min(pen->numdashes, MAX_DASHLEN);

        TRACE("dashes are: ");
        for(i = 0; i < numdashes; i++){
            dash_array[i] = gdip_round(width * pen->dashes[i]);
            TRACE("%ld, ", dash_array[i]);
        }
        TRACE("\n and the pen style is %x\n", pen->style);

        create_gdi_logbrush(pen->brush, &lb, graphics->origin_x, graphics->origin_y);
        gdipen = ExtCreatePen(pen->style, gdip_round(width), &lb,
                              numdashes, dash_array);
        free_gdi_logbrush(&lb);
    }
    else
    {
        create_gdi_logbrush(pen->brush, &lb, graphics->origin_x, graphics->origin_y);
        gdipen = ExtCreatePen(pen->style, gdip_round(width), &lb, 0, NULL);
        free_gdi_logbrush(&lb);
    }

    SelectObject(hdc, gdipen);

    return save_state;
}

static void restore_dc(GpGraphics *graphics, HDC hdc, INT state)
{
    DeleteObject(SelectObject(hdc, GetStockObject(NULL_PEN)));
    RestoreDC(hdc, state);
}

static void round_points(POINT *pti, GpPointF *ptf, INT count)
{
    int i;

    for(i = 0; i < count; i++){
        if(isnan(ptf[i].X))
            pti[i].x = 0;
        else
            pti[i].x = gdip_round(ptf[i].X);

        if(isnan(ptf[i].Y))
            pti[i].y = 0;
        else
            pti[i].y = gdip_round(ptf[i].Y);
    }
}

static void gdi_alpha_blend(GpGraphics *graphics, INT dst_x, INT dst_y, INT dst_width, INT dst_height,
                            HDC hdc, INT src_x, INT src_y, INT src_width, INT src_height)
{
    HDC dst_hdc;
    CompositingMode comp_mode;
    INT technology, shadeblendcaps;

    gdi_dc_acquire(graphics, &dst_hdc);

    technology = GetDeviceCaps(dst_hdc, TECHNOLOGY);
    shadeblendcaps  = GetDeviceCaps(dst_hdc, SHADEBLENDCAPS);

    GdipGetCompositingMode(graphics, &comp_mode);

    if ((technology == DT_RASPRINTER && shadeblendcaps == SB_NONE)
        || comp_mode == CompositingModeSourceCopy)
    {
        TRACE("alpha blending not supported by device, fallback to StretchBlt\n");

        StretchBlt(dst_hdc, dst_x, dst_y, dst_width, dst_height,
                   hdc, src_x, src_y, src_width, src_height, SRCCOPY);
    }
    else
    {
        BLENDFUNCTION bf;

        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        GdiAlphaBlend(dst_hdc, dst_x, dst_y, dst_width, dst_height,
                      hdc, src_x, src_y, src_width, src_height, bf);
    }

    gdi_dc_release(graphics, dst_hdc);
}

static GpStatus get_graphics_device_bounds(GpGraphics* graphics, GpRectF* rect)
{
    RECT wnd_rect;
    GpStatus stat=Ok;
    GpUnit unit;

    if(graphics->hwnd) {
        if(!GetClientRect(graphics->hwnd, &wnd_rect))
            return GenericError;

        rect->X = wnd_rect.left;
        rect->Y = wnd_rect.top;
        rect->Width = wnd_rect.right - wnd_rect.left;
        rect->Height = wnd_rect.bottom - wnd_rect.top;
    }else if (graphics->image){
        stat = GdipGetImageBounds(graphics->image, rect, &unit);
        if (stat == Ok && unit != UnitPixel)
            FIXME("need to convert from unit %i\n", unit);
    }else if (GetObjectType(graphics->hdc) == OBJ_MEMDC){
        HBITMAP hbmp;
        BITMAP bmp;

        rect->X = 0;
        rect->Y = 0;

        hbmp = GetCurrentObject(graphics->hdc, OBJ_BITMAP);
        if (hbmp && GetObjectW(hbmp, sizeof(bmp), &bmp))
        {
            rect->Width = bmp.bmWidth;
            rect->Height = bmp.bmHeight;
        }
        else
        {
            /* FIXME: ??? */
            rect->Width = 1;
            rect->Height = 1;
        }
    }else{
        rect->X = 0;
        rect->Y = 0;
        rect->Width = GetDeviceCaps(graphics->hdc, HORZRES);
        rect->Height = GetDeviceCaps(graphics->hdc, VERTRES);
    }

    return stat;
}

static GpStatus get_clip_hrgn(GpGraphics *graphics, HRGN *hrgn)
{
    GpRegion *rgn;
    GpMatrix transform;
    GpStatus stat;
    GpRectF bounds;
    RECT gdi_bounds;
    BOOL identity;

    stat = get_graphics_device_bounds(graphics, &bounds);

    if (stat == Ok)
    {
        gdi_bounds.left = floorf(bounds.X);
        gdi_bounds.top = floorf(bounds.Y);
        gdi_bounds.right = ceilf(bounds.X + bounds.Width);
        gdi_bounds.bottom = ceilf(bounds.Y + bounds.Height);

        stat = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceDevice, &transform);
    }

    if (stat == Ok)
        stat = GdipIsMatrixIdentity(&transform, &identity);

    if (stat == Ok)
        stat = GdipCloneRegion(graphics->clip, &rgn);

    if (stat == Ok)
    {
        if (!identity)
            stat = GdipTransformRegion(rgn, &transform);

        if (stat == Ok)
            stat = get_region_hrgn(&rgn->node, &gdi_bounds, hrgn);

        GdipDeleteRegion(rgn);
    }

    if (stat == Ok && graphics->gdi_clip)
    {
        if (*hrgn)
            CombineRgn(*hrgn, *hrgn, graphics->gdi_clip, RGN_AND);
        else
        {
            *hrgn = CreateRectRgn(0,0,0,0);
            CombineRgn(*hrgn, graphics->gdi_clip, graphics->gdi_clip, RGN_COPY);
        }
    }

    return stat;
}

/* Draw ARGB data to the given graphics object */
static GpStatus alpha_blend_bmp_pixels(GpGraphics *graphics, INT dst_x, INT dst_y,
    const BYTE *src, INT src_width, INT src_height, INT src_stride, const PixelFormat fmt)
{
    GpBitmap *dst_bitmap = (GpBitmap*)graphics->image;
    INT x, y;
    CompositingMode comp_mode = graphics->compmode;

    for (y=0; y<src_height; y++)
    {
        for (x=0; x<src_width; x++)
        {
            ARGB dst_color, src_color;
            src_color = ((ARGB*)(src + src_stride * y))[x];

            if (comp_mode == CompositingModeSourceCopy)
            {
                if (!(src_color & 0xff000000))
                    GdipBitmapSetPixel(dst_bitmap, x+dst_x, y+dst_y, 0);
                else
                    GdipBitmapSetPixel(dst_bitmap, x+dst_x, y+dst_y, src_color);
            }
            else
            {
                if (!(src_color & 0xff000000))
                    continue;

                GdipBitmapGetPixel(dst_bitmap, x+dst_x, y+dst_y, &dst_color);
                if (fmt & PixelFormatPAlpha)
                    GdipBitmapSetPixel(dst_bitmap, x+dst_x, y+dst_y, color_over_fgpremult(dst_color, src_color));
                else
                    GdipBitmapSetPixel(dst_bitmap, x+dst_x, y+dst_y, color_over(dst_color, src_color));
            }
        }
    }

    return Ok;
}

static GpStatus alpha_blend_hdc_pixels(GpGraphics *graphics, INT dst_x, INT dst_y,
    const BYTE *src, INT src_width, INT src_height, INT src_stride, PixelFormat fmt)
{
    HDC hdc;
    HBITMAP hbitmap;
    BITMAPINFOHEADER bih;
    BYTE *temp_bits;

    hdc = CreateCompatibleDC(0);

    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = src_width;
    bih.biHeight = -src_height;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = 0;
    bih.biXPelsPerMeter = 0;
    bih.biYPelsPerMeter = 0;
    bih.biClrUsed = 0;
    bih.biClrImportant = 0;

    hbitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bih, DIB_RGB_COLORS,
        (void**)&temp_bits, NULL, 0);

    if(!hbitmap || !temp_bits)
        goto done;

    if ((graphics->hdc &&
         GetDeviceCaps(graphics->hdc, TECHNOLOGY) == DT_RASPRINTER &&
         GetDeviceCaps(graphics->hdc, SHADEBLENDCAPS) == SB_NONE) ||
            fmt & PixelFormatPAlpha)
        memcpy(temp_bits, src, src_width * src_height * 4);
    else
        convert_32bppARGB_to_32bppPARGB(src_width, src_height, temp_bits,
                                        4 * src_width, src, src_stride);

    SelectObject(hdc, hbitmap);
    gdi_alpha_blend(graphics, dst_x, dst_y, src_width, src_height,
                    hdc, 0, 0, src_width, src_height);

    DeleteObject(hbitmap);

done:
    DeleteDC(hdc);

    return Ok;
}

static GpStatus alpha_blend_pixels_hrgn(GpGraphics *graphics, INT dst_x, INT dst_y,
    const BYTE *src, INT src_width, INT src_height, INT src_stride, HRGN hregion, PixelFormat fmt)
{
    GpStatus stat=Ok;

    if (graphics->image && graphics->image->type == ImageTypeBitmap)
    {
        DWORD i;
        int size;
        RGNDATA *rgndata;
        RECT *rects;
        HRGN hrgn, visible_rgn;

        hrgn = CreateRectRgn(dst_x, dst_y, dst_x + src_width, dst_y + src_height);
        if (!hrgn)
            return OutOfMemory;

        if (hregion)
            CombineRgn(hrgn, hrgn, hregion, RGN_AND);
        else
        {
            stat = get_clip_hrgn(graphics, &visible_rgn);
            if (stat != Ok)
            {
                DeleteObject(hrgn);
                return stat;
            }

            if (visible_rgn)
            {
                CombineRgn(hrgn, hrgn, visible_rgn, RGN_AND);
                DeleteObject(visible_rgn);
            }
        }

        size = GetRegionData(hrgn, 0, NULL);

        rgndata = malloc(size);
        if (!rgndata)
        {
            DeleteObject(hrgn);
            return OutOfMemory;
        }

        GetRegionData(hrgn, size, rgndata);

        rects = (RECT*)rgndata->Buffer;

        for (i=0; stat == Ok && i<rgndata->rdh.nCount; i++)
        {
            stat = alpha_blend_bmp_pixels(graphics, rects[i].left, rects[i].top,
                &src[(rects[i].left - dst_x) * 4 + (rects[i].top - dst_y) * src_stride],
                rects[i].right - rects[i].left, rects[i].bottom - rects[i].top,
                src_stride, fmt);
        }

        free(rgndata);

        DeleteObject(hrgn);

        return stat;
    }
    else if (is_metafile_graphics(graphics))
    {
        ERR("This should not be used for metafiles; fix caller\n");
        return NotImplemented;
    }
    else
    {
        HDC hdc;
        HRGN hrgn;
        int save;

        stat = gdi_dc_acquire(graphics, &hdc);

        if (stat != Ok)
            return stat;

        save = SaveDC(hdc);

        if (hregion)
            ExtSelectClipRgn(hdc, hregion, RGN_COPY);
        else
        {
            stat = get_clip_hrgn(graphics, &hrgn);

            if (stat != Ok)
            {
                RestoreDC(hdc, save);
                gdi_dc_release(graphics, hdc);
                return stat;
            }

            ExtSelectClipRgn(hdc, hrgn, RGN_COPY);

            DeleteObject(hrgn);
        }

        stat = alpha_blend_hdc_pixels(graphics, dst_x, dst_y, src, src_width,
            src_height, src_stride, fmt);

        RestoreDC(hdc, save);

        gdi_dc_release(graphics, hdc);

        return stat;
    }
}

static GpStatus alpha_blend_pixels(GpGraphics *graphics, INT dst_x, INT dst_y,
    const BYTE *src, INT src_width, INT src_height, INT src_stride, PixelFormat fmt)
{
    return alpha_blend_pixels_hrgn(graphics, dst_x, dst_y, src, src_width, src_height, src_stride, NULL, fmt);
}

/* NOTE: start and end pixels must be in pre-multiplied ARGB format */
static FORCEINLINE ARGB blend_colors_premult(ARGB start, ARGB end, REAL position)
{
    UINT pos = position * 255.0f + 0.5f;
    return
        (((((start >> 24)       ) << 8) + (((end >> 24)       ) - ((start >> 24)       )) * pos) >> 8) << 24 |
        (((((start >> 16) & 0xff) << 8) + (((end >> 16) & 0xff) - ((start >> 16) & 0xff)) * pos) >> 8) << 16 |
        (((((start >>  8) & 0xff) << 8) + (((end >>  8) & 0xff) - ((start >>  8) & 0xff)) * pos) >> 8) <<  8 |
        (((((start      ) & 0xff) << 8) + (((end      ) & 0xff) - ((start      ) & 0xff)) * pos) >> 8);
}

static ARGB blend_colors(ARGB start, ARGB end, REAL position)
{
    INT start_a, end_a, final_a;
    INT pos;

    pos = (INT)(position * 255.0f + 0.5f);

    start_a = ((start >> 24) & 0xff) * (pos ^ 0xff);
    end_a = ((end >> 24) & 0xff) * pos;

    final_a = start_a + end_a;

    if (final_a < 0xff) return 0;

    return (final_a / 0xff) << 24 |
        ((((start >> 16) & 0xff) * start_a + (((end >> 16) & 0xff) * end_a)) / final_a) << 16 |
        ((((start >> 8) & 0xff) * start_a + (((end >> 8) & 0xff) * end_a)) / final_a) << 8 |
        (((start & 0xff) * start_a + ((end & 0xff) * end_a)) / final_a);
}

static ARGB blend_line_gradient(GpLineGradient* brush, REAL position)
{
    REAL blendfac;

    /* clamp to between 0.0 and 1.0, using the wrap mode */
    position = (position - brush->rect.X) / brush->rect.Width;
    if (brush->wrap == WrapModeTile)
    {
        position = fmodf(position, 1.0f);
        if (position < 0.0f) position += 1.0f;
    }
    else /* WrapModeFlip* */
    {
        position = fmodf(position, 2.0f);
        if (position < 0.0f) position += 2.0f;
        if (position > 1.0f) position = 2.0f - position;
    }

    if (brush->blendcount == 1)
        blendfac = position;
    else
    {
        int i=1;
        REAL left_blendpos, left_blendfac, right_blendpos, right_blendfac;
        REAL range;

        /* locate the blend positions surrounding this position */
        while (position > brush->blendpos[i])
            i++;

        /* interpolate between the blend positions */
        left_blendpos = brush->blendpos[i-1];
        left_blendfac = brush->blendfac[i-1];
        right_blendpos = brush->blendpos[i];
        right_blendfac = brush->blendfac[i];
        range = right_blendpos - left_blendpos;
        blendfac = (left_blendfac * (right_blendpos - position) +
                    right_blendfac * (position - left_blendpos)) / range;
    }

    if (brush->pblendcount == 0)
        return blend_colors(brush->startcolor, brush->endcolor, blendfac);
    else
    {
        int i=1;
        ARGB left_blendcolor, right_blendcolor;
        REAL left_blendpos, right_blendpos;

        /* locate the blend colors surrounding this position */
        while (blendfac > brush->pblendpos[i])
            i++;

        /* interpolate between the blend colors */
        left_blendpos = brush->pblendpos[i-1];
        left_blendcolor = brush->pblendcolor[i-1];
        right_blendpos = brush->pblendpos[i];
        right_blendcolor = brush->pblendcolor[i];
        blendfac = (blendfac - left_blendpos) / (right_blendpos - left_blendpos);
        return blend_colors(left_blendcolor, right_blendcolor, blendfac);
    }
}

static BOOL round_color_matrix(const ColorMatrix *matrix, int values[5][5])
{
    /* Convert floating point color matrix to int[5][5], return TRUE if it's an identity */
    BOOL identity = TRUE;
    int i, j;

    for (i=0; i<4; i++)
        for (j=0; j<5; j++)
        {
            if (matrix->m[j][i] != (i == j ? 1.0 : 0.0))
                identity = FALSE;
            values[j][i] = gdip_round(matrix->m[j][i] * 256.0);
        }

    return identity;
}

static ARGB transform_color(ARGB color, int matrix[5][5])
{
    int val[5], res[4];
    int i, j;
    unsigned char a, r, g, b;

    val[0] = ((color >> 16) & 0xff); /* red */
    val[1] = ((color >> 8) & 0xff); /* green */
    val[2] = (color & 0xff); /* blue */
    val[3] = ((color >> 24) & 0xff); /* alpha */
    val[4] = 255; /* translation */

    for (i=0; i<4; i++)
    {
        res[i] = 0;

        for (j=0; j<5; j++)
            res[i] += matrix[j][i] * val[j];
    }

    a = min(max(res[3] / 256, 0), 255);
    r = min(max(res[0] / 256, 0), 255);
    g = min(max(res[1] / 256, 0), 255);
    b = min(max(res[2] / 256, 0), 255);

    return (a << 24) | (r << 16) | (g << 8) | b;
}

static BOOL color_is_gray(ARGB color)
{
    unsigned char r, g, b;

    r = (color >> 16) & 0xff;
    g = (color >> 8) & 0xff;
    b = color & 0xff;

    return (r == g) && (g == b);
}

/* returns preferred pixel format for the applied attributes */
PixelFormat apply_image_attributes(const GpImageAttributes *attributes, LPBYTE data,
    UINT width, UINT height, INT stride, ColorAdjustType type, PixelFormat fmt)
{
    UINT x, y;
    INT i;

    if ((attributes->noop[type] == IMAGEATTR_NOOP_UNDEFINED &&
            attributes->noop[ColorAdjustTypeDefault] == IMAGEATTR_NOOP_SET) ||
            (attributes->noop[type] == IMAGEATTR_NOOP_SET))
        return fmt;

    if (attributes->colorkeys[type].enabled ||
        attributes->colorkeys[ColorAdjustTypeDefault].enabled)
    {
        const struct color_key *key;
        BYTE min_blue, min_green, min_red;
        BYTE max_blue, max_green, max_red;

        if (!data || fmt != PixelFormat32bppARGB)
            return PixelFormat32bppARGB;

        if (attributes->colorkeys[type].enabled)
            key = &attributes->colorkeys[type];
        else
            key = &attributes->colorkeys[ColorAdjustTypeDefault];

        min_blue = key->low&0xff;
        min_green = (key->low>>8)&0xff;
        min_red = (key->low>>16)&0xff;

        max_blue = key->high&0xff;
        max_green = (key->high>>8)&0xff;
        max_red = (key->high>>16)&0xff;

        for (y=0; y<height; y++)
            for (x=0; x<width; x++)
            {
                ARGB *src_color;
                BYTE blue, green, red;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);
                blue = *src_color&0xff;
                green = (*src_color>>8)&0xff;
                red = (*src_color>>16)&0xff;
                if (blue >= min_blue && green >= min_green && red >= min_red &&
                    blue <= max_blue && green <= max_green && red <= max_red)
                    *src_color = 0x00000000;
            }
    }

    if (attributes->colorremaptables[type].enabled ||
        attributes->colorremaptables[ColorAdjustTypeDefault].enabled)
    {
        const struct color_remap_table *table;

        if (!data || fmt != PixelFormat32bppARGB)
            return PixelFormat32bppARGB;

        if (attributes->colorremaptables[type].enabled)
            table = &attributes->colorremaptables[type];
        else
            table = &attributes->colorremaptables[ColorAdjustTypeDefault];

        for (y=0; y<height; y++)
            for (x=0; x<width; x++)
            {
                ARGB *src_color;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);
                for (i=0; i<table->mapsize; i++)
                {
                    if (*src_color == table->colormap[i].oldColor.Argb)
                    {
                        *src_color = table->colormap[i].newColor.Argb;
                        break;
                    }
                }
            }
    }

    if (attributes->colormatrices[type].enabled ||
        attributes->colormatrices[ColorAdjustTypeDefault].enabled)
    {
        const struct color_matrix *colormatrices;
        int color_matrix[5][5];
        int gray_matrix[5][5];
        BOOL identity;

        if (!data || fmt != PixelFormat32bppARGB)
            return PixelFormat32bppARGB;

        if (attributes->colormatrices[type].enabled)
            colormatrices = &attributes->colormatrices[type];
        else
            colormatrices = &attributes->colormatrices[ColorAdjustTypeDefault];

        identity = round_color_matrix(&colormatrices->colormatrix, color_matrix);

        if (colormatrices->flags == ColorMatrixFlagsAltGray)
            identity = (round_color_matrix(&colormatrices->graymatrix, gray_matrix) && identity);

        if (!identity)
        {
            for (y=0; y<height; y++)
            {
                for (x=0; x<width; x++)
                {
                    ARGB *src_color;
                    src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);

                    if (colormatrices->flags == ColorMatrixFlagsDefault ||
                        !color_is_gray(*src_color))
                    {
                        *src_color = transform_color(*src_color, color_matrix);
                    }
                    else if (colormatrices->flags == ColorMatrixFlagsAltGray)
                    {
                        *src_color = transform_color(*src_color, gray_matrix);
                    }
                }
            }
        }
    }

    if (attributes->gamma_enabled[type] ||
        attributes->gamma_enabled[ColorAdjustTypeDefault])
    {
        REAL gamma;

        if (!data || fmt != PixelFormat32bppARGB)
            return PixelFormat32bppARGB;

        if (attributes->gamma_enabled[type])
            gamma = attributes->gamma[type];
        else
            gamma = attributes->gamma[ColorAdjustTypeDefault];

        for (y=0; y<height; y++)
            for (x=0; x<width; x++)
            {
                ARGB *src_color;
                BYTE blue, green, red;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);

                blue = *src_color&0xff;
                green = (*src_color>>8)&0xff;
                red = (*src_color>>16)&0xff;

                /* FIXME: We should probably use a table for this. */
                blue = floorf(powf(blue / 255.0, gamma) * 255.0);
                green = floorf(powf(green / 255.0, gamma) * 255.0);
                red = floorf(powf(red / 255.0, gamma) * 255.0);

                *src_color = (*src_color & 0xff000000) | (red << 16) | (green << 8) | blue;
            }
    }

    return fmt;
}

/* Given a bitmap and its source rectangle, find the smallest rectangle in the
 * bitmap that contains all the pixels we may need to draw it. */
static void get_bitmap_sample_size(InterpolationMode interpolation, WrapMode wrap,
    GpBitmap* bitmap, REAL srcx, REAL srcy, REAL srcwidth, REAL srcheight,
    GpRect *rect)
{
    INT left, top, right, bottom;

    switch (interpolation)
    {
    case InterpolationModeHighQualityBilinear:
    case InterpolationModeHighQualityBicubic:
    /* FIXME: Include a greater range for the prefilter? */
    case InterpolationModeBicubic:
    case InterpolationModeBilinear:
        left = (INT)(floorf(srcx));
        top = (INT)(floorf(srcy));
        right = (INT)(ceilf(srcx+srcwidth));
        bottom = (INT)(ceilf(srcy+srcheight));
        break;
    case InterpolationModeNearestNeighbor:
    default:
        left = gdip_round(srcx);
        top = gdip_round(srcy);
        right = gdip_round(srcx+srcwidth);
        bottom = gdip_round(srcy+srcheight);
        break;
    }

    if (wrap == WrapModeClamp)
    {
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        if (right >= bitmap->width)
            right = bitmap->width-1;
        if (bottom >= bitmap->height)
            bottom = bitmap->height-1;
        if (bottom < top || right < left)
            /* entirely outside image, just sample a pixel so we don't have to
             * special-case this later */
            left = top = right = bottom = 0;
    }
    else
    {
        /* In some cases we can make the rectangle smaller here, but the logic
         * is hard to get right, and tiling suggests we're likely to use the
         * entire source image. */
        if (left < 0 || right >= bitmap->width)
        {
            left = 0;
            right = bitmap->width-1;
        }

        if (top < 0 || bottom >= bitmap->height)
        {
            top = 0;
            bottom = bitmap->height-1;
        }
    }

    rect->X = left;
    rect->Y = top;
    rect->Width = right - left + 1;
    rect->Height = bottom - top + 1;
}

static ARGB sample_bitmap_pixel(GDIPCONST GpRect *src_rect, LPBYTE bits, UINT width,
    UINT height, INT x, INT y, GDIPCONST GpImageAttributes *attributes)
{
    if (attributes->wrap == WrapModeClamp)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return attributes->outside_color;
    }
    else
    {
        /* Tiling. Make sure co-ordinates are positive as it simplifies the math. */
        if (x < 0)
            x = width*2 + x % (INT)(width * 2);
        if (y < 0)
            y = height*2 + y % (INT)(height * 2);

        if (attributes->wrap & WrapModeTileFlipX)
        {
            if ((x / width) % 2 == 0)
                x = x % width;
            else
                x = width - 1 - x % width;
        }
        else
            x = x % width;

        if (attributes->wrap & WrapModeTileFlipY)
        {
            if ((y / height) % 2 == 0)
                y = y % height;
            else
                y = height - 1 - y % height;
        }
        else
            y = y % height;
    }

    if (x < src_rect->X || y < src_rect->Y || x >= src_rect->X + src_rect->Width || y >= src_rect->Y + src_rect->Height)
    {
        ERR("out of range pixel requested\n");
        return 0xffcd0084;
    }

    return ((DWORD*)(bits))[(x - src_rect->X) + (y - src_rect->Y) * src_rect->Width];
}

static FORCEINLINE int positive_ceilf(float f)
{
    return f - (int)f > 0.0f ? f + 1.0f : f;
}

static ARGB resample_bitmap_pixel(GDIPCONST GpRect *src_rect, LPBYTE bits, UINT width,
    UINT height, GpPointF *point, GDIPCONST GpImageAttributes *attributes,
    InterpolationMode interpolation, PixelOffsetMode offset_mode)
{
    static int fixme;

    switch (interpolation)
    {
    default:
        if (!fixme++)
            FIXME("Unimplemented interpolation %i\n", interpolation);
        /* fall-through */
    case InterpolationModeBilinear:
    {
        REAL leftxf, topyf;
        INT leftx, rightx, topy, bottomy;
        ARGB topleft, topright, bottomleft, bottomright;
        ARGB top, bottom;
        float x_offset;

        leftx = (INT)point->X;
        leftxf = (REAL)leftx;
        rightx = positive_ceilf(point->X);
        topy = (INT)point->Y;
        topyf = (REAL)topy;
        bottomy = positive_ceilf(point->Y);

        if (leftx == rightx && topy == bottomy)
            return sample_bitmap_pixel(src_rect, bits, width, height,
                leftx, topy, attributes);

        topleft = sample_bitmap_pixel(src_rect, bits, width, height,
            leftx, topy, attributes);
        topright = sample_bitmap_pixel(src_rect, bits, width, height,
            rightx, topy, attributes);
        bottomleft = sample_bitmap_pixel(src_rect, bits, width, height,
            leftx, bottomy, attributes);
        bottomright = sample_bitmap_pixel(src_rect, bits, width, height,
            rightx, bottomy, attributes);

        x_offset = point->X - leftxf;
        top = blend_colors(topleft, topright, x_offset);
        bottom = blend_colors(bottomleft, bottomright, x_offset);

        return blend_colors(top, bottom, point->Y - topyf);
    }
    case InterpolationModeNearestNeighbor:
    {
        FLOAT pixel_offset;
        switch (offset_mode)
        {
        default:
        case PixelOffsetModeNone:
        case PixelOffsetModeHighSpeed:
            pixel_offset = 0.5;
            break;

        case PixelOffsetModeHalf:
        case PixelOffsetModeHighQuality:
            pixel_offset = 0.0;
            break;
        }
        return sample_bitmap_pixel(src_rect, bits, width, height,
            floorf(point->X + pixel_offset), floorf(point->Y + pixel_offset), attributes);
    }

    }
}

static ARGB resample_bitmap_pixel_premult(GDIPCONST GpRect *src_rect, LPBYTE bits, UINT width,
    UINT height, GpPointF *point, GDIPCONST GpImageAttributes *attributes,
    InterpolationMode interpolation, PixelOffsetMode offset_mode)
{
    static int fixme;

    switch (interpolation)
    {
    default:
        if (!fixme++)
            FIXME("Unimplemented interpolation %i\n", interpolation);
        /* fall-through */
    case InterpolationModeBilinear:
    {
        REAL leftxf, topyf;
        INT leftx, rightx, topy, bottomy;
        ARGB topleft, topright, bottomleft, bottomright;
        ARGB top, bottom;
        float x_offset;

        leftx = (INT)point->X;
        leftxf = (REAL)leftx;
        rightx = positive_ceilf(point->X);
        topy = (INT)point->Y;
        topyf = (REAL)topy;
        bottomy = positive_ceilf(point->Y);

        if (leftx == rightx && topy == bottomy)
            return sample_bitmap_pixel(src_rect, bits, width, height,
                leftx, topy, attributes);

        topleft = sample_bitmap_pixel(src_rect, bits, width, height,
            leftx, topy, attributes);
        topright = sample_bitmap_pixel(src_rect, bits, width, height,
            rightx, topy, attributes);
        bottomleft = sample_bitmap_pixel(src_rect, bits, width, height,
            leftx, bottomy, attributes);
        bottomright = sample_bitmap_pixel(src_rect, bits, width, height,
            rightx, bottomy, attributes);

        x_offset = point->X - leftxf;
        top = blend_colors_premult(topleft, topright, x_offset);
        bottom = blend_colors_premult(bottomleft, bottomright, x_offset);

        return blend_colors_premult(top, bottom, point->Y - topyf);
    }
    case InterpolationModeNearestNeighbor:
    {
        FLOAT pixel_offset;
        switch (offset_mode)
        {
        default:
        case PixelOffsetModeNone:
        case PixelOffsetModeHighSpeed:
            pixel_offset = 0.5;
            break;

        case PixelOffsetModeHalf:
        case PixelOffsetModeHighQuality:
            pixel_offset = 0.0;
            break;
        }
        return sample_bitmap_pixel(src_rect, bits, width, height,
            floorf(point->X + pixel_offset), point->Y + pixel_offset, attributes);
    }

    }
}

static REAL intersect_line_scanline(const GpPointF *p1, const GpPointF *p2, REAL y)
{
    return (p1->X - p2->X) * (p2->Y - y) / (p2->Y - p1->Y) + p2->X;
}

/* is_fill is TRUE if filling regions, FALSE for drawing primitives */
static BOOL brush_can_fill_path(GpBrush *brush, BOOL is_fill)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    {
        if (is_fill)
            return TRUE;
        else
        {
            /* cannot draw semi-transparent colors */
            return (((GpSolidFill*)brush)->color & 0xff000000) == 0xff000000;
        }
    }
    case BrushTypeHatchFill:
    {
        GpHatch *hatch = (GpHatch*)brush;
        return ((hatch->forecol & 0xff000000) == 0xff000000) &&
               ((hatch->backcol & 0xff000000) == 0xff000000);
    }
    case BrushTypeLinearGradient:
    case BrushTypeTextureFill:
    /* Gdi32 isn't much help with these, so we should use brush_fill_pixels instead. */
    default:
        return FALSE;
    }
}

static GpStatus brush_fill_path(GpGraphics *graphics, GpBrush *brush)
{
    HDC hdc;
    GpStatus status;

    status = gdi_dc_acquire(graphics, &hdc);
    if (status != Ok)
        return status;

    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    {
        GpSolidFill *fill = (GpSolidFill*)brush;
        HBITMAP bmp = ARGB2BMP(fill->color);

        if (bmp)
        {
            RECT rc;
            /* partially transparent fill */

            if (!SelectClipPath(hdc, RGN_AND))
            {
                status = GenericError;
                DeleteObject(bmp);
                break;
            }
            if (GetClipBox(hdc, &rc) != NULLREGION)
            {
                HDC src_hdc = CreateCompatibleDC(NULL);

                if (!src_hdc)
                {
                    status = OutOfMemory;
                    DeleteObject(bmp);
                    break;
                }

                SelectObject(src_hdc, bmp);
                gdi_alpha_blend(graphics, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                src_hdc, 0, 0, 1, 1);
                DeleteDC(src_hdc);
            }

            DeleteObject(bmp);
            break;
        }
        /* else fall through */
    }
    default:
    {
        HBRUSH gdibrush, old_brush;

        gdibrush = create_gdi_brush(brush, graphics->origin_x, graphics->origin_y);
        if (!gdibrush)
        {
            status = OutOfMemory;
            break;
        }

        old_brush = SelectObject(hdc, gdibrush);
        FillPath(hdc);
        SelectObject(hdc, old_brush);
        DeleteObject(gdibrush);
        break;
    }
    }

    gdi_dc_release(graphics, hdc);

    return status;
}

static BOOL brush_can_fill_pixels(GpBrush *brush)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    case BrushTypeHatchFill:
    case BrushTypeLinearGradient:
    case BrushTypeTextureFill:
    case BrushTypePathGradient:
        return TRUE;
    default:
        return FALSE;
    }
}

static GpStatus brush_fill_pixels(GpGraphics *graphics, GpBrush *brush,
    DWORD *argb_pixels, GpRect *fill_area, UINT cdwStride)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    {
        int x, y;
        GpSolidFill *fill = (GpSolidFill*)brush;
        for (y=0; y<fill_area->Height; y++)
            for (x=0; x<fill_area->Width; x++)
                argb_pixels[x + y*cdwStride] = fill->color;
        return Ok;
    }
    case BrushTypeHatchFill:
    {
        int x, y;
        GpHatch *fill = (GpHatch*)brush;
        const unsigned char *hatch_data;
        ARGB hatch_palette[4];

        if (get_hatch_data(fill->hatchstyle, &hatch_data) != Ok)
            return NotImplemented;

        init_hatch_palette(hatch_palette, fill->forecol, fill->backcol);

        /* See create_hatch_bitmap for an explanation of how index is derived. */
        for (y = 0; y < fill_area->Height; y++, argb_pixels += cdwStride)
        {
            const int hy = ~(y + fill_area->Y - graphics->origin_y) & 7;
            const int hx = graphics->origin_x & 7;
            const unsigned int row = (0x10101 * hatch_data[hy]) >> hx;

            for (x = 0; x < fill_area->Width; x++)
            {
                const unsigned int srow = row >> (~(x + fill_area->X) & 7);
                int index;
                if (hatch_data[8])
                    index = (srow & 1) ? 2 : (srow & 0x82) ? 1 : 0;
                else
                    index = (srow & 1) ? 3 : 0;

                argb_pixels[x] = hatch_palette[index];
            }
        }

        return Ok;
    }
    case BrushTypeLinearGradient:
    {
        GpLineGradient *fill = (GpLineGradient*)brush;
        GpPointF draw_points[3];
        GpStatus stat;
        int x, y;

        draw_points[0].X = fill_area->X;
        draw_points[0].Y = fill_area->Y;
        draw_points[1].X = fill_area->X+1;
        draw_points[1].Y = fill_area->Y;
        draw_points[2].X = fill_area->X;
        draw_points[2].Y = fill_area->Y+1;

        /* Transform the points to a co-ordinate space where X is the point's
         * position in the gradient, 0.0 being the start point and 1.0 the
         * end point. */
        stat = gdip_transform_points(graphics, CoordinateSpaceWorld,
            WineCoordinateSpaceGdiDevice, draw_points, 3);

        if (stat == Ok)
        {
            GpMatrix world_to_gradient = fill->transform;

            stat = GdipInvertMatrix(&world_to_gradient);
            if (stat == Ok)
                stat = GdipTransformMatrixPoints(&world_to_gradient, draw_points, 3);
        }

        if (stat == Ok)
        {
            REAL x_delta = draw_points[1].X - draw_points[0].X;
            REAL y_delta = draw_points[2].X - draw_points[0].X;

            for (y=0; y<fill_area->Height; y++)
            {
                for (x=0; x<fill_area->Width; x++)
                {
                    REAL pos = draw_points[0].X + x * x_delta + y * y_delta;

                    argb_pixels[x + y*cdwStride] = blend_line_gradient(fill, pos);
                }
            }
        }

        return stat;
    }
    case BrushTypeTextureFill:
    {
        GpTexture *fill = (GpTexture*)brush;
        GpPointF draw_points[3];
        GpStatus stat;
        int x, y;
        GpBitmap *bitmap;
        int src_stride;
        GpRect src_area;

        if (fill->image->type != ImageTypeBitmap)
        {
            FIXME("metafile texture brushes not implemented\n");
            return NotImplemented;
        }

        bitmap = (GpBitmap*)fill->image;
        src_stride = sizeof(ARGB) * bitmap->width;

        src_area.X = src_area.Y = 0;
        src_area.Width = bitmap->width;
        src_area.Height = bitmap->height;

        draw_points[0].X = fill_area->X;
        draw_points[0].Y = fill_area->Y;
        draw_points[1].X = fill_area->X+1;
        draw_points[1].Y = fill_area->Y;
        draw_points[2].X = fill_area->X;
        draw_points[2].Y = fill_area->Y+1;

        /* Transform the points to the co-ordinate space of the bitmap. */
        stat = gdip_transform_points(graphics, CoordinateSpaceWorld,
            WineCoordinateSpaceGdiDevice, draw_points, 3);

        if (stat == Ok)
        {
            GpMatrix world_to_texture = fill->transform;

            stat = GdipInvertMatrix(&world_to_texture);
            if (stat == Ok)
                stat = GdipTransformMatrixPoints(&world_to_texture, draw_points, 3);
        }

        if (stat == Ok && !fill->bitmap_bits)
        {
            BitmapData lockeddata;

            fill->bitmap_bits = calloc(bitmap->width * bitmap->height, sizeof(ARGB));
            if (!fill->bitmap_bits)
                stat = OutOfMemory;

            if (stat == Ok)
            {
                lockeddata.Width = bitmap->width;
                lockeddata.Height = bitmap->height;
                lockeddata.Stride = src_stride;
                lockeddata.PixelFormat = PixelFormat32bppARGB;
                lockeddata.Scan0 = fill->bitmap_bits;

                stat = GdipBitmapLockBits(bitmap, &src_area, ImageLockModeRead|ImageLockModeUserInputBuf,
                    PixelFormat32bppARGB, &lockeddata);
            }

            if (stat == Ok)
                stat = GdipBitmapUnlockBits(bitmap, &lockeddata);

            if (stat == Ok)
                apply_image_attributes(fill->imageattributes, fill->bitmap_bits,
                    bitmap->width, bitmap->height,
                    src_stride, ColorAdjustTypeBitmap, lockeddata.PixelFormat);

            if (stat != Ok)
            {
                free(fill->bitmap_bits);
                fill->bitmap_bits = NULL;
            }
        }

        if (stat == Ok)
        {
            REAL x_dx = draw_points[1].X - draw_points[0].X;
            REAL x_dy = draw_points[1].Y - draw_points[0].Y;
            REAL y_dx = draw_points[2].X - draw_points[0].X;
            REAL y_dy = draw_points[2].Y - draw_points[0].Y;

            for (y=0; y<fill_area->Height; y++)
            {
                for (x=0; x<fill_area->Width; x++)
                {
                    GpPointF point;
                    point.X = draw_points[0].X + x * x_dx + y * y_dx;
                    point.Y = draw_points[0].Y + x * x_dy + y * y_dy;

                    argb_pixels[x + y*cdwStride] = resample_bitmap_pixel(
                        &src_area, fill->bitmap_bits, bitmap->width, bitmap->height,
                        &point, fill->imageattributes, graphics->interpolation,
                        graphics->pixeloffset);
                }
            }
        }

        return stat;
    }
    case BrushTypePathGradient:
    {
        GpPathGradient *fill = (GpPathGradient*)brush;
        GpPath *flat_path;
        GpMatrix world_to_device;
        GpStatus stat;
        int i, figure_start=0;
        GpPointF start_point, end_point, center_point;
        BYTE type;
        REAL min_yf, max_yf, line1_xf, line2_xf;
        INT min_y, max_y, min_x, max_x;
        INT x, y;
        ARGB outer_color;
        static BOOL transform_fixme_once;

        if (fill->focus.X != 0.0 || fill->focus.Y != 0.0)
        {
            static int once;
            if (!once++)
                FIXME("path gradient focus not implemented\n");
        }

        if (fill->gamma)
        {
            static int once;
            if (!once++)
                FIXME("path gradient gamma correction not implemented\n");
        }

        if (fill->blendcount)
        {
            static int once;
            if (!once++)
                FIXME("path gradient blend not implemented\n");
        }

        if (fill->pblendcount)
        {
            static int once;
            if (!once++)
                FIXME("path gradient preset blend not implemented\n");
        }

        if (!transform_fixme_once)
        {
            BOOL is_identity=TRUE;
            GdipIsMatrixIdentity(&fill->transform, &is_identity);
            if (!is_identity)
            {
                FIXME("path gradient transform not implemented\n");
                transform_fixme_once = TRUE;
            }
        }

        stat = GdipClonePath(fill->path, &flat_path);

        if (stat != Ok)
            return stat;

        stat = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice,
            CoordinateSpaceWorld, &world_to_device);
        if (stat == Ok)
        {
            stat = GdipTransformPath(flat_path, &world_to_device);

            if (stat == Ok)
            {
                center_point = fill->center;
                stat = GdipTransformMatrixPoints(&world_to_device, &center_point, 1);
            }

            if (stat == Ok)
                stat = GdipFlattenPath(flat_path, NULL, 0.5);
        }

        if (stat != Ok)
        {
            GdipDeletePath(flat_path);
            return stat;
        }

        for (i=0; i<flat_path->pathdata.Count; i++)
        {
            int start_center_line=0, end_center_line=0;
            BOOL seen_start = FALSE, seen_end = FALSE, seen_center = FALSE;
            REAL center_distance;
            ARGB start_color, end_color;
            REAL dy, dx;

            type = flat_path->pathdata.Types[i];

            if ((type&PathPointTypePathTypeMask) == PathPointTypeStart)
                figure_start = i;

            start_point = flat_path->pathdata.Points[i];

            start_color = fill->surroundcolors[min(i, fill->surroundcolorcount-1)];

            if ((type&PathPointTypeCloseSubpath) == PathPointTypeCloseSubpath || i+1 >= flat_path->pathdata.Count)
            {
                end_point = flat_path->pathdata.Points[figure_start];
                end_color = fill->surroundcolors[min(figure_start, fill->surroundcolorcount-1)];
            }
            else if ((flat_path->pathdata.Types[i+1] & PathPointTypePathTypeMask) == PathPointTypeLine)
            {
                end_point = flat_path->pathdata.Points[i+1];
                end_color = fill->surroundcolors[min(i+1, fill->surroundcolorcount-1)];
            }
            else
                continue;

            outer_color = start_color;

            min_yf = center_point.Y;
            if (min_yf > start_point.Y) min_yf = start_point.Y;
            if (min_yf > end_point.Y) min_yf = end_point.Y;

            if (min_yf < fill_area->Y)
                min_y = fill_area->Y;
            else
                min_y = (INT)ceil(min_yf);

            max_yf = center_point.Y;
            if (max_yf < start_point.Y) max_yf = start_point.Y;
            if (max_yf < end_point.Y) max_yf = end_point.Y;

            if (max_yf > fill_area->Y + fill_area->Height)
                max_y = fill_area->Y + fill_area->Height;
            else
                max_y = (INT)ceil(max_yf);

            dy = end_point.Y - start_point.Y;
            dx = end_point.X - start_point.X;

            /* This is proportional to the distance from start-end line to center point. */
            center_distance = dy * (start_point.X - center_point.X) +
                dx * (center_point.Y - start_point.Y);

            for (y=min_y; y<max_y; y++)
            {
                REAL yf = (REAL)y;

                if (!seen_start && yf >= start_point.Y)
                {
                    seen_start = TRUE;
                    start_center_line ^= 1;
                }
                if (!seen_end && yf >= end_point.Y)
                {
                    seen_end = TRUE;
                    end_center_line ^= 1;
                }
                if (!seen_center && yf >= center_point.Y)
                {
                    seen_center = TRUE;
                    start_center_line ^= 1;
                    end_center_line ^= 1;
                }

                if (start_center_line)
                    line1_xf = intersect_line_scanline(&start_point, &center_point, yf);
                else
                    line1_xf = intersect_line_scanline(&start_point, &end_point, yf);

                if (end_center_line)
                    line2_xf = intersect_line_scanline(&end_point, &center_point, yf);
                else
                    line2_xf = intersect_line_scanline(&start_point, &end_point, yf);

                if (line1_xf < line2_xf)
                {
                    min_x = (INT)ceil(line1_xf);
                    max_x = (INT)ceil(line2_xf);
                }
                else
                {
                    min_x = (INT)ceil(line2_xf);
                    max_x = (INT)ceil(line1_xf);
                }

                if (min_x < fill_area->X)
                    min_x = fill_area->X;
                if (max_x > fill_area->X + fill_area->Width)
                    max_x = fill_area->X + fill_area->Width;

                for (x=min_x; x<max_x; x++)
                {
                    REAL xf = (REAL)x;
                    REAL distance;

                    if (start_color != end_color)
                    {
                        REAL blend_amount, pdy, pdx;
                        pdy = yf - center_point.Y;
                        pdx = xf - center_point.X;

                        if (fabs(pdx) <= 0.001 && fabs(pdy) <= 0.001)
                        {
                            /* Too close to center point, don't try to calculate outer color */
                            outer_color = start_color;
                        }
                        else
                        {
                            blend_amount = ( (center_point.Y - start_point.Y) * pdx + (start_point.X - center_point.X) * pdy ) / ( dy * pdx - dx * pdy );
                            outer_color = blend_colors(start_color, end_color, blend_amount);
                        }
                    }

                    distance = (end_point.Y - start_point.Y) * (start_point.X - xf) +
                        (end_point.X - start_point.X) * (yf - start_point.Y);

                    distance = distance / center_distance;

                    argb_pixels[(x-fill_area->X) + (y-fill_area->Y)*cdwStride] =
                        blend_colors(outer_color, fill->centercolor, distance);
                }
            }
        }

        GdipDeletePath(flat_path);
        return stat;
    }
    default:
        return NotImplemented;
    }
}

/* Draws the linecap the specified color and size on the hdc.  The linecap is in
 * direction of the line from x1, y1 to x2, y2 and is anchored on x2, y2. Probably
 * should not be called on an hdc that has a path you care about. */
static void draw_cap(GpGraphics *graphics, COLORREF color, GpLineCap cap, REAL size,
    const GpCustomLineCap *custom, REAL x1, REAL y1, REAL x2, REAL y2)
{
    HDC hdc;
    HGDIOBJ oldbrush = NULL, oldpen = NULL;
    GpMatrix matrix;
    HBRUSH brush = NULL;
    HPEN pen = NULL;
    PointF ptf[4], *custptf = NULL;
    POINT pt[4], *custpt = NULL;
    BYTE *tp = NULL;
    REAL theta, dsmall, dbig, dx, dy = 0.0;
    INT i, count;
    LOGBRUSH lb;
    BOOL customstroke;

    if((x1 == x2) && (y1 == y2))
        return;

    gdi_dc_acquire(graphics, &hdc);

    theta = gdiplus_atan2(y2 - y1, x2 - x1);

    customstroke = (cap == LineCapCustom) && custom && (!custom->fill);
    if(!customstroke){
        brush = CreateSolidBrush(color);
        lb.lbStyle = BS_SOLID;
        lb.lbColor = color;
        lb.lbHatch = 0;
        pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT |
                           PS_JOIN_MITER, 1, &lb, 0,
                           NULL);
        oldbrush = SelectObject(hdc, brush);
        oldpen = SelectObject(hdc, pen);
    }

    switch(cap){
        case LineCapFlat:
            break;
        case LineCapSquare:
        case LineCapSquareAnchor:
        case LineCapDiamondAnchor:
            size = size * (cap & LineCapNoAnchor ? ANCHOR_WIDTH : 1.0) / 2.0;
            if(cap == LineCapDiamondAnchor){
                dsmall = cos(theta + M_PI_2) * size;
                dbig = sin(theta + M_PI_2) * size;
            }
            else{
                dsmall = cos(theta + M_PI_4) * size;
                dbig = sin(theta + M_PI_4) * size;
            }

            ptf[0].X = x2 - dsmall;
            ptf[1].X = x2 + dbig;

            ptf[0].Y = y2 - dbig;
            ptf[3].Y = y2 + dsmall;

            ptf[1].Y = y2 - dsmall;
            ptf[2].Y = y2 + dbig;

            ptf[3].X = x2 - dbig;
            ptf[2].X = x2 + dsmall;

            gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptf, 4);

            round_points(pt, ptf, 4);

            Polygon(hdc, pt, 4);

            break;
        case LineCapArrowAnchor:
            size = size * 4.0 / sqrt(3.0);

            dx = cos(M_PI / 6.0 + theta) * size;
            dy = sin(M_PI / 6.0 + theta) * size;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;

            dx = cos(- M_PI / 6.0 + theta) * size;
            dy = sin(- M_PI / 6.0 + theta) * size;

            ptf[1].X = x2 - dx;
            ptf[1].Y = y2 - dy;

            ptf[2].X = x2;
            ptf[2].Y = y2;

            gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptf, 3);

            round_points(pt, ptf, 3);

            Polygon(hdc, pt, 3);

            break;
        case LineCapRoundAnchor:
            dx = dy = ANCHOR_WIDTH * size / 2.0;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;
            ptf[1].X = x2 + dx;
            ptf[1].Y = y2 + dy;

            gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptf, 2);

            round_points(pt, ptf, 2);

            Ellipse(hdc, pt[0].x, pt[0].y, pt[1].x, pt[1].y);

            break;
        case LineCapTriangle:
            size = size / 2.0;
            dx = cos(M_PI_2 + theta) * size;
            dy = sin(M_PI_2 + theta) * size;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;
            ptf[1].X = x2 + dx;
            ptf[1].Y = y2 + dy;

            dx = cos(theta) * size;
            dy = sin(theta) * size;

            ptf[2].X = x2 + dx;
            ptf[2].Y = y2 + dy;

            gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptf, 3);

            round_points(pt, ptf, 3);

            Polygon(hdc, pt, 3);

            break;
        case LineCapRound:
            dx = dy = size / 2.0;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;
            ptf[1].X = x2 + dx;
            ptf[1].Y = y2 + dy;

            dx = -cos(M_PI_2 + theta) * size;
            dy = -sin(M_PI_2 + theta) * size;

            ptf[2].X = x2 - dx;
            ptf[2].Y = y2 - dy;
            ptf[3].X = x2 + dx;
            ptf[3].Y = y2 + dy;

            gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptf, 4);

            round_points(pt, ptf, 4);

            Pie(hdc, pt[0].x, pt[0].y, pt[1].x, pt[1].y, pt[2].x,
                pt[2].y, pt[3].x, pt[3].y);

            break;
        case LineCapCustom:
            if(!custom)
                break;

            if (custom->type == CustomLineCapTypeAdjustableArrow)
            {
                GpAdjustableArrowCap *arrow = (GpAdjustableArrowCap *)custom;
                if (arrow->cap.fill && arrow->height <= 0.0)
                    break;
            }

            count = custom->pathdata.Count;
            custptf = malloc(count * sizeof(PointF));
            custpt = malloc(count * sizeof(POINT));
            tp = malloc(count);

            if(!custptf || !custpt || !tp)
                goto custend;

            memcpy(custptf, custom->pathdata.Points, count * sizeof(PointF));

            GdipSetMatrixElements(&matrix, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
            GdipScaleMatrix(&matrix, size, size, MatrixOrderAppend);
            GdipRotateMatrix(&matrix, (180.0 / M_PI) * (theta - M_PI_2),
                             MatrixOrderAppend);
            GdipTranslateMatrix(&matrix, x2, y2, MatrixOrderAppend);
            GdipTransformMatrixPoints(&matrix, custptf, count);

            gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, custptf, count);

            round_points(custpt, custptf, count);

            for(i = 0; i < count; i++)
                tp[i] = convert_path_point_type(custom->pathdata.Types[i]);

            if(custom->fill){
                BeginPath(hdc);
                PolyDraw(hdc, custpt, tp, count);
                EndPath(hdc);
                StrokeAndFillPath(hdc);
            }
            else
                PolyDraw(hdc, custpt, tp, count);

custend:
            free(custptf);
            free(custpt);
            free(tp);
            break;
        default:
            break;
    }

    if(!customstroke){
        SelectObject(hdc, oldbrush);
        SelectObject(hdc, oldpen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    gdi_dc_release(graphics, hdc);
}

/* Shortens the line by the given percent by changing x2, y2.
 * If percent is > 1.0 then the line will change direction.
 * If percent is negative it can lengthen the line. */
static void shorten_line_percent(REAL x1, REAL  y1, REAL *x2, REAL *y2, REAL percent)
{
    REAL dist, theta, dx, dy;

    if((y1 == *y2) && (x1 == *x2))
        return;

    dist = hypotf(*x2 - x1, *y2 - y1) * -percent;
    theta = gdiplus_atan2((*y2 - y1), (*x2 - x1));
    dx = cos(theta) * dist;
    dy = sin(theta) * dist;

    *x2 = *x2 + dx;
    *y2 = *y2 + dy;
}

/* Shortens the line by the given amount by changing x2, y2.
 * If the amount is greater than the distance, the line will become length 0.
 * If the amount is negative, it can lengthen the line. */
static void shorten_line_amt(REAL x1, REAL y1, REAL *x2, REAL *y2, REAL amt)
{
    REAL dx, dy, percent;

    dx = *x2 - x1;
    dy = *y2 - y1;
    if(dx == 0 && dy == 0)
        return;

    percent = amt / hypotf(dx, dy);
    if(percent >= 1.0){
        *x2 = x1;
        *y2 = y1;
        return;
    }

    shorten_line_percent(x1, y1, x2, y2, percent);
}

/* Conducts a linear search to find the bezier points that will back off
 * the endpoint of the curve by a distance of amt. Linear search works
 * better than binary in this case because there are multiple solutions,
 * and binary searches often find a bad one. I don't think this is what
 * Windows does but short of rendering the bezier without GDI's help it's
 * the best we can do. If rev then work from the start of the passed points
 * instead of the end. */
static void shorten_bezier_amt(GpPointF * pt, REAL amt, BOOL rev)
{
    GpPointF origpt[4];
    REAL percent = 0.00, dx, dy, origx, origy, diff = -1.0;
    INT i, first = 0, second = 1, third = 2, fourth = 3;

    if(rev){
        first = 3;
        second = 2;
        third = 1;
        fourth = 0;
    }

    origx = pt[fourth].X;
    origy = pt[fourth].Y;
    memcpy(origpt, pt, sizeof(GpPointF) * 4);

    for(i = 0; (i < MAX_ITERS) && (diff < amt); i++){
        /* reset bezier points to original values */
        memcpy(pt, origpt, sizeof(GpPointF) * 4);
        /* Perform magic on bezier points. Order is important here.*/
        shorten_line_percent(pt[third].X, pt[third].Y, &pt[fourth].X, &pt[fourth].Y, percent);
        shorten_line_percent(pt[second].X, pt[second].Y, &pt[third].X, &pt[third].Y, percent);
        shorten_line_percent(pt[third].X, pt[third].Y, &pt[fourth].X, &pt[fourth].Y, percent);
        shorten_line_percent(pt[first].X, pt[first].Y, &pt[second].X, &pt[second].Y, percent);
        shorten_line_percent(pt[second].X, pt[second].Y, &pt[third].X, &pt[third].Y, percent);
        shorten_line_percent(pt[third].X, pt[third].Y, &pt[fourth].X, &pt[fourth].Y, percent);

        dx = pt[fourth].X - origx;
        dy = pt[fourth].Y - origy;

        diff = hypotf(dx, dy);
        percent += 0.0005 * amt;
    }
}

/* Draws a combination of bezier curves and lines between points. */
static GpStatus draw_poly(GpGraphics *graphics, GpPen *pen, GDIPCONST GpPointF * pt,
    GDIPCONST BYTE * types, INT count, BOOL caps)
{
    HDC hdc;
    POINT *pti = malloc(count * sizeof(POINT));
    BYTE *tp = malloc(count);
    GpPointF *ptcopy = malloc(count * sizeof(GpPointF));
    INT i, j;
    GpStatus status = GenericError;

    if(!count){
        status = Ok;
        goto end;
    }
    if(!pti || !tp || !ptcopy){
        status = OutOfMemory;
        goto end;
    }

    for(i = 1; i < count; i++){
        if((types[i] & PathPointTypePathTypeMask) == PathPointTypeBezier){
            if((i + 2 >= count) || !(types[i + 1] & PathPointTypeBezier)
                || !(types[i + 2] & PathPointTypeBezier)){
                ERR("Bad bezier points\n");
                goto end;
            }
            i += 2;
        }
    }

    memcpy(ptcopy, pt, count * sizeof(GpPointF));

    /* If we are drawing caps, go through the points and adjust them accordingly,
     * and draw the caps. */
    if(caps){
        switch(types[count - 1] & PathPointTypePathTypeMask){
            case PathPointTypeBezier:
                if(pen->endcap == LineCapArrowAnchor)
                    shorten_bezier_amt(&ptcopy[count - 4], pen->width, FALSE);
                else if((pen->endcap == LineCapCustom) && pen->customend)
                    shorten_bezier_amt(&ptcopy[count - 4],
                                       pen->width * pen->customend->inset, FALSE);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->endcap, pen->width, pen->customend,
                    pt[count - 1].X - (ptcopy[count - 1].X - ptcopy[count - 2].X),
                    pt[count - 1].Y - (ptcopy[count - 1].Y - ptcopy[count - 2].Y),
                    pt[count - 1].X, pt[count - 1].Y);

                break;
            case PathPointTypeLine:
                if(pen->endcap == LineCapArrowAnchor)
                    shorten_line_amt(ptcopy[count - 2].X, ptcopy[count - 2].Y,
                                     &ptcopy[count - 1].X, &ptcopy[count - 1].Y,
                                     pen->width);
                else if((pen->endcap == LineCapCustom) && pen->customend)
                    shorten_line_amt(ptcopy[count - 2].X, ptcopy[count - 2].Y,
                                     &ptcopy[count - 1].X, &ptcopy[count - 1].Y,
                                     pen->customend->inset * pen->width);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->endcap, pen->width, pen->customend,
                         pt[count - 2].X, pt[count - 2].Y, pt[count - 1].X,
                         pt[count - 1].Y);

                break;
            default:
                ERR("Bad path last point\n");
                goto end;
        }

        /* Find start of points */
        for(j = 1; j < count && ((types[j] & PathPointTypePathTypeMask)
            == PathPointTypeStart); j++);

        switch(types[j] & PathPointTypePathTypeMask){
            case PathPointTypeBezier:
                if(pen->startcap == LineCapArrowAnchor)
                    shorten_bezier_amt(&ptcopy[j - 1], pen->width, TRUE);
                else if((pen->startcap == LineCapCustom) && pen->customstart)
                    shorten_bezier_amt(&ptcopy[j - 1],
                                       pen->width * pen->customstart->inset, TRUE);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->startcap, pen->width, pen->customstart,
                    pt[j - 1].X - (ptcopy[j - 1].X - ptcopy[j].X),
                    pt[j - 1].Y - (ptcopy[j - 1].Y - ptcopy[j].Y),
                    pt[j - 1].X, pt[j - 1].Y);

                break;
            case PathPointTypeLine:
                if(pen->startcap == LineCapArrowAnchor)
                    shorten_line_amt(ptcopy[j].X, ptcopy[j].Y,
                                     &ptcopy[j - 1].X, &ptcopy[j - 1].Y,
                                     pen->width);
                else if((pen->startcap == LineCapCustom) && pen->customstart)
                    shorten_line_amt(ptcopy[j].X, ptcopy[j].Y,
                                     &ptcopy[j - 1].X, &ptcopy[j - 1].Y,
                                     pen->customstart->inset * pen->width);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->startcap, pen->width, pen->customstart,
                         pt[j].X, pt[j].Y, pt[j - 1].X,
                         pt[j - 1].Y);

                break;
            default:
                ERR("Bad path points\n");
                goto end;
        }
    }

    gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptcopy, count);

    round_points(pti, ptcopy, count);

    for(i = 0; i < count; i++){
        tp[i] = convert_path_point_type(types[i]);
    }

    status = gdi_dc_acquire(graphics, &hdc);
    if (status != Ok)
        goto end;

    PolyDraw(hdc, pti, tp, count);

    gdi_dc_release(graphics, hdc);

    status = Ok;

end:
    free(pti);
    free(ptcopy);
    free(tp);

    return status;
}

GpStatus trace_path(GpGraphics *graphics, GpPath *path)
{
    HDC hdc;
    GpStatus result;

    result = gdi_dc_acquire(graphics, &hdc);
    if (result != Ok)
        return result;

    BeginPath(hdc);
    result = draw_poly(graphics, NULL, path->pathdata.Points,
                       path->pathdata.Types, path->pathdata.Count, FALSE);
    EndPath(hdc);

    gdi_dc_release(graphics, hdc);

    return result;
}

typedef enum GraphicsContainerType {
    BEGIN_CONTAINER,
    SAVE_GRAPHICS
} GraphicsContainerType;

typedef struct _GraphicsContainerItem {
    struct list entry;
    GraphicsContainer contid;
    GraphicsContainerType type;

    SmoothingMode smoothing;
    CompositingQuality compqual;
    InterpolationMode interpolation;
    CompositingMode compmode;
    TextRenderingHint texthint;
    REAL scale;
    GpUnit unit;
    PixelOffsetMode pixeloffset;
    UINT textcontrast;
    GpMatrix worldtrans;
    GpRegion* clip;
    INT origin_x, origin_y;
} GraphicsContainerItem;

static GpStatus init_container(GraphicsContainerItem** container,
        GDIPCONST GpGraphics* graphics, GraphicsContainerType type){
    GpStatus sts;

    *container = calloc(1, sizeof(GraphicsContainerItem));
    if(!(*container))
        return OutOfMemory;

    (*container)->contid = graphics->contid + 1;
    (*container)->type = type;

    (*container)->smoothing = graphics->smoothing;
    (*container)->compqual = graphics->compqual;
    (*container)->interpolation = graphics->interpolation;
    (*container)->compmode = graphics->compmode;
    (*container)->texthint = graphics->texthint;
    (*container)->scale = graphics->scale;
    (*container)->unit = graphics->unit;
    (*container)->textcontrast = graphics->textcontrast;
    (*container)->pixeloffset = graphics->pixeloffset;
    (*container)->origin_x = graphics->origin_x;
    (*container)->origin_y = graphics->origin_y;
    (*container)->worldtrans = graphics->worldtrans;

    sts = GdipCloneRegion(graphics->clip, &(*container)->clip);
    if(sts != Ok){
        free(*container);
        *container = NULL;
        return sts;
    }

    return Ok;
}

static void delete_container(GraphicsContainerItem* container)
{
    GdipDeleteRegion(container->clip);
    free(container);
}

static GpStatus restore_container(GpGraphics* graphics,
        GDIPCONST GraphicsContainerItem* container){
    GpStatus sts;
    GpRegion *newClip;

    sts = GdipCloneRegion(container->clip, &newClip);
    if(sts != Ok) return sts;

    graphics->worldtrans = container->worldtrans;

    GdipDeleteRegion(graphics->clip);
    graphics->clip = newClip;

    graphics->contid = container->contid - 1;

    graphics->smoothing = container->smoothing;
    graphics->compqual = container->compqual;
    graphics->interpolation = container->interpolation;
    graphics->compmode = container->compmode;
    graphics->texthint = container->texthint;
    graphics->scale = container->scale;
    graphics->unit = container->unit;
    graphics->textcontrast = container->textcontrast;
    graphics->pixeloffset = container->pixeloffset;
    graphics->origin_x = container->origin_x;
    graphics->origin_y = container->origin_y;

    return Ok;
}

static GpStatus get_graphics_bounds(GpGraphics* graphics, GpRectF* rect)
{
    GpStatus stat = get_graphics_device_bounds(graphics, rect);

    if (stat == Ok && has_gdi_dc(graphics))
    {
        GpPointF points[4], min_point, max_point;
        int i;

        points[0].X = points[2].X = rect->X;
        points[0].Y = points[1].Y = rect->Y;
        points[1].X = points[3].X = rect->X + rect->Width;
        points[2].Y = points[3].Y = rect->Y + rect->Height;

        gdip_transform_points(graphics, CoordinateSpaceDevice, WineCoordinateSpaceGdiDevice, points, 4);

        min_point = max_point = points[0];

        for (i=1; i<4; i++)
        {
            if (points[i].X < min_point.X) min_point.X = points[i].X;
            if (points[i].Y < min_point.Y) min_point.Y = points[i].Y;
            if (points[i].X > max_point.X) max_point.X = points[i].X;
            if (points[i].Y > max_point.Y) max_point.Y = points[i].Y;
        }

        rect->X = min_point.X;
        rect->Y = min_point.Y;
        rect->Width = max_point.X - min_point.X;
        rect->Height = max_point.Y - min_point.Y;
    }

    return stat;
}

/* on success, rgn will contain the region of the graphics object which
 * is visible after clipping has been applied */
static GpStatus get_visible_clip_region(GpGraphics *graphics, GpRegion *rgn)
{
    GpStatus stat;
    GpRectF rectf;
    GpRegion* tmp;

    /* Ignore graphics image bounds for metafiles */
    if (is_metafile_graphics(graphics))
        return GdipCombineRegionRegion(rgn, graphics->clip, CombineModeReplace);

    if((stat = get_graphics_bounds(graphics, &rectf)) != Ok)
        return stat;

    if((stat = GdipCreateRegion(&tmp)) != Ok)
        return stat;

    if((stat = GdipCombineRegionRect(tmp, &rectf, CombineModeReplace)) != Ok)
        goto end;

    if((stat = GdipCombineRegionRegion(tmp, graphics->clip, CombineModeIntersect)) != Ok)
        goto end;

    stat = GdipCombineRegionRegion(rgn, tmp, CombineModeReplace);

end:
    GdipDeleteRegion(tmp);
    return stat;
}

void get_log_fontW(const GpFont *font, GpGraphics *graphics, LOGFONTW *lf)
{
    REAL height;

    if (font->unit == UnitPixel)
    {
        height = units_to_pixels(font->emSize, graphics->unit, graphics->yres, graphics->printer_display);
    }
    else
    {
        if (graphics->unit == UnitDisplay || graphics->unit == UnitPixel)
            height = units_to_pixels(font->emSize, font->unit, graphics->xres, graphics->printer_display);
        else
            height = units_to_pixels(font->emSize, font->unit, graphics->yres, graphics->printer_display);
    }

    lf->lfHeight = -(height + 0.5);
    lf->lfWidth = 0;
    lf->lfEscapement = 0;
    lf->lfOrientation = 0;
    lf->lfWeight = font->otm.otmTextMetrics.tmWeight;
    lf->lfItalic = font->otm.otmTextMetrics.tmItalic ? 1 : 0;
    lf->lfUnderline = font->otm.otmTextMetrics.tmUnderlined ? 1 : 0;
    lf->lfStrikeOut = font->otm.otmTextMetrics.tmStruckOut ? 1 : 0;
    lf->lfCharSet = font->otm.otmTextMetrics.tmCharSet;
    lf->lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf->lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf->lfQuality = DEFAULT_QUALITY;
    lf->lfPitchAndFamily = 0;
    lstrcpyW(lf->lfFaceName, font->family->FamilyName);
}

void get_font_hfont(GpGraphics *graphics, GDIPCONST GpFont *font,
                           GDIPCONST GpStringFormat *format, HFONT *hfont,
                           LOGFONTW *lfw_return, GDIPCONST GpMatrix *matrix)
{
    HDC hdc = CreateCompatibleDC(0);
    REAL angle, rel_width, rel_height, font_height;
    LOGFONTW lfw;
    HFONT unscaled_font;
    TEXTMETRICW textmet;

    if (font->unit == UnitPixel || font->unit == UnitWorld)
        font_height = font->emSize;
    else
    {
        REAL unit_scale, res;

        res = (graphics->unit == UnitDisplay || graphics->unit == UnitPixel) ? graphics->xres : graphics->yres;
        unit_scale = units_scale(font->unit, graphics->unit, res, graphics->printer_display);

        font_height = font->emSize * unit_scale;
    }

    transform_properties(graphics, matrix, TRUE, &rel_width, &rel_height, &angle);
    /* If the font unit is not pixels scaling should not be applied */
    if (font->unit != UnitPixel && font->unit != UnitWorld)
    {
        rel_width /= graphics->scale;
        rel_height /= graphics->scale;
    }

    get_log_fontW(font, graphics, &lfw);
    lfw.lfHeight = -gdip_round(font_height * rel_height);
    unscaled_font = CreateFontIndirectW(&lfw);

    SelectObject(hdc, unscaled_font);
    GetTextMetricsW(hdc, &textmet);

    lfw.lfWidth = gdip_round(textmet.tmAveCharWidth * rel_width / rel_height);
    lfw.lfEscapement = lfw.lfOrientation = gdip_round((angle / M_PI) * 1800.0);

    *hfont = CreateFontIndirectW(&lfw);

    if (lfw_return)
        *lfw_return = lfw;

    DeleteDC(hdc);
    DeleteObject(unscaled_font);
}

GpStatus WINGDIPAPI GdipCreateFromHDC(HDC hdc, GpGraphics **graphics)
{
    TRACE("(%p, %p)\n", hdc, graphics);

    return GdipCreateFromHDC2(hdc, NULL, graphics);
}

static void get_gdi_transform(HDC hdc, GpMatrix *matrix)
{
    XFORM xform;

    if (hdc == NULL)
    {
        GdipSetMatrixElements(matrix, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
        return;
    }

    GetTransform(hdc, 0x204, &xform);
    GdipSetMatrixElements(matrix, xform.eM11, xform.eM12, xform.eM21, xform.eM22, xform.eDx, xform.eDy);
}

GpStatus WINGDIPAPI GdipCreateFromHDC2(HDC hdc, HANDLE hDevice, GpGraphics **graphics)
{
    GpStatus retval;
    HBITMAP hbitmap;
    DIBSECTION dib;

    TRACE("(%p, %p, %p)\n", hdc, hDevice, graphics);

    if(hDevice != NULL)
        FIXME("Don't know how to handle parameter hDevice\n");

    if(hdc == NULL)
        return OutOfMemory;

    if(graphics == NULL)
        return InvalidParameter;

    *graphics = calloc(1, sizeof(GpGraphics));
    if(!*graphics)  return OutOfMemory;

    GdipSetMatrixElements(&(*graphics)->worldtrans, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);

    if((retval = GdipCreateRegion(&(*graphics)->clip)) != Ok){
        free(*graphics);
        return retval;
    }

    hbitmap = GetCurrentObject(hdc, OBJ_BITMAP);
    if (hbitmap && GetObjectW(hbitmap, sizeof(dib), &dib) == sizeof(dib) &&
        dib.dsBmih.biBitCount == 32 && dib.dsBmih.biCompression == BI_RGB)
    {
        (*graphics)->alpha_hdc = 1;
    }

    (*graphics)->hdc = hdc;
    (*graphics)->hwnd = WindowFromDC(hdc);
    (*graphics)->owndc = FALSE;
    (*graphics)->smoothing = SmoothingModeDefault;
    (*graphics)->compqual = CompositingQualityDefault;
    (*graphics)->interpolation = InterpolationModeBilinear;
    (*graphics)->pixeloffset = PixelOffsetModeDefault;
    (*graphics)->compmode = CompositingModeSourceOver;
    (*graphics)->unit = UnitDisplay;
    (*graphics)->scale = 1.0;
    (*graphics)->xres = GetDeviceCaps(hdc, LOGPIXELSX);
    (*graphics)->yres = GetDeviceCaps(hdc, LOGPIXELSY);
    (*graphics)->busy = FALSE;
    (*graphics)->textcontrast = 4;
    list_init(&(*graphics)->containers);
    (*graphics)->contid = 0;
    (*graphics)->printer_display = (GetDeviceCaps(hdc, TECHNOLOGY) == DT_RASPRINTER);
    get_gdi_transform(hdc, &(*graphics)->gdi_transform);

    (*graphics)->gdi_clip = CreateRectRgn(0,0,0,0);
    if (!GetClipRgn(hdc, (*graphics)->gdi_clip))
    {
        DeleteObject((*graphics)->gdi_clip);
        (*graphics)->gdi_clip = NULL;
    }

    TRACE("<-- %p\n", *graphics);

    return Ok;
}

GpStatus graphics_from_image(GpImage *image, GpGraphics **graphics)
{
    GpStatus retval;

    *graphics = calloc(1, sizeof(GpGraphics));
    if(!*graphics)  return OutOfMemory;

    GdipSetMatrixElements(&(*graphics)->worldtrans, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    GdipSetMatrixElements(&(*graphics)->gdi_transform, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);

    if((retval = GdipCreateRegion(&(*graphics)->clip)) != Ok){
        free(*graphics);
        return retval;
    }

    (*graphics)->hdc = NULL;
    (*graphics)->hwnd = NULL;
    (*graphics)->owndc = FALSE;
    (*graphics)->image = image;
    /* We have to store the image type here because the image may be freed
     * before GdipDeleteGraphics is called, and metafiles need special treatment. */
    (*graphics)->image_type = image->type;
    (*graphics)->smoothing = SmoothingModeDefault;
    (*graphics)->compqual = CompositingQualityDefault;
    (*graphics)->interpolation = InterpolationModeBilinear;
    (*graphics)->pixeloffset = PixelOffsetModeDefault;
    (*graphics)->compmode = CompositingModeSourceOver;
    (*graphics)->unit = UnitDisplay;
    (*graphics)->scale = 1.0;
    (*graphics)->xres = image->xres;
    (*graphics)->yres = image->yres;
    (*graphics)->busy = FALSE;
    (*graphics)->textcontrast = 4;
    list_init(&(*graphics)->containers);
    (*graphics)->contid = 0;

    TRACE("<-- %p\n", *graphics);

    return Ok;
}

GpStatus WINGDIPAPI GdipCreateFromHWND(HWND hwnd, GpGraphics **graphics)
{
    GpStatus ret;
    HDC hdc;

    TRACE("(%p, %p)\n", hwnd, graphics);

    hdc = GetDC(hwnd);

    if((ret = GdipCreateFromHDC(hdc, graphics)) != Ok)
    {
        ReleaseDC(hwnd, hdc);
        return ret;
    }

    (*graphics)->hwnd = hwnd;
    (*graphics)->owndc = TRUE;

    ReleaseDC(hwnd, hdc);
    (*graphics)->hdc = NULL;

    return Ok;
}

/* FIXME: no icm handling */
GpStatus WINGDIPAPI GdipCreateFromHWNDICM(HWND hwnd, GpGraphics **graphics)
{
    TRACE("(%p, %p)\n", hwnd, graphics);

    return GdipCreateFromHWND(hwnd, graphics);
}

GpStatus WINGDIPAPI GdipCreateStreamOnFile(GDIPCONST WCHAR * filename,
    UINT access, IStream **stream)
{
    DWORD dwMode;
    HRESULT ret;

    TRACE("(%s, %u, %p)\n", debugstr_w(filename), access, stream);

    if(!stream || !filename)
        return InvalidParameter;

    if(access & GENERIC_WRITE)
        dwMode = STGM_SHARE_DENY_WRITE | STGM_WRITE | STGM_CREATE;
    else if(access & GENERIC_READ)
        dwMode = STGM_SHARE_DENY_WRITE | STGM_READ | STGM_FAILIFTHERE;
    else
        return InvalidParameter;

    ret = SHCreateStreamOnFileW(filename, dwMode, stream);

    return hresult_to_status(ret);
}

GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics *graphics)
{
    GraphicsContainerItem *cont, *next;
    GpStatus stat;
    TRACE("(%p)\n", graphics);

    if(!graphics) return InvalidParameter;
    if(graphics->busy) return ObjectBusy;

    assert(graphics->hdc_refs == 0);

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_GraphicsDeleted((GpMetafile*)graphics->image);
        if (stat != Ok)
            return stat;
    }

    if (graphics->temp_hdc)
    {
        if (graphics->owndc)
            ReleaseDC(graphics->hwnd, graphics->temp_hdc);
        else
            DeleteDC(graphics->temp_hdc);
        graphics->temp_hdc = NULL;
    }

    LIST_FOR_EACH_ENTRY_SAFE(cont, next, &graphics->containers, GraphicsContainerItem, entry){
        list_remove(&cont->entry);
        delete_container(cont);
    }

    GdipDeleteRegion(graphics->clip);

    DeleteObject(graphics->gdi_clip);

    /* Native returns ObjectBusy on the second free, instead of crashing as we'd
     * do otherwise, but we can't have that in the test suite because it means
     * accessing freed memory. */
    graphics->busy = TRUE;

    free(graphics);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawArc(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle)
{
    GpStatus status;
    GpPath *path;
    GpRectF rect;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y,
          width, height, startAngle, sweepAngle);

    if(!graphics || !pen || width <= 0 || height <= 0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        set_rect(&rect, x, y, width, height);
        return METAFILE_DrawArc((GpMetafile *)graphics->image, pen, &rect, startAngle, sweepAngle);
    }

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathArc(path, x, y, width, height, startAngle, sweepAngle);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawArcI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height, REAL startAngle, REAL sweepAngle)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %.2f, %.2f)\n", graphics, pen, x, y,
          width, height, startAngle, sweepAngle);

    return GdipDrawArc(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height,startAngle,sweepAngle);
}

GpStatus WINGDIPAPI GdipDrawBezier(GpGraphics *graphics, GpPen *pen, REAL x1,
    REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
    GpPointF pt[4];

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x1, y1,
          x2, y2, x3, y3, x4, y4);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pt[0].X = x1;
    pt[0].Y = y1;
    pt[1].X = x2;
    pt[1].Y = y2;
    pt[2].X = x3;
    pt[2].Y = y3;
    pt[3].X = x4;
    pt[3].Y = y4;
    return GdipDrawBeziers(graphics, pen, pt, 4);
}

GpStatus WINGDIPAPI GdipDrawBezierI(GpGraphics *graphics, GpPen *pen, INT x1,
    INT y1, INT x2, INT y2, INT x3, INT y3, INT x4, INT y4)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %d, %d, %d, %d)\n", graphics, pen, x1, y1,
          x2, y2, x3, y3, x4, y4);

    return GdipDrawBezier(graphics, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2, (REAL)x3, (REAL)y3, (REAL)x4, (REAL)y4);
}

GpStatus WINGDIPAPI GdipDrawBeziers(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count)
{
    GpStatus status;
    GpPath *path;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!graphics || !pen || !points || (count <= 0))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathBeziers(path, points, count);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawBeziersI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count)
{
    GpPointF *pts;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!graphics || !pen || !points || (count <= 0))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pts = malloc(sizeof(GpPointF) * count);
    if(!pts)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pts[i].X = (REAL)points[i].X;
        pts[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawBeziers(graphics,pen,pts,count);

    free(pts);

    return ret;
}

GpStatus WINGDIPAPI GdipDrawClosedCurve(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    return GdipDrawClosedCurve2(graphics, pen, points, count, 1.0);
}

GpStatus WINGDIPAPI GdipDrawClosedCurveI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    return GdipDrawClosedCurve2I(graphics, pen, points, count, 1.0);
}

GpStatus WINGDIPAPI GdipDrawClosedCurve2(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count, REAL tension)
{
    GpPath *path;
    GpStatus status;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!graphics || !pen || !points || count <= 0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathClosedCurve2(path, points, count, tension);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);

    return status;
}

GpStatus WINGDIPAPI GdipDrawClosedCurve2I(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count, REAL tension)
{
    GpPointF *ptf;
    GpStatus stat;
    INT i;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!points || count <= 0)
        return InvalidParameter;

    ptf = malloc(sizeof(GpPointF) * count);
    if(!ptf)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        ptf[i].X = (REAL)points[i].X;
        ptf[i].Y = (REAL)points[i].Y;
    }

    stat = GdipDrawClosedCurve2(graphics, pen, ptf, count, tension);

    free(ptf);

    return stat;
}

GpStatus WINGDIPAPI GdipDrawCurve(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    return GdipDrawCurve2(graphics,pen,points,count,1.0);
}

GpStatus WINGDIPAPI GdipDrawCurveI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count)
{
    GpPointF *pointsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!points)
        return InvalidParameter;

    pointsF = malloc(sizeof(GpPointF) * count);
    if(!pointsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawCurve(graphics,pen,pointsF,count);
    free(pointsF);

    return ret;
}

/* Approximates cardinal spline with Bezier curves. */
GpStatus WINGDIPAPI GdipDrawCurve2(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count, REAL tension)
{
    GpPath *path;
    GpStatus status;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(count < 2)
        return InvalidParameter;

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathCurve2(path, points, count, tension);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawCurve2I(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count, REAL tension)
{
    GpPointF *pointsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!points)
        return InvalidParameter;

    pointsF = malloc(sizeof(GpPointF) * count);
    if(!pointsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawCurve2(graphics,pen,pointsF,count,tension);
    free(pointsF);

    return ret;
}

GpStatus WINGDIPAPI GdipDrawCurve3(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count, INT offset, INT numberOfSegments,
    REAL tension)
{
    TRACE("(%p, %p, %p, %d, %d, %d, %.2f)\n", graphics, pen, points, count, offset, numberOfSegments, tension);

    if(offset >= count || numberOfSegments > count - offset - 1 || numberOfSegments <= 0){
        return InvalidParameter;
    }

    return GdipDrawCurve2(graphics, pen, points + offset, numberOfSegments + 1, tension);
}

GpStatus WINGDIPAPI GdipDrawCurve3I(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count, INT offset, INT numberOfSegments,
    REAL tension)
{
    TRACE("(%p, %p, %p, %d, %d, %d, %.2f)\n", graphics, pen, points, count, offset, numberOfSegments, tension);

    if(count < 0){
        return OutOfMemory;
    }

    if(offset >= count || numberOfSegments > count - offset - 1 || numberOfSegments <= 0){
        return InvalidParameter;
    }

    return GdipDrawCurve2I(graphics, pen, points + offset, numberOfSegments + 1, tension);
}

GpStatus WINGDIPAPI GdipDrawEllipse(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height)
{
    GpPath *path;
    GpStatus status;
    GpRectF rect;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y, width, height);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        set_rect(&rect, x, y, width, height);
        return METAFILE_DrawEllipse((GpMetafile *)graphics->image, pen, &rect);
    }

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathEllipse(path, x, y, width, height);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawEllipseI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, pen, x, y, width, height);

    return GdipDrawEllipse(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height);
}


GpStatus WINGDIPAPI GdipDrawImage(GpGraphics *graphics, GpImage *image, REAL x, REAL y)
{
    UINT width, height;

    TRACE("(%p, %p, %.2f, %.2f)\n", graphics, image, x, y);

    if(!graphics || !image)
        return InvalidParameter;

    GdipGetImageWidth(image, &width);
    GdipGetImageHeight(image, &height);

    return GdipDrawImagePointRect(graphics, image, x, y,
                                  0.0, 0.0, (REAL)width, (REAL)height, UnitPixel);
}

GpStatus WINGDIPAPI GdipDrawImageFX(GpGraphics *graphics, GpImage *image, GpRectF *src_rect,
    GpMatrix *transform, CGpEffect *effect, GpImageAttributes *imageattr,
    GpUnit src_unit)
{
    GpRectF src_rect_buf;
    GpPointF points[3];
    GpStatus status;

    TRACE("(%p, %p, %p, %p, %p, %p, %d)\n", graphics, image, src_rect, transform, effect, imageattr, src_unit);

    if (!graphics || !image)
        return InvalidParameter;

    if (effect)
        FIXME("effect not implemented\n");

    if (!src_rect)
    {
        if ((status = GdipGetImageBounds(image, &src_rect_buf, &src_unit)) != Ok)
            return status;

        /* Metafiles may have different left-top coordinates */
        if (src_rect_buf.X != 0.0 || src_rect_buf.Y != 0.0)
        {
            FIXME("image bounds %s left-top not at origin", debugstr_rectf(&src_rect_buf));
            /* TODO: only use width and height (force origin)? */
        }

        src_rect = &src_rect_buf;
    }

    points[0].X = points[2].X = src_rect->X;
    points[0].Y = points[1].Y = src_rect->Y;
    points[1].X = src_rect->X + src_rect->Width;
    points[2].Y = src_rect->Y + src_rect->Height;

    if (transform)
        GdipTransformMatrixPoints(transform, points, 3);

    return GdipDrawImagePointsRect(graphics, image, points, 3,
                                   src_rect->X, src_rect->Y,
                                   src_rect->Width, src_rect->Height,
                                   src_unit, imageattr, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImageI(GpGraphics *graphics, GpImage *image, INT x,
    INT y)
{
    TRACE("(%p, %p, %d, %d)\n", graphics, image, x, y);

    return GdipDrawImage(graphics, image, (REAL)x, (REAL)y);
}

GpStatus WINGDIPAPI GdipDrawImagePointRect(GpGraphics *graphics, GpImage *image,
    REAL x, REAL y, REAL srcx, REAL srcy, REAL srcwidth, REAL srcheight,
    GpUnit srcUnit)
{
    GpPointF points[3];
    REAL scale_x, scale_y, width, height;

    TRACE("(%p, %p, %f, %f, %f, %f, %f, %f, %d)\n", graphics, image, x, y, srcx, srcy, srcwidth, srcheight, srcUnit);

    if (!graphics || !image) return InvalidParameter;

    scale_x = units_scale(srcUnit, graphics->unit, graphics->xres, graphics->printer_display);
    scale_x *= graphics->xres / image->xres;
    scale_y = units_scale(srcUnit, graphics->unit, graphics->yres, graphics->printer_display);
    scale_y *= graphics->yres / image->yres;
    width = srcwidth * scale_x;
    height = srcheight * scale_y;

    points[0].X = points[2].X = x;
    points[0].Y = points[1].Y = y;
    points[1].X = x + width;
    points[2].Y = y + height;

    return GdipDrawImagePointsRect(graphics, image, points, 3, srcx, srcy,
        srcwidth, srcheight, srcUnit, NULL, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImagePointRectI(GpGraphics *graphics, GpImage *image,
    INT x, INT y, INT srcx, INT srcy, INT srcwidth, INT srcheight,
    GpUnit srcUnit)
{
    return GdipDrawImagePointRect(graphics, image, x, y, srcx, srcy, srcwidth, srcheight, srcUnit);
}

GpStatus WINGDIPAPI GdipDrawImagePoints(GpGraphics *graphics, GpImage *image,
    GDIPCONST GpPointF *dstpoints, INT count)
{
    UINT width, height;

    TRACE("(%p, %p, %p, %d)\n", graphics, image, dstpoints, count);

    if(!image)
        return InvalidParameter;

    GdipGetImageWidth(image, &width);
    GdipGetImageHeight(image, &height);

    return GdipDrawImagePointsRect(graphics, image, dstpoints, count, 0, 0,
        width, height, UnitPixel, NULL, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImagePointsI(GpGraphics *graphics, GpImage *image,
    GDIPCONST GpPoint *dstpoints, INT count)
{
    GpPointF ptf[3];

    TRACE("(%p, %p, %p, %d)\n", graphics, image, dstpoints, count);

    if (count != 3 || !dstpoints)
        return InvalidParameter;

    ptf[0].X = (REAL)dstpoints[0].X;
    ptf[0].Y = (REAL)dstpoints[0].Y;
    ptf[1].X = (REAL)dstpoints[1].X;
    ptf[1].Y = (REAL)dstpoints[1].Y;
    ptf[2].X = (REAL)dstpoints[2].X;
    ptf[2].Y = (REAL)dstpoints[2].Y;

    return GdipDrawImagePoints(graphics, image, ptf, count);
}

static BOOL CALLBACK play_metafile_proc(EmfPlusRecordType record_type, unsigned int flags,
    unsigned int dataSize, const unsigned char *pStr, void *userdata)
{
    GdipPlayMetafileRecord(userdata, record_type, flags, dataSize, pStr);
    return TRUE;
}

GpStatus WINGDIPAPI GdipDrawImagePointsRect(GpGraphics *graphics, GpImage *image,
     GDIPCONST GpPointF *points, INT count, REAL srcx, REAL srcy, REAL srcwidth,
     REAL srcheight, GpUnit srcUnit, GDIPCONST GpImageAttributes* imageAttributes,
     DrawImageAbort callback, VOID * callbackData)
{
    GpPointF ptf[4];
    POINT pti[4];
    GpStatus stat;

    TRACE("(%p, %p, %p, %d, %f, %f, %f, %f, %d, %p, %p, %p)\n", graphics, image, points,
          count, srcx, srcy, srcwidth, srcheight, srcUnit, imageAttributes, callback,
          callbackData);

    if (count == 4)
        return NotImplemented;

    if(!graphics || !image || !points || count != 3)
        return InvalidParameter;

    TRACE("%s %s %s\n", debugstr_pointf(&points[0]), debugstr_pointf(&points[1]),
        debugstr_pointf(&points[2]));

    if (is_metafile_graphics(graphics))
    {
        return METAFILE_DrawImagePointsRect((GpMetafile*)graphics->image,
                image, points, count, srcx, srcy, srcwidth, srcheight,
                srcUnit, imageAttributes, callback, callbackData);
    }

    memcpy(ptf, points, 3 * sizeof(GpPointF));

    /* Ensure source width/height is positive */
    if (srcwidth < 0)
    {
        GpPointF tmp = ptf[1];
        srcx = srcx + srcwidth;
        srcwidth = -srcwidth;
        ptf[2].X = ptf[2].X + ptf[1].X - ptf[0].X;
        ptf[2].Y = ptf[2].Y + ptf[1].Y - ptf[0].Y;
        ptf[1] = ptf[0];
        ptf[0] = tmp;
    }

    if (srcheight < 0)
    {
        GpPointF tmp = ptf[2];
        srcy = srcy + srcheight;
        srcheight = -srcheight;
        ptf[1].X = ptf[1].X + ptf[2].X - ptf[0].X;
        ptf[1].Y = ptf[1].Y + ptf[2].Y - ptf[0].Y;
        ptf[2] = ptf[0];
        ptf[0] = tmp;
    }

    ptf[3].X = ptf[2].X + ptf[1].X - ptf[0].X;
    ptf[3].Y = ptf[2].Y + ptf[1].Y - ptf[0].Y;
    if (!srcwidth || !srcheight || (ptf[3].X == ptf[0].X && ptf[3].Y == ptf[0].Y))
        return Ok;
    gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, ptf, 4);
    round_points(pti, ptf, 4);

    TRACE("%s %s %s %s\n", wine_dbgstr_point(&pti[0]), wine_dbgstr_point(&pti[1]),
        wine_dbgstr_point(&pti[2]), wine_dbgstr_point(&pti[3]));

    srcx = units_to_pixels(srcx, srcUnit, image->xres, graphics->printer_display);
    srcy = units_to_pixels(srcy, srcUnit, image->yres, graphics->printer_display);
    srcwidth = units_to_pixels(srcwidth, srcUnit, image->xres, graphics->printer_display);
    srcheight = units_to_pixels(srcheight, srcUnit, image->yres, graphics->printer_display);
    TRACE("src pixels: %f,%f %fx%f\n", srcx, srcy, srcwidth, srcheight);

    if (image->type == ImageTypeBitmap)
    {
        GpBitmap* bitmap = (GpBitmap*)image;
        BOOL do_resampling = FALSE;
        BOOL use_software = FALSE;

        TRACE("graphics: %.2fx%.2f dpi, fmt %#x, scale %f, image: %.2fx%.2f dpi, fmt %#x, color %08lx\n",
            graphics->xres, graphics->yres,
            graphics->image && graphics->image->type == ImageTypeBitmap ? ((GpBitmap *)graphics->image)->format : 0,
            graphics->scale, image->xres, image->yres, bitmap->format,
            imageAttributes ? imageAttributes->outside_color : 0);

        if (ptf[1].Y != ptf[0].Y || ptf[2].X != ptf[0].X ||
            ptf[1].X - ptf[0].X != srcwidth || ptf[2].Y - ptf[0].Y != srcheight ||
            srcx < 0 || srcy < 0 ||
            srcx + srcwidth > bitmap->width || srcy + srcheight > bitmap->height)
            do_resampling = TRUE;

        if (imageAttributes || graphics->alpha_hdc || do_resampling ||
            (graphics->image && graphics->image->type == ImageTypeBitmap))
            use_software = TRUE;

        if (use_software)
        {
            RECT dst_area;
            GpRectF graphics_bounds;
            GpRect src_area;
            int i, x, y, src_stride, dst_stride;
            LPBYTE src_data, dst_data, dst_dyn_data=NULL;
            BitmapData lockeddata;
            InterpolationMode interpolation = graphics->interpolation;
            PixelOffsetMode offset_mode = graphics->pixeloffset;
            static const GpImageAttributes defaultImageAttributes = {WrapModeClamp, 0, FALSE};

            if (!imageAttributes)
                imageAttributes = &defaultImageAttributes;

            dst_area.left = dst_area.right = pti[0].x;
            dst_area.top = dst_area.bottom = pti[0].y;
            for (i=1; i<4; i++)
            {
                if (dst_area.left > pti[i].x) dst_area.left = pti[i].x;
                if (dst_area.right < pti[i].x) dst_area.right = pti[i].x;
                if (dst_area.top > pti[i].y) dst_area.top = pti[i].y;
                if (dst_area.bottom < pti[i].y) dst_area.bottom = pti[i].y;
            }

            stat = get_graphics_device_bounds(graphics, &graphics_bounds);
            if (stat != Ok) return stat;

            if (graphics_bounds.X > dst_area.left) dst_area.left = floorf(graphics_bounds.X);
            if (graphics_bounds.Y > dst_area.top) dst_area.top = floorf(graphics_bounds.Y);
            if (graphics_bounds.X + graphics_bounds.Width < dst_area.right) dst_area.right = ceilf(graphics_bounds.X + graphics_bounds.Width);
            if (graphics_bounds.Y + graphics_bounds.Height < dst_area.bottom) dst_area.bottom = ceilf(graphics_bounds.Y + graphics_bounds.Height);

            TRACE("dst_area: %s\n", wine_dbgstr_rect(&dst_area));

            if (IsRectEmpty(&dst_area)) return Ok;

            if (do_resampling)
            {
                get_bitmap_sample_size(interpolation, imageAttributes->wrap,
                    bitmap, srcx, srcy, srcwidth, srcheight, &src_area);
            }
            else
            {
                /* Make sure src_area is equal in size to dst_area. */
                src_area.X = srcx + dst_area.left - pti[0].x;
                src_area.Y = srcy + dst_area.top - pti[0].y;
                src_area.Width = dst_area.right - dst_area.left;
                src_area.Height = dst_area.bottom - dst_area.top;
            }

            TRACE("src_area: %d x %d\n", src_area.Width, src_area.Height);

            src_data = calloc(src_area.Width * src_area.Height, sizeof(ARGB));
            if (!src_data)
                return OutOfMemory;
            src_stride = sizeof(ARGB) * src_area.Width;

            /* Read the bits we need from the source bitmap into a compatible buffer. */
            lockeddata.Width = src_area.Width;
            lockeddata.Height = src_area.Height;
            lockeddata.Stride = src_stride;
            lockeddata.Scan0 = src_data;
            if (!do_resampling && bitmap->format == PixelFormat32bppPARGB)
                lockeddata.PixelFormat = apply_image_attributes(imageAttributes, NULL, 0, 0, 0, ColorAdjustTypeBitmap, bitmap->format);
            else if (imageAttributes != &defaultImageAttributes)
                lockeddata.PixelFormat = PixelFormat32bppARGB;
            else
                lockeddata.PixelFormat = PixelFormat32bppPARGB;

            stat = GdipBitmapLockBits(bitmap, &src_area, ImageLockModeRead|ImageLockModeUserInputBuf,
                lockeddata.PixelFormat, &lockeddata);

            if (stat == Ok)
                stat = GdipBitmapUnlockBits(bitmap, &lockeddata);

            if (stat != Ok)
            {
                free(src_data);
                return stat;
            }

            apply_image_attributes(imageAttributes, src_data,
                src_area.Width, src_area.Height,
                src_stride, ColorAdjustTypeBitmap, lockeddata.PixelFormat);

            if (do_resampling)
            {
                GpMatrix dst_to_src;
                REAL m11, m12, m21, m22, mdx, mdy;
                REAL x_dx, x_dy, y_dx, y_dy;
                ARGB *dst_color;
                GpPointF src_pointf_row, src_pointf;

                m11 = (ptf[1].X - ptf[0].X) / srcwidth;
                m12 = (ptf[1].Y - ptf[0].Y) / srcwidth;
                m21 = (ptf[2].X - ptf[0].X) / srcheight;
                m22 = (ptf[2].Y - ptf[0].Y) / srcheight;
                mdx = ptf[0].X - m11 * srcx - m21 * srcy;
                mdy = ptf[0].Y - m12 * srcx - m22 * srcy;

                GdipSetMatrixElements(&dst_to_src, m11, m12, m21, m22, mdx, mdy);

                stat = GdipInvertMatrix(&dst_to_src);
                if (stat != Ok) return stat;

                dst_stride = sizeof(ARGB) * (dst_area.right - dst_area.left);
                x_dx = dst_to_src.matrix[0];
                x_dy = dst_to_src.matrix[1];
                y_dx = dst_to_src.matrix[2];
                y_dy = dst_to_src.matrix[3];

                /* Transform the bits as needed to the destination. */
                dst_data = dst_dyn_data = calloc((dst_area.right - dst_area.left) * (dst_area.bottom - dst_area.top), sizeof(ARGB));
                if (!dst_data)
                {
                    free(src_data);
                    return OutOfMemory;
                }
                dst_color = (ARGB*)(dst_data);

                /* Calculate top left point of transformed image.
                   It would be used as reference point for adding */
                src_pointf_row.X = dst_to_src.matrix[4] +
                                   dst_area.left * x_dx + dst_area.top * y_dx;
                src_pointf_row.Y = dst_to_src.matrix[5] +
                                   dst_area.left * x_dy + dst_area.top * y_dy;

                for (y = dst_area.top; y < dst_area.bottom;
                     y++, src_pointf_row.X += y_dx, src_pointf_row.Y += y_dy)
                {
                    for (x = dst_area.left, src_pointf = src_pointf_row; x < dst_area.right;
                         x++, src_pointf.X += x_dx, src_pointf.Y += x_dy)
                    {
                        if (src_pointf.X >= srcx && src_pointf.X < srcx + srcwidth &&
                            src_pointf.Y >= srcy && src_pointf.Y < srcy + srcheight)
                        {
                            if (lockeddata.PixelFormat != PixelFormat32bppPARGB)
                                *dst_color = resample_bitmap_pixel(&src_area, src_data, bitmap->width, bitmap->height, &src_pointf,
                                                                   imageAttributes, interpolation, offset_mode);
                            else
                                *dst_color = resample_bitmap_pixel_premult(&src_area, src_data, bitmap->width, bitmap->height, &src_pointf,
                                                                           imageAttributes, interpolation, offset_mode);
                        }
                        dst_color++;
                    }
                }
            }
            else
            {
                dst_data = src_data;
                dst_stride = src_stride;
            }

            gdi_transform_acquire(graphics);

            stat = alpha_blend_pixels(graphics, dst_area.left, dst_area.top,
                dst_data, dst_area.right - dst_area.left, dst_area.bottom - dst_area.top, dst_stride,
                lockeddata.PixelFormat);

            gdi_transform_release(graphics);

            free(src_data);

            free(dst_dyn_data);

            return stat;
        }
        else
        {
            HDC src_hdc, dst_hdc;
            HBITMAP hbitmap, old_hbm=NULL;
            HRGN hrgn;
            INT save_state;
            BITMAPINFOHEADER bih;
            BYTE *temp_bits;
            PixelFormat dst_format;
            INT dib_stride;

            src_hdc = CreateCompatibleDC(0);

            if (bitmap->format == PixelFormat16bppRGB555 ||
                bitmap->format == PixelFormat24bppRGB)
                dst_format = bitmap->format;
            else if (bitmap->image.flags & ImageFlagsHasAlpha)
                dst_format = PixelFormat32bppPARGB;
            else
                dst_format = PixelFormat32bppRGB;

            bih.biSize = sizeof(BITMAPINFOHEADER);
            bih.biWidth = bitmap->width;
            bih.biHeight = -bitmap->height;
            bih.biPlanes = 1;
            bih.biBitCount = PIXELFORMATBPP(dst_format);
            bih.biCompression = BI_RGB;
            bih.biSizeImage = 0;
            bih.biXPelsPerMeter = 0;
            bih.biYPelsPerMeter = 0;
            bih.biClrUsed = 0;
            bih.biClrImportant = 0;

            hbitmap = CreateDIBSection(src_hdc, (BITMAPINFO*)&bih, DIB_RGB_COLORS,
                (void**)&temp_bits, NULL, 0);

            dib_stride = ((bitmap->width * PIXELFORMATBPP(dst_format) + 31) / 8) & ~3;

            convert_pixels(bitmap->width, bitmap->height,
                dib_stride, temp_bits, dst_format, bitmap->image.palette,
                bitmap->stride, bitmap->bits, bitmap->format,
                bitmap->image.palette);

            old_hbm = SelectObject(src_hdc, hbitmap);

            gdi_dc_acquire(graphics, &dst_hdc);

            save_state = SaveDC(dst_hdc);

            stat = get_clip_hrgn(graphics, &hrgn);

            if (stat == Ok)
            {
                ExtSelectClipRgn(dst_hdc, hrgn, RGN_COPY);
                DeleteObject(hrgn);
            }

            gdi_transform_acquire(graphics);

            if (bitmap->image.flags & ImageFlagsHasAlpha)
            {
                gdi_alpha_blend(graphics, pti[0].x, pti[0].y, pti[1].x - pti[0].x, pti[2].y - pti[0].y,
                                src_hdc, srcx, srcy, srcwidth, srcheight);
            }
            else
            {
                StretchBlt(dst_hdc, pti[0].x, pti[0].y, pti[1].x-pti[0].x, pti[2].y-pti[0].y,
                    src_hdc, srcx, srcy, srcwidth, srcheight, SRCCOPY);
            }

            gdi_transform_release(graphics);

            RestoreDC(dst_hdc, save_state);

            gdi_dc_release(graphics, dst_hdc);

            SelectObject(src_hdc, old_hbm);
            DeleteDC(src_hdc);
            DeleteObject(hbitmap);
        }
    }
    else if (image->type == ImageTypeMetafile && ((GpMetafile*)image)->hemf)
    {
        GpRectF rc;

        set_rect(&rc, srcx, srcy, srcwidth, srcheight);
        return GdipEnumerateMetafileSrcRectDestPoints(graphics, (GpMetafile*)image,
            points, count, &rc, srcUnit, play_metafile_proc, image, imageAttributes);
    }
    else
    {
        WARN("GpImage with nothing we can draw (metafile in wrong state?)\n");
        return InvalidParameter;
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawImagePointsRectI(GpGraphics *graphics, GpImage *image,
     GDIPCONST GpPoint *points, INT count, INT srcx, INT srcy, INT srcwidth,
     INT srcheight, GpUnit srcUnit, GDIPCONST GpImageAttributes* imageAttributes,
     DrawImageAbort callback, VOID * callbackData)
{
    GpPointF pointsF[3];
    INT i;

    TRACE("(%p, %p, %p, %d, %d, %d, %d, %d, %d, %p, %p, %p)\n", graphics, image, points, count,
          srcx, srcy, srcwidth, srcheight, srcUnit, imageAttributes, callback,
          callbackData);

    if (count == 4)
        return NotImplemented;

    if (!points || count != 3)
        return InvalidParameter;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    return GdipDrawImagePointsRect(graphics, image, pointsF, count, (REAL)srcx, (REAL)srcy,
                                   (REAL)srcwidth, (REAL)srcheight, srcUnit, imageAttributes,
                                   callback, callbackData);
}

GpStatus WINGDIPAPI GdipDrawImageRectRect(GpGraphics *graphics, GpImage *image,
    REAL dstx, REAL dsty, REAL dstwidth, REAL dstheight, REAL srcx, REAL srcy,
    REAL srcwidth, REAL srcheight, GpUnit srcUnit,
    GDIPCONST GpImageAttributes* imageattr, DrawImageAbort callback,
    VOID * callbackData)
{
    GpPointF points[3];

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %d, %p, %p, %p)\n",
          graphics, image, dstx, dsty, dstwidth, dstheight, srcx, srcy,
          srcwidth, srcheight, srcUnit, imageattr, callback, callbackData);

    points[0].X = dstx;
    points[0].Y = dsty;
    points[1].X = dstx + dstwidth;
    points[1].Y = dsty;
    points[2].X = dstx;
    points[2].Y = dsty + dstheight;

    return GdipDrawImagePointsRect(graphics, image, points, 3, srcx, srcy,
               srcwidth, srcheight, srcUnit, imageattr, callback, callbackData);
}

GpStatus WINGDIPAPI GdipDrawImageRectRectI(GpGraphics *graphics, GpImage *image,
	INT dstx, INT dsty, INT dstwidth, INT dstheight, INT srcx, INT srcy,
	INT srcwidth, INT srcheight, GpUnit srcUnit,
	GDIPCONST GpImageAttributes* imageAttributes, DrawImageAbort callback,
	VOID * callbackData)
{
    GpPointF points[3];

    TRACE("(%p, %p, %d, %d, %d, %d, %d, %d, %d, %d, %d, %p, %p, %p)\n",
          graphics, image, dstx, dsty, dstwidth, dstheight, srcx, srcy,
          srcwidth, srcheight, srcUnit, imageAttributes, callback, callbackData);

    points[0].X = dstx;
    points[0].Y = dsty;
    points[1].X = dstx + dstwidth;
    points[1].Y = dsty;
    points[2].X = dstx;
    points[2].Y = dsty + dstheight;

    return GdipDrawImagePointsRect(graphics, image, points, 3, srcx, srcy,
               srcwidth, srcheight, srcUnit, imageAttributes, callback, callbackData);
}

GpStatus WINGDIPAPI GdipDrawImageRect(GpGraphics *graphics, GpImage *image,
    REAL x, REAL y, REAL width, REAL height)
{
    RectF bounds;
    GpUnit unit;
    GpStatus ret;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, image, x, y, width, height);

    if(!graphics || !image)
        return InvalidParameter;

    ret = GdipGetImageBounds(image, &bounds, &unit);
    if(ret != Ok)
        return ret;

    return GdipDrawImageRectRect(graphics, image, x, y, width, height,
                                 bounds.X, bounds.Y, bounds.Width, bounds.Height,
                                 unit, NULL, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImageRectI(GpGraphics *graphics, GpImage *image,
    INT x, INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, image, x, y, width, height);

    return GdipDrawImageRect(graphics, image, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus WINGDIPAPI GdipDrawLine(GpGraphics *graphics, GpPen *pen, REAL x1,
    REAL y1, REAL x2, REAL y2)
{
    GpPointF pt[2];

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x1, y1, x2, y2);

    if (!pen)
        return InvalidParameter;

    if (pen->unit == UnitPixel && pen->width <= 0.0)
        return Ok;

    pt[0].X = x1;
    pt[0].Y = y1;
    pt[1].X = x2;
    pt[1].Y = y2;
    return GdipDrawLines(graphics, pen, pt, 2);
}

GpStatus WINGDIPAPI GdipDrawLineI(GpGraphics *graphics, GpPen *pen, INT x1,
    INT y1, INT x2, INT y2)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, pen, x1, y1, x2, y2);

    return GdipDrawLine(graphics, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
}

GpStatus WINGDIPAPI GdipDrawLines(GpGraphics *graphics, GpPen *pen, GDIPCONST
    GpPointF *points, INT count)
{
    GpStatus status;
    GpPath *path;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!pen || !graphics || (count < 2))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathLine2(path, points, count);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawLinesI(GpGraphics *graphics, GpPen *pen, GDIPCONST
    GpPoint *points, INT count)
{
    GpStatus retval;
    GpPointF *ptf;
    int i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    ptf = malloc(count * sizeof(GpPointF));
    if(!ptf) return OutOfMemory;

    for(i = 0; i < count; i ++){
        ptf[i].X = (REAL) points[i].X;
        ptf[i].Y = (REAL) points[i].Y;
    }

    retval = GdipDrawLines(graphics, pen, ptf, count);

    free(ptf);
    return retval;
}

static GpStatus GDI32_GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path)
{
    HDC hdc;
    INT save_state;
    GpStatus retval;
    HRGN hrgn=NULL;

    retval = gdi_dc_acquire(graphics, &hdc);
    if (retval != Ok)
        return retval;

    save_state = prepare_dc(graphics, hdc, pen);

    retval = get_clip_hrgn(graphics, &hrgn);

    if (retval != Ok)
        goto end;

    ExtSelectClipRgn(graphics->hdc, hrgn, RGN_COPY);

    gdi_transform_acquire(graphics);

    retval = draw_poly(graphics, pen, path->pathdata.Points,
                       path->pathdata.Types, path->pathdata.Count, TRUE);

    gdi_transform_release(graphics);

end:
    restore_dc(graphics, hdc, save_state);
    DeleteObject(hrgn);
    gdi_dc_release(graphics, hdc);

    return retval;
}

static GpStatus SOFTWARE_GdipDrawThinPath(GpGraphics *graphics, GpPen *pen, GpPath *path)
{
    GpStatus stat;
    GpPath *flat_path, *anchor_path;
    GpRegion *anchor_region;
    GpMatrix* transform;
    GpRectF gp_bound_rect;
    GpRect gp_output_area;
    RECT output_area;
    INT output_height, output_width;
    DWORD *output_bits, *brush_bits=NULL;
    int i;
    static const BYTE static_dash_pattern[] = {1,1,1,0,1,0,1,0};
    const BYTE *dash_pattern;
    INT dash_pattern_size;
    BYTE *dyn_dash_pattern = NULL;

    stat = GdipClonePath(path, &flat_path);

    if (stat != Ok)
        return stat;

    stat = GdipCreateMatrix(&transform);

    if (stat == Ok)
    {
        stat = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice,
                CoordinateSpaceWorld, transform);

        if (stat == Ok)
            stat = GdipFlattenPath(flat_path, transform, 1.0);

        if (stat == Ok)
            stat = widen_flat_path_anchors(flat_path, pen, 1.0, &anchor_path);

        GdipDeleteMatrix(transform);
    }

    /* estimate the output size in pixels, can be larger than necessary */
    if (stat == Ok)
    {
        output_area.left = floorf(flat_path->pathdata.Points[0].X);
        output_area.right = ceilf(flat_path->pathdata.Points[0].X);
        output_area.top = floorf(flat_path->pathdata.Points[0].Y);
        output_area.bottom = ceilf(flat_path->pathdata.Points[0].Y);

        for (i=1; i<flat_path->pathdata.Count; i++)
        {
            REAL x, y;
            x = flat_path->pathdata.Points[i].X;
            y = flat_path->pathdata.Points[i].Y;

            if (floorf(x) < output_area.left) output_area.left = floorf(x);
            if (floorf(y) < output_area.top) output_area.top = floorf(y);
            if (ceilf(x) > output_area.right) output_area.right = ceilf(x);
            if (ceilf(y) > output_area.bottom) output_area.bottom = ceilf(y);
        }

        for (i=0; i<anchor_path->pathdata.Count; i++)
        {
            REAL x, y;
            x = anchor_path->pathdata.Points[i].X;
            y = anchor_path->pathdata.Points[i].Y;

            if (floorf(x) < output_area.left) output_area.left = floorf(x);
            if (floorf(y) < output_area.top) output_area.top = floorf(y);
            if (ceilf(x) > output_area.right) output_area.right = ceilf(x);
            if (ceilf(y) > output_area.bottom) output_area.bottom = ceilf(y);
        }

        stat = get_graphics_device_bounds(graphics, &gp_bound_rect);
    }

    if (stat == Ok)
    {
        output_area.left = max(output_area.left, floorf(gp_bound_rect.X));
        output_area.top = max(output_area.top, floorf(gp_bound_rect.Y));
        output_area.right = min(output_area.right, ceilf(gp_bound_rect.X + gp_bound_rect.Width));
        output_area.bottom = min(output_area.bottom, ceilf(gp_bound_rect.Y + gp_bound_rect.Height));

        output_width = output_area.right - output_area.left + 1;
        output_height = output_area.bottom - output_area.top + 1;

        if (output_width <= 0 || output_height <= 0)
        {
            GdipDeletePath(flat_path);
            GdipDeletePath(anchor_path);
            return Ok;
        }

        gp_output_area.X = output_area.left;
        gp_output_area.Y = output_area.top;
        gp_output_area.Width = output_width;
        gp_output_area.Height = output_height;

        output_bits = calloc(output_width * output_height, sizeof(DWORD));
        if (!output_bits)
            stat = OutOfMemory;
    }

    if (stat == Ok)
    {
        if (pen->brush->bt != BrushTypeSolidColor)
        {
            /* allocate and draw brush output */
            brush_bits = calloc(output_width * output_height, sizeof(DWORD));

            if (brush_bits)
            {
                stat = brush_fill_pixels(graphics, pen->brush, brush_bits,
                    &gp_output_area, output_width);
            }
            else
                stat = OutOfMemory;
        }

        if (stat == Ok)
        {
            /* convert dash pattern to bool array */
            switch (pen->dash)
            {
            case DashStyleCustom:
            {
                dash_pattern_size = 0;

                for (i=0; i < pen->numdashes; i++)
                    dash_pattern_size += gdip_round(pen->dashes[i]);

                if (dash_pattern_size != 0)
                {
                    dash_pattern = dyn_dash_pattern = malloc(dash_pattern_size);

                    if (dyn_dash_pattern)
                    {
                        int j=0;
                        for (i=0; i < pen->numdashes; i++)
                        {
                            int k;
                            for (k=0; k < gdip_round(pen->dashes[i]); k++)
                                dyn_dash_pattern[j++] = (i&1)^1;
                        }
                    }
                    else
                        stat = OutOfMemory;

                    break;
                }
                /* else fall through */
            }
            case DashStyleSolid:
            default:
                dash_pattern = static_dash_pattern;
                dash_pattern_size = 1;
                break;
            case DashStyleDash:
                dash_pattern = static_dash_pattern;
                dash_pattern_size = 4;
                break;
            case DashStyleDot:
                dash_pattern = &static_dash_pattern[4];
                dash_pattern_size = 2;
                break;
            case DashStyleDashDot:
                dash_pattern = static_dash_pattern;
                dash_pattern_size = 6;
                break;
            case DashStyleDashDotDot:
                dash_pattern = static_dash_pattern;
                dash_pattern_size = 8;
                break;
            }
        }

        if (stat == Ok)
        {
            /* trace path */
            GpPointF subpath_start = flat_path->pathdata.Points[0];
            INT prev_x = INT_MAX, prev_y = INT_MAX;
            int dash_pos = dash_pattern_size - 1;

            for (i=0; i < flat_path->pathdata.Count; i++)
            {
                BYTE type, type2;
                GpPointF start_point, end_point;
                GpPoint start_pointi, end_pointi;

                type = flat_path->pathdata.Types[i];
                if (i+1 < flat_path->pathdata.Count)
                    type2 = flat_path->pathdata.Types[i+1];
                else
                    type2 = PathPointTypeStart;

                start_point = flat_path->pathdata.Points[i];

                if ((type & PathPointTypePathTypeMask) == PathPointTypeStart)
                    subpath_start = start_point;

                if ((type & PathPointTypeCloseSubpath) == PathPointTypeCloseSubpath)
                    end_point = subpath_start;
                else if ((type2 & PathPointTypePathTypeMask) == PathPointTypeStart)
                    continue;
                else
                    end_point = flat_path->pathdata.Points[i+1];

                start_pointi.X = floorf(start_point.X);
                start_pointi.Y = floorf(start_point.Y);
                end_pointi.X = floorf(end_point.X);
                end_pointi.Y = floorf(end_point.Y);

                if(start_pointi.X == end_pointi.X && start_pointi.Y == end_pointi.Y)
                    continue;

                /* draw line segment */
                if (abs(start_pointi.Y - end_pointi.Y) > abs(start_pointi.X - end_pointi.X))
                {
                    INT x, y, start_y, end_y, step;

                    if (start_pointi.Y < end_pointi.Y)
                    {
                        step = 1;
                        start_y = ceilf(start_point.Y) - output_area.top;
                        end_y = end_pointi.Y - output_area.top;
                    }
                    else
                    {
                        step = -1;
                        start_y = start_point.Y - output_area.top;
                        end_y = ceilf(end_point.Y) - output_area.top;
                    }

                    for (y=start_y; y != (end_y+step); y+=step)
                    {
                        x = gdip_round( start_point.X +
                            (end_point.X - start_point.X) * (y + output_area.top - start_point.Y) / (end_point.Y - start_point.Y) )
                            - output_area.left;

                        if (x == prev_x && y == prev_y)
                            continue;

                        prev_x = x;
                        prev_y = y;
                        dash_pos = (dash_pos + 1 == dash_pattern_size) ? 0 : dash_pos + 1;

                        if (!dash_pattern[dash_pos])
                            continue;

                        if (x < 0 || x >= output_width || y < 0 || y >= output_height)
                            continue;

                        if (brush_bits)
                            output_bits[x + y*output_width] = brush_bits[x + y*output_width];
                        else
                            output_bits[x + y*output_width] = ((GpSolidFill*)pen->brush)->color;
                    }
                }
                else
                {
                    INT x, y, start_x, end_x, step;

                    if (start_pointi.X < end_pointi.X)
                    {
                        step = 1;
                        start_x = ceilf(start_point.X) - output_area.left;
                        end_x = end_pointi.X - output_area.left;
                    }
                    else
                    {
                        step = -1;
                        start_x = start_point.X - output_area.left;
                        end_x = ceilf(end_point.X) - output_area.left;
                    }

                    for (x=start_x; x != (end_x+step); x+=step)
                    {
                        y = gdip_round( start_point.Y +
                            (end_point.Y - start_point.Y) * (x + output_area.left - start_point.X) / (end_point.X - start_point.X) )
                            - output_area.top;

                        if (x == prev_x && y == prev_y)
                            continue;

                        prev_x = x;
                        prev_y = y;
                        dash_pos = (dash_pos + 1 == dash_pattern_size) ? 0 : dash_pos + 1;

                        if (!dash_pattern[dash_pos])
                            continue;

                        if (x < 0 || x >= output_width || y < 0 || y >= output_height)
                            continue;

                        if (brush_bits)
                            output_bits[x + y*output_width] = brush_bits[x + y*output_width];
                        else
                            output_bits[x + y*output_width] = ((GpSolidFill*)pen->brush)->color;
                    }
                }
            }

            /* draw anchors */
            stat = GdipCreateRegionPath(anchor_path, &anchor_region);
            if (stat == Ok)
            {
                HRGN hrgn;
                DWORD rgn_data_size;
                RGNDATA *rgn_data;
                RECT *rects;
                INT x, y;

                stat = GdipCombineRegionRectI(anchor_region, &gp_output_area, CombineModeIntersect);

                if (stat == Ok)
                    stat = GdipGetRegionHRgn(anchor_region, NULL, &hrgn);

                if (stat == Ok)
                {
                    rgn_data_size = GetRegionData(hrgn, 0, NULL);

                    if (rgn_data_size)
                    {
                        rgn_data = malloc(rgn_data_size);

                        if (rgn_data)
                        {
                            GetRegionData(hrgn, rgn_data_size, rgn_data);

                            rects = (RECT*)&rgn_data->Buffer;

                            for (i=0; i < rgn_data->rdh.nCount; i++)
                            {
                                RECT rc;
                                rc = rects[i];

                                OffsetRect(&rc, -output_area.left, -output_area.top);

                                for (y = rc.top; y < rc.bottom; y++)
                                {
                                    for (x = rc.left; x < rc.right; x++)
                                    {
                                        if (brush_bits)
                                            output_bits[x + y*output_width] = brush_bits[x + y*output_width];
                                        else
                                            output_bits[x + y*output_width] = ((GpSolidFill*)pen->brush)->color;
                                    }
                                }
                            }

                            free(rgn_data);
                        }
                        else
                            stat = OutOfMemory;
                    }

                    DeleteObject(hrgn);
                }

                GdipDeleteRegion(anchor_region);
            }
        }

        /* draw output image */
        if (stat == Ok)
        {
            gdi_transform_acquire(graphics);

            stat = alpha_blend_pixels(graphics, output_area.left, output_area.top,
                (BYTE*)output_bits, output_width, output_height, output_width * 4,
                PixelFormat32bppARGB);

            gdi_transform_release(graphics);
        }

        free(brush_bits);
        free(dyn_dash_pattern);
        free(output_bits);
    }

    GdipDeletePath(flat_path);
    GdipDeletePath(anchor_path);

    return stat;
}

static GpStatus SOFTWARE_GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path)
{
    GpStatus stat;
    GpPath *wide_path;
    GpMatrix *transform=NULL;
    REAL flatness=1.0;

    /* Check if the final pen thickness in pixels is too thin. */
    if (pen->unit == UnitPixel)
    {
        if (pen->width < 1.415)
            return SOFTWARE_GdipDrawThinPath(graphics, pen, path);
    }
    else
    {
        GpPointF points[3] = {{0,0}, {1,0}, {0,1}};

        points[1].X = pen->width;
        points[2].Y = pen->width;

        stat = gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice,
            CoordinateSpaceWorld, points, 3);

        if (stat != Ok)
            return stat;

        if (((points[1].X-points[0].X)*(points[1].X-points[0].X) +
             (points[1].Y-points[0].Y)*(points[1].Y-points[0].Y) < 2.0001) &&
            ((points[2].X-points[0].X)*(points[2].X-points[0].X) +
             (points[2].Y-points[0].Y)*(points[2].Y-points[0].Y) < 2.0001))
            return SOFTWARE_GdipDrawThinPath(graphics, pen, path);
    }

    stat = GdipClonePath(path, &wide_path);

    if (stat != Ok)
        return stat;

    if (pen->unit == UnitPixel)
    {
        /* We have to transform this to device coordinates to get the widths right. */
        stat = GdipCreateMatrix(&transform);

        if (stat == Ok)
            stat = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice,
                CoordinateSpaceWorld, transform);
    }
    else
    {
        /* Set flatness based on the final coordinate space */
        GpMatrix t;

        stat = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice,
            CoordinateSpaceWorld, &t);

        if (stat != Ok)
            return stat;

        flatness = 1.0/sqrt(fmax(
            t.matrix[0] * t.matrix[0] + t.matrix[1] * t.matrix[1],
            t.matrix[2] * t.matrix[2] + t.matrix[3] * t.matrix[3]));
    }

    if (stat == Ok)
        stat = GdipWidenPath(wide_path, pen, transform, flatness);

    if (pen->unit == UnitPixel)
    {
        /* Transform the path back to world coordinates */
        if (stat == Ok)
            stat = GdipInvertMatrix(transform);

        if (stat == Ok)
            stat = GdipTransformPath(wide_path, transform);
    }

    /* Actually draw the path */
    if (stat == Ok)
        stat = GdipFillPath(graphics, pen->brush, wide_path);

    GdipDeleteMatrix(transform);

    GdipDeletePath(wide_path);

    return stat;
}

GpStatus WINGDIPAPI GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path)
{
    GpStatus retval;

    TRACE("(%p, %p, %p)\n", graphics, pen, path);

    if(!pen || !graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (path->pathdata.Count == 0)
        return Ok;

    if (is_metafile_graphics(graphics))
        retval = METAFILE_DrawPath((GpMetafile*)graphics->image, pen, path);
    else if (!has_gdi_dc(graphics) || graphics->alpha_hdc || !brush_can_fill_path(pen->brush, FALSE))
        retval = SOFTWARE_GdipDrawPath(graphics, pen, path);
    else
        retval = GDI32_GdipDrawPath(graphics, pen, path);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawPie(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle)
{
    GpStatus status;
    GpPath *path;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y,
            width, height, startAngle, sweepAngle);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathPie(path, x, y, width, height, startAngle, sweepAngle);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawPieI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height, REAL startAngle, REAL sweepAngle)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %.2f, %.2f)\n", graphics, pen, x, y,
            width, height, startAngle, sweepAngle);

    return GdipDrawPie(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height,startAngle,sweepAngle);
}

GpStatus WINGDIPAPI GdipDrawRectangle(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height)
{
    GpRectF rect;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y, width, height);

    set_rect(&rect, x, y, width, height);
    return GdipDrawRectangles(graphics, pen, &rect, 1);
}

GpStatus WINGDIPAPI GdipDrawRectangleI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, pen, x, y, width, height);

    return GdipDrawRectangle(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height);
}

GpStatus WINGDIPAPI GdipDrawRectangles(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpRectF* rects, INT count)
{
    GpStatus status;
    GpPath *path;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, rects, count);

    if(!graphics || !pen || !rects || count < 1)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
        return METAFILE_DrawRectangles((GpMetafile *)graphics->image, pen, rects, count);

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathRectangles(path, rects, count);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipDrawRectanglesI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpRect* rects, INT count)
{
    GpRectF *rectsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, rects, count);

    if(!rects || count<=0)
        return InvalidParameter;

    rectsF = malloc(sizeof(GpRectF) * count);
    if(!rectsF)
        return OutOfMemory;

    for(i = 0;i < count;i++)
        set_rect(&rectsF[i], rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);

    ret = GdipDrawRectangles(graphics, pen, rectsF, count);
    free(rectsF);

    return ret;
}

GpStatus WINGDIPAPI GdipFillClosedCurve2(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count, REAL tension, GpFillMode fill)
{
    GpPath *path;
    GpStatus status;

    TRACE("(%p, %p, %p, %d, %.2f, %d)\n", graphics, brush, points,
            count, tension, fill);

    if(!graphics || !brush || !points)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(count == 1)    /* Do nothing */
        return Ok;

    status = GdipCreatePath(fill, &path);
    if (status != Ok) return status;

    status = GdipAddPathClosedCurve2(path, points, count, tension);
    if (status == Ok)
        status = GdipFillPath(graphics, brush, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipFillClosedCurve2I(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count, REAL tension, GpFillMode fill)
{
    GpPointF *ptf;
    GpStatus stat;
    INT i;

    TRACE("(%p, %p, %p, %d, %.2f, %d)\n", graphics, brush, points,
            count, tension, fill);

    if(!points || count == 0)
        return InvalidParameter;

    if(count == 1)    /* Do nothing */
        return Ok;

    ptf = malloc(sizeof(GpPointF) * count);
    if(!ptf)
        return OutOfMemory;

    for(i = 0;i < count;i++){
        ptf[i].X = (REAL)points[i].X;
        ptf[i].Y = (REAL)points[i].Y;
    }

    stat = GdipFillClosedCurve2(graphics, brush, ptf, count, tension, fill);

    free(ptf);

    return stat;
}

GpStatus WINGDIPAPI GdipFillClosedCurve(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);
    return GdipFillClosedCurve2(graphics, brush, points, count,
               0.5f, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillClosedCurveI(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);
    return GdipFillClosedCurve2I(graphics, brush, points, count,
               0.5f, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillEllipse(GpGraphics *graphics, GpBrush *brush, REAL x,
    REAL y, REAL width, REAL height)
{
    GpStatus stat;
    GpPath *path;
    GpRectF rect;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, brush, x, y, width, height);

    if(!graphics || !brush)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        set_rect(&rect, x, y, width, height);
        return METAFILE_FillEllipse((GpMetafile *)graphics->image, brush, &rect);
    }

    stat = GdipCreatePath(FillModeAlternate, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathEllipse(path, x, y, width, height);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillEllipseI(GpGraphics *graphics, GpBrush *brush, INT x,
    INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, brush, x, y, width, height);

    return GdipFillEllipse(graphics,brush,(REAL)x,(REAL)y,(REAL)width,(REAL)height);
}

static GpStatus GDI32_GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
    HDC hdc;
    INT save_state;
    GpStatus retval;
    HRGN hrgn=NULL;

    if(!brush_can_fill_path(brush, TRUE))
        return NotImplemented;

    retval = gdi_dc_acquire(graphics, &hdc);
    if (retval != Ok)
        return retval;

    save_state = SaveDC(hdc);
    EndPath(hdc);
    SetPolyFillMode(hdc, (path->fill == FillModeAlternate ? ALTERNATE : WINDING));

    retval = get_clip_hrgn(graphics, &hrgn);

    if (retval != Ok)
        goto end;

    ExtSelectClipRgn(hdc, hrgn, RGN_COPY);

    gdi_transform_acquire(graphics);

    BeginPath(hdc);
    retval = draw_poly(graphics, NULL, path->pathdata.Points,
                       path->pathdata.Types, path->pathdata.Count, FALSE);

    if(retval == Ok)
    {
        EndPath(hdc);
        retval = brush_fill_path(graphics, brush);
    }

    gdi_transform_release(graphics);

end:
    RestoreDC(hdc, save_state);
    DeleteObject(hrgn);
    gdi_dc_release(graphics, hdc);

    return retval;
}

static GpStatus SOFTWARE_GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
    GpStatus stat;
    GpRegion *rgn;

    if (!brush_can_fill_pixels(brush))
        return NotImplemented;

    /* FIXME: This could probably be done more efficiently without regions. */

    stat = GdipCreateRegionPath(path, &rgn);

    if (stat == Ok)
    {
        stat = GdipFillRegion(graphics, brush, rgn);

        GdipDeleteRegion(rgn);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
    GpStatus stat = NotImplemented;

    TRACE("(%p, %p, %p)\n", graphics, brush, path);

    if(!brush || !graphics || !path)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!path->pathdata.Count)
        return Ok;

    if (is_metafile_graphics(graphics))
        return METAFILE_FillPath((GpMetafile*)graphics->image, brush, path);

    if (!graphics->image && !graphics->alpha_hdc)
        stat = GDI32_GdipFillPath(graphics, brush, path);

    if (stat == NotImplemented)
        stat = SOFTWARE_GdipFillPath(graphics, brush, path);

    if (stat == NotImplemented)
    {
        FIXME("Not implemented for brushtype %i\n", brush->bt);
        stat = Ok;
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPie(GpGraphics *graphics, GpBrush *brush, REAL x,
    REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle)
{
    GpStatus stat;
    GpPath *path;
    GpRectF rect;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n",
            graphics, brush, x, y, width, height, startAngle, sweepAngle);

    if(!graphics || !brush)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        set_rect(&rect, x, y, width, height);
        return METAFILE_FillPie((GpMetafile *)graphics->image, brush, &rect, startAngle, sweepAngle);
    }

    stat = GdipCreatePath(FillModeAlternate, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathPie(path, x, y, width, height, startAngle, sweepAngle);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPieI(GpGraphics *graphics, GpBrush *brush, INT x,
    INT y, INT width, INT height, REAL startAngle, REAL sweepAngle)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %.2f, %.2f)\n",
            graphics, brush, x, y, width, height, startAngle, sweepAngle);

    return GdipFillPie(graphics,brush,(REAL)x,(REAL)y,(REAL)width,(REAL)height,startAngle,sweepAngle);
}

GpStatus WINGDIPAPI GdipFillPolygon(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count, GpFillMode fillMode)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %p, %d, %d)\n", graphics, brush, points, count, fillMode);

    if(!graphics || !brush || !points || !count)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(fillMode, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathPolygon(path, points, count);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPolygonI(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count, GpFillMode fillMode)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %p, %d, %d)\n", graphics, brush, points, count, fillMode);

    if(!graphics || !brush || !points || !count)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(fillMode, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathPolygonI(path, points, count);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPolygon2(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);

    return GdipFillPolygon(graphics, brush, points, count, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillPolygon2I(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);

    return GdipFillPolygonI(graphics, brush, points, count, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillRectangle(GpGraphics *graphics, GpBrush *brush,
    REAL x, REAL y, REAL width, REAL height)
{
    GpRectF rect;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, brush, x, y, width, height);

    set_rect(&rect, x, y, width, height);
    return GdipFillRectangles(graphics, brush, &rect, 1);
}

GpStatus WINGDIPAPI GdipFillRectangleI(GpGraphics *graphics, GpBrush *brush,
    INT x, INT y, INT width, INT height)
{
    GpRectF rect;

    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, brush, x, y, width, height);

    set_rect(&rect, x, y, width, height);
    return GdipFillRectangles(graphics, brush, &rect, 1);
}

GpStatus WINGDIPAPI GdipFillRectangles(GpGraphics *graphics, GpBrush *brush, GDIPCONST GpRectF *rects,
    INT count)
{
    GpStatus status;
    GpPath *path;

    TRACE("(%p, %p, %p, %d)\n", graphics, brush, rects, count);

    if(!graphics || !brush || !rects || count <= 0)
        return InvalidParameter;

    if (is_metafile_graphics(graphics))
    {
        status = METAFILE_FillRectangles((GpMetafile*)graphics->image, brush, rects, count);
        /* FIXME: Add gdi32 drawing. */
        return status;
    }

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathRectangles(path, rects, count);
    if (status == Ok)
        status = GdipFillPath(graphics, brush, path);

    GdipDeletePath(path);
    return status;
}

GpStatus WINGDIPAPI GdipFillRectanglesI(GpGraphics *graphics, GpBrush *brush, GDIPCONST GpRect *rects,
    INT count)
{
    GpRectF *rectsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, brush, rects, count);

    if(!rects || count <= 0)
        return InvalidParameter;

    rectsF = malloc(sizeof(GpRectF) * count);
    if(!rectsF)
        return OutOfMemory;

    for(i = 0; i < count; i++)
        set_rect(&rectsF[i], rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);

    ret = GdipFillRectangles(graphics,brush,rectsF,count);
    free(rectsF);

    return ret;
}

static GpStatus get_clipped_region_hrgn(GpGraphics* graphics, GpRegion* region, HRGN *hrgn)
{
    GpStatus status;
    GpRegion *tmp_region, *device_region;
    GpMatrix gdip_transform, gdi_transform;
    BOOL gdip_identity, gdi_identity;
    GpRectF device_bounds;

    status = get_graphics_device_bounds(graphics, &device_bounds);

    if (status == Ok)
        status = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, &gdip_transform);

    if (status == Ok)
        status = GdipIsMatrixIdentity(&gdip_transform, &gdip_identity);

    if (status == Ok)
        status = get_graphics_transform(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceDevice, &gdi_transform);

    if (status == Ok)
        status = GdipIsMatrixIdentity(&gdi_transform, &gdi_identity);

    if (status == Ok)
        status = GdipCreateRegionRect(&device_bounds, &device_region);

    if (status == Ok)
    {
        if (gdip_identity)
            status = GdipCombineRegionRegion(device_region, region, CombineModeIntersect);
        else
        {
            status = GdipCloneRegion(region, &tmp_region);

            if (status == Ok)
            {
                status = GdipTransformRegion(tmp_region, &gdip_transform);

                if (status == Ok)
                    GdipCombineRegionRegion(device_region, tmp_region, CombineModeIntersect);

                GdipDeleteRegion(tmp_region);
            }
        }

        if (status == Ok)
        {
            if (gdi_identity)
                status = GdipCombineRegionRegion(device_region, graphics->clip, CombineModeIntersect);
            else
            {
                status = GdipCloneRegion(graphics->clip, &tmp_region);

                if (status == Ok)
                {
                    status = GdipTransformRegion(tmp_region, &gdi_transform);

                    if (status == Ok)
                        GdipCombineRegionRegion(device_region, tmp_region, CombineModeIntersect);

                    GdipDeleteRegion(tmp_region);
                }
            }
        }

        if (status == Ok)
            status = GdipGetRegionHRgn(device_region, NULL, hrgn);

        GdipDeleteRegion(device_region);
    }

    if (status == Ok && graphics->gdi_clip)
    {
        CombineRgn(*hrgn, *hrgn, graphics->gdi_clip, RGN_AND);
    }

    return status;
}

static GpStatus GDI32_GdipFillRegion(GpGraphics* graphics, GpBrush* brush,
    GpRegion* region)
{
    INT save_state;
    GpStatus status;
    HDC hdc;
    HRGN hrgn;
    RECT rc;

    if(!brush_can_fill_path(brush, TRUE))
        return NotImplemented;

    status = gdi_dc_acquire(graphics, &hdc);
    if (status != Ok)
        return status;

    save_state = SaveDC(hdc);
    EndPath(hdc);

    hrgn = NULL;
    status = get_clipped_region_hrgn(graphics, region, &hrgn);
    if (status != Ok)
        goto end;

    ExtSelectClipRgn(hdc, hrgn, RGN_COPY);
    DeleteObject(hrgn);

    if (GetClipBox(hdc, &rc) != NULLREGION)
    {
        BeginPath(hdc);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        EndPath(hdc);

        status = brush_fill_path(graphics, brush);
    }

end:
    RestoreDC(hdc, save_state);
    gdi_dc_release(graphics, hdc);

    return status;
}

static GpStatus SOFTWARE_GdipFillRegion(GpGraphics *graphics, GpBrush *brush,
    GpRegion* region)
{
    GpStatus stat;
    DWORD *pixel_data;
    HRGN hregion;
    RECT bound_rect;
    GpRect gp_bound_rect;

    if (!brush_can_fill_pixels(brush))
        return NotImplemented;

    stat = gdi_transform_acquire(graphics);

    if (stat == Ok)
        stat = get_clipped_region_hrgn(graphics, region, &hregion);

    if (stat == Ok && GetRgnBox(hregion, &bound_rect) == NULLREGION)
    {
        DeleteObject(hregion);
        gdi_transform_release(graphics);
        return Ok;
    }

    if (stat == Ok)
    {
        gp_bound_rect.X = bound_rect.left;
        gp_bound_rect.Y = bound_rect.top;
        gp_bound_rect.Width = bound_rect.right - bound_rect.left;
        gp_bound_rect.Height = bound_rect.bottom - bound_rect.top;

        pixel_data = calloc(gp_bound_rect.Width * gp_bound_rect.Height, sizeof(*pixel_data));
        if (!pixel_data)
            stat = OutOfMemory;

        if (stat == Ok)
        {
            stat = brush_fill_pixels(graphics, brush, pixel_data,
                &gp_bound_rect, gp_bound_rect.Width);

            if (stat == Ok)
                stat = alpha_blend_pixels_hrgn(graphics, gp_bound_rect.X,
                    gp_bound_rect.Y, (BYTE*)pixel_data, gp_bound_rect.Width,
                    gp_bound_rect.Height, gp_bound_rect.Width * 4, hregion,
                    PixelFormat32bppARGB);

            free(pixel_data);
        }

        DeleteObject(hregion);
    }

    gdi_transform_release(graphics);

    return stat;
}

/*****************************************************************************
 * GdipFillRegion [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipFillRegion(GpGraphics* graphics, GpBrush* brush,
        GpRegion* region)
{
    GpStatus stat = NotImplemented;

    TRACE("(%p, %p, %p)\n", graphics, brush, region);

    if (!(graphics && brush && region))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
        stat = METAFILE_FillRegion((GpMetafile*)graphics->image, brush, region);
    else
    {
        if (!graphics->image && !graphics->alpha_hdc)
            stat = GDI32_GdipFillRegion(graphics, brush, region);

        if (stat == NotImplemented)
            stat = SOFTWARE_GdipFillRegion(graphics, brush, region);
    }

    if (stat == NotImplemented)
    {
        FIXME("not implemented for brushtype %i\n", brush->bt);
        stat = Ok;
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFlush(GpGraphics *graphics, GpFlushIntention intention)
{
    TRACE("(%p,%u)\n", graphics, intention);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    /* We have no internal operation queue, so there's no need to clear it. */

    if (has_gdi_dc(graphics))
        GdiFlush();

    return Ok;
}

/*****************************************************************************
 * GdipGetClipBounds [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipGetClipBounds(GpGraphics *graphics, GpRectF *rect)
{
    GpStatus status;
    GpRegion *clip;

    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    status = GdipCreateRegion(&clip);
    if (status != Ok) return status;

    status = GdipGetClip(graphics, clip);
    if (status == Ok)
        status = GdipGetRegionBounds(clip, graphics, rect);

    GdipDeleteRegion(clip);
    return status;
}

/*****************************************************************************
 * GdipGetClipBoundsI [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipGetClipBoundsI(GpGraphics *graphics, GpRect *rect)
{
    GpRectF rectf;
    GpStatus stat;

    TRACE("(%p, %p)\n", graphics, rect);

    if (!rect)
        return InvalidParameter;

    if ((stat = GdipGetClipBounds(graphics, &rectf)) == Ok)
    {
        rect->X = gdip_round(rectf.X);
        rect->Y = gdip_round(rectf.Y);
        rect->Width  = gdip_round(rectf.Width);
        rect->Height = gdip_round(rectf.Height);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipGetCompositingMode(GpGraphics *graphics,
    CompositingMode *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->compmode;

    return Ok;
}

/* FIXME: Compositing quality is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetCompositingQuality(GpGraphics *graphics,
    CompositingQuality *quality)
{
    TRACE("(%p, %p)\n", graphics, quality);

    if(!graphics || !quality)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *quality = graphics->compqual;

    return Ok;
}

/* FIXME: Interpolation mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetInterpolationMode(GpGraphics *graphics,
    InterpolationMode *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->interpolation;

    return Ok;
}

/* FIXME: Need to handle color depths less than 24bpp */
GpStatus WINGDIPAPI GdipGetNearestColor(GpGraphics *graphics, ARGB* argb)
{
    TRACE("(%p, %p)\n", graphics, argb);

    if(!graphics || !argb)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (graphics->image && graphics->image->type == ImageTypeBitmap)
    {
        static int once;
        GpBitmap *bitmap = (GpBitmap *)graphics->image;
        if (IsIndexedPixelFormat(bitmap->format) && !once++)
            FIXME("(%p, %p): Passing color unmodified\n", graphics, argb);
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipGetPageScale(GpGraphics *graphics, REAL *scale)
{
    TRACE("(%p, %p)\n", graphics, scale);

    if(!graphics || !scale)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *scale = graphics->scale;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetPageUnit(GpGraphics *graphics, GpUnit *unit)
{
    TRACE("(%p, %p)\n", graphics, unit);

    if(!graphics || !unit)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *unit = graphics->unit;

    return Ok;
}

/* FIXME: Pixel offset mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetPixelOffsetMode(GpGraphics *graphics, PixelOffsetMode
    *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->pixeloffset;

    return Ok;
}

/* FIXME: Smoothing mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetSmoothingMode(GpGraphics *graphics, SmoothingMode *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->smoothing;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetTextContrast(GpGraphics *graphics, UINT *contrast)
{
    TRACE("(%p, %p)\n", graphics, contrast);

    if(!graphics || !contrast)
        return InvalidParameter;

    *contrast = graphics->textcontrast;

    return Ok;
}

/* FIXME: Text rendering hint is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetTextRenderingHint(GpGraphics *graphics,
    TextRenderingHint *hint)
{
    TRACE("(%p, %p)\n", graphics, hint);

    if(!graphics || !hint)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *hint = graphics->texthint;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetVisibleClipBounds(GpGraphics *graphics, GpRectF *rect)
{
    GpRegion *clip_rgn;
    GpStatus stat;
    GpMatrix device_to_world;

    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics || !rect)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    /* intersect window and graphics clipping regions */
    if((stat = GdipCreateRegion(&clip_rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, clip_rgn)) != Ok)
        goto cleanup;

    /* transform to world coordinates */
    if((stat = get_graphics_transform(graphics, CoordinateSpaceWorld, CoordinateSpaceDevice, &device_to_world)) != Ok)
        goto cleanup;

    if((stat = GdipTransformRegion(clip_rgn, &device_to_world)) != Ok)
        goto cleanup;

    /* get bounds of the region */
    stat = GdipGetRegionBounds(clip_rgn, graphics, rect);

cleanup:
    GdipDeleteRegion(clip_rgn);

    return stat;
}

GpStatus WINGDIPAPI GdipGetVisibleClipBoundsI(GpGraphics *graphics, GpRect *rect)
{
    GpRectF rectf;
    GpStatus stat;

    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics || !rect)
        return InvalidParameter;

    if((stat = GdipGetVisibleClipBounds(graphics, &rectf)) == Ok)
    {
        rect->X = gdip_round(rectf.X);
        rect->Y = gdip_round(rectf.Y);
        rect->Width  = gdip_round(rectf.Width);
        rect->Height = gdip_round(rectf.Height);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipGetWorldTransform(GpGraphics *graphics, GpMatrix *matrix)
{
    TRACE("(%p, %s)\n", graphics, debugstr_matrix(matrix));

    if(!graphics || !matrix)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *matrix = graphics->worldtrans;
    return Ok;
}

GpStatus WINGDIPAPI GdipGraphicsClear(GpGraphics *graphics, ARGB color)
{
    GpSolidFill *brush;
    GpStatus stat;
    GpRectF wnd_rect;
    CompositingMode prev_comp_mode;

    TRACE("(%p, %lx)\n", graphics, color);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
        return METAFILE_GraphicsClear((GpMetafile*)graphics->image, color);

    if((stat = GdipCreateSolidFill(color, &brush)) != Ok)
        return stat;

    if((stat = GdipGetVisibleClipBounds(graphics, &wnd_rect)) != Ok){
        GdipDeleteBrush((GpBrush*)brush);
        return stat;
    }

    GdipGetCompositingMode(graphics, &prev_comp_mode);
    GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
    GdipFillRectangle(graphics, (GpBrush*)brush, wnd_rect.X, wnd_rect.Y,
                                                 wnd_rect.Width, wnd_rect.Height);
    GdipSetCompositingMode(graphics, prev_comp_mode);

    GdipDeleteBrush((GpBrush*)brush);

    return Ok;
}

GpStatus WINGDIPAPI GdipIsClipEmpty(GpGraphics *graphics, BOOL *res)
{
    TRACE("(%p, %p)\n", graphics, res);

    if(!graphics || !res)
        return InvalidParameter;

    return GdipIsEmptyRegion(graphics->clip, graphics, res);
}

GpStatus WINGDIPAPI GdipIsVisiblePoint(GpGraphics *graphics, REAL x, REAL y, BOOL *result)
{
    GpStatus stat;
    GpRegion* rgn;
    GpPointF pt;

    TRACE("(%p, %.2f, %.2f, %p)\n", graphics, x, y, result);

    if(!graphics || !result)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pt.X = x;
    pt.Y = y;
    if((stat = GdipTransformPoints(graphics, CoordinateSpaceDevice,
                   CoordinateSpaceWorld, &pt, 1)) != Ok)
        return stat;

    if((stat = GdipCreateRegion(&rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, rgn)) != Ok)
        goto cleanup;

    stat = GdipIsVisibleRegionPoint(rgn, pt.X, pt.Y, graphics, result);

cleanup:
    GdipDeleteRegion(rgn);
    return stat;
}

GpStatus WINGDIPAPI GdipIsVisiblePointI(GpGraphics *graphics, INT x, INT y, BOOL *result)
{
    return GdipIsVisiblePoint(graphics, (REAL)x, (REAL)y, result);
}

GpStatus WINGDIPAPI GdipIsVisibleRect(GpGraphics *graphics, REAL x, REAL y, REAL width, REAL height, BOOL *result)
{
    GpStatus stat;
    GpRegion* rgn;
    GpPointF pts[2];

    TRACE("(%p %.2f %.2f %.2f %.2f %p)\n", graphics, x, y, width, height, result);

    if(!graphics || !result)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pts[0].X = x;
    pts[0].Y = y;
    pts[1].X = x + width;
    pts[1].Y = y + height;

    if((stat = GdipTransformPoints(graphics, CoordinateSpaceDevice,
                    CoordinateSpaceWorld, pts, 2)) != Ok)
        return stat;

    pts[1].X -= pts[0].X;
    pts[1].Y -= pts[0].Y;

    if((stat = GdipCreateRegion(&rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, rgn)) != Ok)
        goto cleanup;

    stat = GdipIsVisibleRegionRect(rgn, pts[0].X, pts[0].Y, pts[1].X, pts[1].Y, graphics, result);

cleanup:
    GdipDeleteRegion(rgn);
    return stat;
}

GpStatus WINGDIPAPI GdipIsVisibleRectI(GpGraphics *graphics, INT x, INT y, INT width, INT height, BOOL *result)
{
    return GdipIsVisibleRect(graphics, (REAL)x, (REAL)y, (REAL)width, (REAL)height, result);
}

/* Populates gdip_font_link_info struct based on the base_font and input string */
static void generate_font_link_info(struct gdip_format_string_info *info, DWORD length, GDIPCONST GpFont *base_font)
{
    IMLangFontLink *iMLFL;
    GpFont *gpfont;
    HFONT map_hfont, hfont, old_font;
    LONG processed, progress = 0;
    struct gdip_font_link_section *section;
    DWORD string_codepages;
    WORD *glyph_indices;
    HRESULT hr;
    GpStatus stat;

    list_init(&info->font_link_info.sections);
    info->font_link_info.base_font = base_font;

    glyph_indices = calloc(length, sizeof(*glyph_indices));
    GetGlyphIndicesW(info->hdc, info->string, length, glyph_indices, GGI_MARK_NONEXISTING_GLYPHS);

    /* Newlines won't have a glyph but don't need a fallback */
    for (progress = 0; progress < length; progress++)
        if (info->string[progress] == '\r' || info->string[progress] == '\n')
            glyph_indices[progress] = 0;

    GetGlobalFontLinkObject(&iMLFL);

    get_font_hfont(info->graphics, base_font, NULL, &hfont, NULL, NULL);

    progress = 0;
    while (progress < length)
    {
        section = calloc(1, sizeof(*section));
        section->start = progress;

        if (glyph_indices[progress] != 0xffff)
        {
            section->font = (GpFont *)base_font;

            processed = 0;
            while (progress + processed < length && glyph_indices[progress + processed] != 0xffff)
                processed++;
        }
        else
        {
            IMLangFontLink_GetStrCodePages(iMLFL, &info->string[progress], length - progress,
                                            0, &string_codepages, &processed);
            hr = IMLangFontLink_MapFont(iMLFL, info->hdc, string_codepages, hfont, &map_hfont);
            if (SUCCEEDED(hr))
            {
                old_font = SelectObject(info->hdc, map_hfont);
                stat = GdipCreateFontFromDC(info->hdc, &gpfont);
                SelectObject(info->hdc, old_font);
                IMLangFontLink_ReleaseFont(iMLFL, map_hfont);
                if (stat == Ok)
                    section->font = gpfont;
                else
                    section->font = (GpFont *)base_font;
            }
            else
                section->font = (GpFont *)base_font;
        }

        section->end = section->start + processed;
        list_add_tail(&info->font_link_info.sections, &section->entry);
        progress += processed;
    }

    DeleteObject(hfont);
    IMLangFontLink_Release(iMLFL);
    free(glyph_indices);
}

static void font_link_get_text_extent_point(struct gdip_format_string_info *info,
                                            INT index, int length, int max_ext, LPINT fit, SIZE *size)
{
    DWORD to_measure_length;
    HFONT hfont, oldhfont;
    SIZE sizeaux = { 0 };
    int i = index, fitaux = 0;
    struct gdip_font_link_section *section;

    size->cx = 0;
    size->cy = 0;

    if (fit)
        *fit = 0;

    LIST_FOR_EACH_ENTRY(section, &info->font_link_info.sections, struct gdip_font_link_section, entry)
    {
        if (i >= section->end) continue;

        to_measure_length = min(length - (i - index), section->end - i);

        get_font_hfont(info->graphics, section->font, NULL, &hfont, NULL, NULL);
        oldhfont = SelectObject(info->hdc, hfont);
        GetTextExtentExPointW(info->hdc, &info->string[i], to_measure_length, max_ext, &fitaux, NULL, &sizeaux);
        SelectObject(info->hdc, oldhfont);
        DeleteObject(hfont);

        max_ext -= sizeaux.cx;
        if (fit)
            *fit += fitaux;
        size->cx += sizeaux.cx;
        size->cy = max(size->cy, sizeaux.cy);

        i += to_measure_length;
        if ((i - index) >= length || fitaux < to_measure_length) break;
    }
}

static void release_font_link_info(struct gdip_font_link_info *font_link_info)
{
    struct list *entry;

    while ((entry = list_head(&font_link_info->sections)))
    {
        struct gdip_font_link_section *section = LIST_ENTRY(entry, struct gdip_font_link_section, entry);
        list_remove(entry);
        if (section->font != font_link_info->base_font)
            GdipDeleteFont(section->font);
        free(section);
    }
}

GpStatus gdip_format_string(GpGraphics *graphics, HDC hdc,
    GDIPCONST WCHAR *string, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format, int ignore_empty_clip,
    gdip_format_string_callback callback, void *user_data)
{
    WCHAR* stringdup;
    int sum = 0, height = 0, fit, fitcpy, i, j, lret, nwidth,
        nheight, lineend, lineno = 0;
    RectF bounds;
    StringAlignment halign;
    GpStatus stat = Ok;
    SIZE size;
    HotkeyPrefix hkprefix;
    INT *hotkeyprefix_offsets=NULL;
    INT hotkeyprefix_count=0;
    INT hotkeyprefix_pos=0, hotkeyprefix_end_pos=0;
    BOOL seen_prefix = FALSE, unixstyle_newline = TRUE;
    struct gdip_format_string_info info;

    info.graphics = graphics;
    info.hdc = hdc;
    info.rect = rect;
    info.bounds = &bounds;
    info.user_data = user_data;

    if(length == -1) length = lstrlenW(string);

    stringdup = calloc(length + 1, sizeof(WCHAR));
    if(!stringdup) return OutOfMemory;

    info.string = stringdup;

    if (!format)
        format = &default_drawstring_format;

    info.format = format;

    nwidth = (int)(rect->Width + 0.005f);
    nheight = (int)(rect->Height + 0.005f);
    if (ignore_empty_clip)
    {
        if (!nwidth) nwidth = INT_MAX;
        if (!nheight) nheight = INT_MAX;
    }

    hkprefix = format->hkprefix;

    if (hkprefix == HotkeyPrefixShow)
    {
        for (i=0; i<length; i++)
        {
            if (string[i] == '&')
                hotkeyprefix_count++;
        }
    }

    if (hotkeyprefix_count)
    {
        hotkeyprefix_offsets = calloc(hotkeyprefix_count, sizeof(INT));
        if (!hotkeyprefix_offsets)
        {
            free(stringdup);
            return OutOfMemory;
        }
    }

    hotkeyprefix_count = 0;

    for(i = 0, j = 0; i < length; i++){

        /* FIXME: tabs should be handled using tabstops from stringformat */
        if (string[i] == '\t')
            continue;

        if (seen_prefix && hkprefix == HotkeyPrefixShow && string[i] != '&')
            hotkeyprefix_offsets[hotkeyprefix_count++] = j;
        else if (!seen_prefix && hkprefix != HotkeyPrefixNone && string[i] == '&')
        {
            seen_prefix = TRUE;
            continue;
        }

        seen_prefix = FALSE;

        stringdup[j] = string[i];
        j++;
    }

    length = j;

    halign = format->align;

    generate_font_link_info(&info, length, font);

    while(sum < length){
        font_link_get_text_extent_point(&info, sum, length - sum, nwidth, &fit, &size);
        fitcpy = fit;

        if(fit == 0)
            break;

        for(lret = 0; lret < fit; lret++) {
            if(*(stringdup + sum + lret) == '\n')
            {
               unixstyle_newline = TRUE;
               break;
            }

            if(*(stringdup + sum + lret) == '\r' && lret + 1 < fit
               && *(stringdup + sum + lret + 1) == '\n')
            {
               unixstyle_newline = FALSE;
               break;
            }
        }

        /* Line break code (may look strange, but it imitates windows). */
        if(lret < fit)
            lineend = fit = lret;    /* this is not an off-by-one error */
        else if(fit < (length - sum)){
            if(*(stringdup + sum + fit) == ' ')
                while(*(stringdup + sum + fit) == ' ')
                    fit++;
            else if (!(format->attr & StringFormatFlagsNoWrap))
                while(*(stringdup + sum + fit - 1) != ' '){
                    fit--;

                    if(*(stringdup + sum + fit) == '\t')
                        break;

                    if(fit == 0){
                        fit = fitcpy;
                        break;
                    }
                }
            lineend = fit;
            while(*(stringdup + sum + lineend - 1) == ' ' ||
                  *(stringdup + sum + lineend - 1) == '\t')
                lineend--;
        }
        else
            lineend = fit;

        font_link_get_text_extent_point(&info, sum, lineend, nwidth, &j, &size);

        bounds.Width = size.cx;

        if(height + size.cy > nheight)
        {
            if (format->attr & StringFormatFlagsLineLimit)
                break;
            bounds.Height = nheight - height;
        }
        else
            bounds.Height = size.cy;

        bounds.Y = rect->Y + height;

        switch (halign)
        {
        case StringAlignmentNear:
        default:
            bounds.X = rect->X;
            break;
        case StringAlignmentCenter:
            bounds.X = rect->X + (rect->Width/2) - (bounds.Width/2);
            break;
        case StringAlignmentFar:
            bounds.X = rect->X + rect->Width - bounds.Width;
            break;
        }

        for (hotkeyprefix_end_pos=hotkeyprefix_pos; hotkeyprefix_end_pos<hotkeyprefix_count; hotkeyprefix_end_pos++)
            if (hotkeyprefix_offsets[hotkeyprefix_end_pos] >= sum + lineend)
                break;

        info.index = sum;
        info.length = lineend;
        info.lineno = lineno;
        info.underlined_indexes = &hotkeyprefix_offsets[hotkeyprefix_pos];
        info.underlined_index_count = hotkeyprefix_end_pos-hotkeyprefix_pos;

        stat = callback(&info);

        if (stat != Ok)
            break;


        if (unixstyle_newline)
        {
            height += size.cy;
            lineno++;
            sum += fit + (lret < fitcpy ? 1 : 0);
        }
        else
        {
            height += size.cy;
            lineno++;
            sum += fit + (lret < fitcpy ? 2 : 0);
        }

        hotkeyprefix_pos = hotkeyprefix_end_pos;

        if(height > nheight)
            break;

        /* Stop if this was a linewrap (but not if it was a linebreak). */
        if ((lret == fitcpy) && (format->attr & StringFormatFlagsNoWrap))
            break;
    }

    release_font_link_info(&info.font_link_info);
    free(stringdup);
    free(hotkeyprefix_offsets);

    return stat;
}

void transform_properties(GpGraphics *graphics, GDIPCONST GpMatrix *matrix, BOOL graphics_transform,
                          REAL *rel_width, REAL *rel_height, REAL *angle)
{
    GpPointF pt[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    GpMatrix xform;

    if (matrix)
    {
        xform = *matrix;
        GdipTransformMatrixPoints(&xform, pt, 3);
    }

    if (graphics_transform)
        gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, pt, 3);

    if (rel_width)
        *rel_width = hypotf(pt[1].Y - pt[0].Y, pt[1].X - pt[0].X);
    if (rel_height)
        *rel_height = hypotf(pt[2].Y - pt[0].Y, pt[2].X - pt[0].X);
    if (angle)
        *angle = -gdiplus_atan2((pt[1].Y - pt[0].Y), (pt[1].X - pt[0].X));
}

struct measure_ranges_args {
    GpRegion **regions;
    REAL rel_width, rel_height;
};

static GpStatus measure_ranges_callback(struct gdip_format_string_info *info)
{
    int i;
    GpStatus stat = Ok;
    struct measure_ranges_args *args = info->user_data;
    CharacterRange *ranges = info->format->character_ranges;

    for (i=0; i < info->format->range_count; i++)
    {
        INT range_start = max(info->index, ranges[i].First);
        INT range_end = min(info->index + info->length, ranges[i].First + ranges[i].Length);
        if (range_start < range_end)
        {
            GpRectF range_rect;
            SIZE range_size;

            range_rect.Y = info->bounds->Y / args->rel_height;
            range_rect.Height = info->bounds->Height / args->rel_height;

            font_link_get_text_extent_point(info, info->index, range_start - info->index, INT_MAX, NULL, &range_size);
            range_rect.X = (info->bounds->X + range_size.cx) / args->rel_width;

            font_link_get_text_extent_point(info, info->index, range_end - info->index, INT_MAX, NULL, &range_size);
            range_rect.Width = (info->bounds->X + range_size.cx) / args->rel_width - range_rect.X;

            stat = GdipCombineRegionRect(args->regions[i], &range_rect, CombineModeUnion);
            if (stat != Ok)
                break;
        }
    }

    return stat;
}

GpStatus WINGDIPAPI GdipMeasureCharacterRanges(GpGraphics* graphics,
        GDIPCONST WCHAR* string, INT length, GDIPCONST GpFont* font,
        GDIPCONST RectF* layoutRect, GDIPCONST GpStringFormat *stringFormat,
        INT regionCount, GpRegion** regions)
{
    GpStatus stat;
    int i;
    HFONT gdifont, oldfont;
    struct measure_ranges_args args;
    HDC hdc, temp_hdc=NULL;
    RectF scaled_rect;
    REAL margin_x;

    TRACE("(%p %s %d %p %s %p %d %p)\n", graphics, debugstr_wn(string, length),
            length, font, debugstr_rectf(layoutRect), stringFormat, regionCount, regions);

    if (!(graphics && string && font && layoutRect && stringFormat && regions))
        return InvalidParameter;

    if (regionCount < stringFormat->range_count)
        return InvalidParameter;

    if(!has_gdi_dc(graphics))
    {
        hdc = temp_hdc = CreateCompatibleDC(0);
        if (!temp_hdc) return OutOfMemory;
    }
    else
    {
        stat = gdi_dc_acquire(graphics, &hdc);
        if (stat != Ok)
            return stat;
    }

    if (stringFormat->attr)
        TRACE("may be ignoring some format flags: attr %x\n", stringFormat->attr);


    margin_x = stringFormat->generic_typographic ? 0.0 : font->emSize / 6.0;
    margin_x *= units_scale(font->unit, graphics->unit, graphics->xres, graphics->printer_display);
    transform_properties(graphics, NULL, TRUE, &args.rel_width, &args.rel_height, NULL);
    scaled_rect.X = (layoutRect->X + margin_x) * args.rel_width;
    scaled_rect.Y = layoutRect->Y * args.rel_height;
    scaled_rect.Width = layoutRect->Width * args.rel_width;
    scaled_rect.Height = layoutRect->Height * args.rel_height;

    if (scaled_rect.Width >= 1 << 23) scaled_rect.Width = 1 << 23;
    if (scaled_rect.Height >= 1 << 23) scaled_rect.Height = 1 << 23;

    get_font_hfont(graphics, font, stringFormat, &gdifont, NULL, NULL);
    oldfont = SelectObject(hdc, gdifont);

    for (i=0; i<stringFormat->range_count; i++)
    {
        stat = GdipSetEmpty(regions[i]);
        if (stat != Ok)
        {
            SelectObject(hdc, oldfont);
            DeleteObject(gdifont);
            if (temp_hdc)
                DeleteDC(temp_hdc);
            return stat;
        }
    }

    args.regions = regions;

    gdi_transform_acquire(graphics);

    stat = gdip_format_string(graphics, hdc, string, length, font, &scaled_rect, stringFormat,
        (stringFormat->attr & StringFormatFlagsNoClip) != 0, measure_ranges_callback, &args);

    gdi_transform_release(graphics);

    SelectObject(hdc, oldfont);
    DeleteObject(gdifont);

    if (temp_hdc)
        DeleteDC(temp_hdc);
    else
        gdi_dc_release(graphics, hdc);

    return stat;
}

struct measure_string_args {
    RectF *bounds;
    INT *codepointsfitted;
    INT *linesfilled;
    REAL rel_width, rel_height;
};

static GpStatus measure_string_callback(struct gdip_format_string_info *info)
{
    struct measure_string_args *args = info->user_data;
    RectF *bounds = args->bounds;
    REAL new_width, new_height;

    new_width = info->bounds->Width / args->rel_width;
    new_height = (info->bounds->Height + info->bounds->Y) / args->rel_height - bounds->Y;

    if (new_width > bounds->Width)
        bounds->Width = new_width;

    if (new_height > bounds->Height)
        bounds->Height = new_height;

    if (args->codepointsfitted)
        *args->codepointsfitted = info->index + info->length;

    if (args->linesfilled)
        (*args->linesfilled)++;

    switch (info->format ? info->format->align : StringAlignmentNear)
    {
    case StringAlignmentCenter:
        bounds->X = bounds->X + (info->rect->Width/2) - (bounds->Width/2);
        break;
    case StringAlignmentFar:
        bounds->X = bounds->X + info->rect->Width - bounds->Width;
        break;
    default:
        break;
    }

    return Ok;
}

/* Find the smallest rectangle that bounds the text when it is printed in rect
 * according to the format options listed in format. If rect has 0 width and
 * height, then just find the smallest rectangle that bounds the text when it's
 * printed at location (rect->X, rect-Y). */
GpStatus WINGDIPAPI GdipMeasureString(GpGraphics *graphics,
    GDIPCONST WCHAR *string, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format, RectF *bounds,
    INT *codepointsfitted, INT *linesfilled)
{
    GpStatus status;
    HFONT oldfont, gdifont;
    struct measure_string_args args;
    HDC temp_hdc=NULL, hdc;
    RectF scaled_rect;
    REAL margin_x;
    INT lines, glyphs;

    TRACE("(%p, %s, %i, %p, %s, %p, %p, %p, %p)\n", graphics,
        debugstr_wn(string, length), length, font, debugstr_rectf(rect), format,
        bounds, codepointsfitted, linesfilled);

    if(!graphics || !string || !font || !rect || !bounds)
        return InvalidParameter;

    if(!has_gdi_dc(graphics))
    {
        hdc = temp_hdc = CreateCompatibleDC(0);
        if (!temp_hdc) return OutOfMemory;
    }
    else
    {
        status = gdi_dc_acquire(graphics, &hdc);
        if (status != Ok)
            return status;
    }

    if(linesfilled) *linesfilled = 0;
    if(codepointsfitted) *codepointsfitted = 0;

    if(format)
        TRACE("may be ignoring some format flags: attr %x\n", format->attr);

    transform_properties(graphics, NULL, TRUE, &args.rel_width, &args.rel_height, NULL);
    margin_x = (format && format->generic_typographic) ? 0.0 : font->emSize / 6.0;
    margin_x *= units_scale(font->unit, graphics->unit, graphics->xres, graphics->printer_display);

    scaled_rect.X = (rect->X + margin_x) * args.rel_width;
    scaled_rect.Y = rect->Y * args.rel_height;
    scaled_rect.Width = rect->Width * args.rel_width;
    scaled_rect.Height = rect->Height * args.rel_height;
    if (scaled_rect.Width >= 0.5)
    {
        scaled_rect.Width -= margin_x * 2.0 * args.rel_width;
        if (scaled_rect.Width < 0.5) /* doesn't fit */
            goto end;
    }

    if (scaled_rect.Width >= 1 << 23) scaled_rect.Width = 1 << 23;
    if (scaled_rect.Height >= 1 << 23) scaled_rect.Height = 1 << 23;

    get_font_hfont(graphics, font, format, &gdifont, NULL, NULL);
    oldfont = SelectObject(hdc, gdifont);

    set_rect(bounds, rect->X, rect->Y, 0.0f, 0.0f);

    args.bounds = bounds;
    args.codepointsfitted = &glyphs;
    args.linesfilled = &lines;
    lines = glyphs = 0;

    gdi_transform_acquire(graphics);

    gdip_format_string(graphics, hdc, string, length, font, &scaled_rect, format, TRUE,
        measure_string_callback, &args);

    gdi_transform_release(graphics);

    if (linesfilled) *linesfilled = lines;
    if (codepointsfitted) *codepointsfitted = glyphs;

    if (lines)
        bounds->Width += margin_x * 2.0;

    SelectObject(hdc, oldfont);
    DeleteObject(gdifont);
end:
    if (temp_hdc)
        DeleteDC(temp_hdc);
    else
        gdi_dc_release(graphics, hdc);

    return Ok;
}

struct draw_string_args {
    GDIPCONST GpBrush *brush;
    REAL x, y, rel_width, rel_height, ascent;
};

static GpStatus draw_string_callback(struct gdip_format_string_info *info)
{
    struct draw_string_args *args = info->user_data;
    int i = info->index;
    PointF position;
    SIZE size;
    DWORD to_draw_length;
    struct gdip_font_link_section *section;
    GpStatus stat = Ok;

    position.X = args->x + info->bounds->X / args->rel_width;
    position.Y = args->y + info->bounds->Y / args->rel_height + args->ascent;

    LIST_FOR_EACH_ENTRY(section, &info->font_link_info.sections, struct gdip_font_link_section, entry)
    {
        if (i >= section->end) continue;

        to_draw_length = min(info->length - (i - info->index), section->end - i);
        TRACE("index %d, todraw %ld, used %s\n", i, to_draw_length, section->font == info->font_link_info.base_font ? "base font" : "map");
        font_link_get_text_extent_point(info, i, to_draw_length, 0, NULL, &size);
        stat = draw_driver_string(info->graphics, &info->string[i], to_draw_length,
            section->font, info->format, args->brush, &position,
            DriverStringOptionsCmapLookup|DriverStringOptionsRealizedAdvance, NULL);
        position.X += size.cx / args->rel_width;
        i += to_draw_length;
        if (stat != Ok || (i - info->index) >= info->length) break;
    }

    if (stat == Ok && info->underlined_index_count)
    {
        OUTLINETEXTMETRICW otm;
        REAL underline_y, underline_height;
        int i;

        GetOutlineTextMetricsW(info->hdc, sizeof(otm), &otm);

        position.X = args->x + info->bounds->X / args->rel_width;
        underline_height = otm.otmsUnderscoreSize / args->rel_height;
        underline_y = position.Y - otm.otmsUnderscorePosition / args->rel_height - underline_height / 2;

        for (i=0; i<info->underlined_index_count; i++)
        {
            REAL start_x, end_x;
            SIZE text_size;
            INT ofs = info->underlined_indexes[i] - info->index;

            font_link_get_text_extent_point(info, info->index, ofs, INT_MAX, NULL, &text_size);
            start_x = text_size.cx / args->rel_width;

            font_link_get_text_extent_point(info, info->index, ofs+1, INT_MAX, NULL, &text_size);
            end_x = text_size.cx / args->rel_width;

            GdipFillRectangle(info->graphics, (GpBrush*)args->brush, position.X+start_x, underline_y, end_x-start_x, underline_height);
        }
    }

    return stat;
}

GpStatus WINGDIPAPI GdipDrawString(GpGraphics *graphics, GDIPCONST WCHAR *string,
    INT length, GDIPCONST GpFont *font, GDIPCONST RectF *rect,
    GDIPCONST GpStringFormat *format, GDIPCONST GpBrush *brush)
{
    GpStatus status;
    HRGN rgn = NULL;
    HFONT gdifont;
    GpPointF rectcpy[4];
    POINT corners[4];
    REAL rel_width, rel_height, margin_x;
    INT save_state, format_flags = 0;
    REAL offsety = 0.0;
    struct draw_string_args args;
    RectF scaled_rect;
    HDC hdc, temp_hdc=NULL;
    TEXTMETRICW textmetric;

    TRACE("(%p, %s, %i, %p, %s, %p, %p)\n", graphics, debugstr_wn(string, length),
        length, font, debugstr_rectf(rect), format, brush);

    if(!graphics || !string || !font || !brush || !rect)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(has_gdi_dc(graphics))
    {
        status = gdi_dc_acquire(graphics, &hdc);
        if (status != Ok)
            return status;
    }
    else
    {
        hdc = temp_hdc = CreateCompatibleDC(0);
    }

    if(format){
        TRACE("may be ignoring some format flags: attr %x\n", format->attr);

        format_flags = format->attr;

        /* Should be no need to explicitly test for StringAlignmentNear as
         * that is default behavior if no alignment is passed. */
        if(format->line_align != StringAlignmentNear){
            RectF bounds, in_rect = *rect;
            in_rect.Height = 0.0; /* avoid height clipping */
            GdipMeasureString(graphics, string, length, font, &in_rect, format, &bounds, 0, 0);

            TRACE("bounds %s\n", debugstr_rectf(&bounds));

            if(format->line_align == StringAlignmentCenter)
                offsety = (rect->Height - bounds.Height) / 2;
            else if(format->line_align == StringAlignmentFar)
                offsety = (rect->Height - bounds.Height);
        }
        TRACE("line align %d, offsety %f\n", format->line_align, offsety);
    }

    save_state = SaveDC(hdc);

    transform_properties(graphics, NULL, TRUE, &rel_width, &rel_height, NULL);
    rectcpy[3].X = rectcpy[0].X = rect->X;
    rectcpy[1].Y = rectcpy[0].Y = rect->Y;
    rectcpy[2].X = rectcpy[1].X = rect->X + rect->Width;
    rectcpy[3].Y = rectcpy[2].Y = rect->Y + rect->Height;
    gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, rectcpy, 4);
    round_points(corners, rectcpy, 4);

    margin_x = (format && format->generic_typographic) ? 0.0 : font->emSize / 6.0;
    margin_x *= units_scale(font->unit, graphics->unit, graphics->xres, graphics->printer_display);

    scaled_rect.X = margin_x * rel_width;
    scaled_rect.Y = 0.0;
    scaled_rect.Width = rel_width * rect->Width;
    scaled_rect.Height = rel_height * rect->Height;
    if (scaled_rect.Width >= 0.5)
    {
        scaled_rect.Width -= margin_x * 2.0 * rel_width;
        if (scaled_rect.Width < 0.5) /* doesn't fit */
            goto end;
    }

    if (scaled_rect.Width >= 1 << 23) scaled_rect.Width = 1 << 23;
    if (scaled_rect.Height >= 1 << 23) scaled_rect.Height = 1 << 23;

    if (!(format_flags & StringFormatFlagsNoClip) &&
        scaled_rect.Width != 1 << 23 && scaled_rect.Height != 1 << 23 &&
        rect->Width > 0.0 && rect->Height > 0.0)
    {
        /* FIXME: If only the width or only the height is 0, we should probably still clip */
        rgn = CreatePolygonRgn(corners, 4, ALTERNATE);
        SelectClipRgn(hdc, rgn);
    }

    get_font_hfont(graphics, font, format, &gdifont, NULL, NULL);
    SelectObject(hdc, gdifont);

    args.brush = brush;

    args.x = rect->X;
    args.y = rect->Y + offsety;

    args.rel_width = rel_width;
    args.rel_height = rel_height;

    gdi_transform_acquire(graphics);

    GetTextMetricsW(hdc, &textmetric);
    args.ascent = textmetric.tmAscent / rel_height;

    gdip_format_string(graphics, hdc, string, length, font, &scaled_rect, format, TRUE,
        draw_string_callback, &args);

    gdi_transform_release(graphics);

    DeleteObject(rgn);
    DeleteObject(gdifont);
end:
    RestoreDC(hdc, save_state);

    if (temp_hdc)
        DeleteDC(temp_hdc);
    else
        gdi_dc_release(graphics, hdc);

    return Ok;
}

GpStatus WINGDIPAPI GdipResetClip(GpGraphics *graphics)
{
    GpStatus stat;

    TRACE("(%p)\n", graphics);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_ResetClip((GpMetafile *)graphics->image);
        if (stat != Ok)
            return stat;
    }

    return GdipSetInfinite(graphics->clip);
}

GpStatus WINGDIPAPI GdipResetWorldTransform(GpGraphics *graphics)
{
    GpStatus stat;

    TRACE("(%p)\n", graphics);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_ResetWorldTransform((GpMetafile*)graphics->image);

        if (stat != Ok)
            return stat;
    }

    return GdipSetMatrixElements(&graphics->worldtrans, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
}

GpStatus WINGDIPAPI GdipRotateWorldTransform(GpGraphics *graphics, REAL angle,
    GpMatrixOrder order)
{
    GpStatus stat;

    TRACE("(%p, %.2f, %d)\n", graphics, angle, order);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_RotateWorldTransform((GpMetafile*)graphics->image, angle, order);

        if (stat != Ok)
            return stat;
    }

    return GdipRotateMatrix(&graphics->worldtrans, angle, order);
}

static GpStatus begin_container(GpGraphics *graphics,
    GraphicsContainerType type, GraphicsContainer *state)
{
    GraphicsContainerItem *container;
    GpStatus sts;

    if(!graphics || !state)
        return InvalidParameter;

    sts = init_container(&container, graphics, type);
    if(sts != Ok)
        return sts;

    list_add_head(&graphics->containers, &container->entry);
    *state = graphics->contid = container->contid;

    if (is_metafile_graphics(graphics)) {
        if (type == BEGIN_CONTAINER)
            METAFILE_BeginContainerNoParams((GpMetafile*)graphics->image, container->contid);
        else
            METAFILE_SaveGraphics((GpMetafile*)graphics->image, container->contid);
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipSaveGraphics(GpGraphics *graphics, GraphicsState *state)
{
    TRACE("(%p, %p)\n", graphics, state);
    return begin_container(graphics, SAVE_GRAPHICS, state);
}

GpStatus WINGDIPAPI GdipBeginContainer2(GpGraphics *graphics,
        GraphicsContainer *state)
{
    TRACE("(%p, %p)\n", graphics, state);
    return begin_container(graphics, BEGIN_CONTAINER, state);
}

GpStatus WINGDIPAPI GdipBeginContainer(GpGraphics *graphics, GDIPCONST GpRectF *dstrect, GDIPCONST GpRectF *srcrect, GpUnit unit, GraphicsContainer *state)
{
    GraphicsContainerItem *container;
    GpMatrix transform;
    GpStatus stat;
    GpRectF scaled_srcrect;
    REAL scale_x, scale_y;

    TRACE("(%p, %s, %s, %d, %p)\n", graphics, debugstr_rectf(dstrect), debugstr_rectf(srcrect), unit, state);

    if(!graphics || !dstrect || !srcrect || unit < UnitPixel || unit > UnitMillimeter || !state)
        return InvalidParameter;

    stat = init_container(&container, graphics, BEGIN_CONTAINER);
    if(stat != Ok)
        return stat;

    list_add_head(&graphics->containers, &container->entry);
    *state = graphics->contid = container->contid;

    scale_x = units_to_pixels(1.0, unit, graphics->xres, graphics->printer_display);
    scale_y = units_to_pixels(1.0, unit, graphics->yres, graphics->printer_display);

    scaled_srcrect.X = scale_x * srcrect->X;
    scaled_srcrect.Y = scale_y * srcrect->Y;
    scaled_srcrect.Width = scale_x * srcrect->Width;
    scaled_srcrect.Height = scale_y * srcrect->Height;

    transform.matrix[0] = dstrect->Width / scaled_srcrect.Width;
    transform.matrix[1] = 0.0;
    transform.matrix[2] = 0.0;
    transform.matrix[3] = dstrect->Height / scaled_srcrect.Height;
    transform.matrix[4] = dstrect->X - scaled_srcrect.X;
    transform.matrix[5] = dstrect->Y - scaled_srcrect.Y;

    GdipMultiplyMatrix(&graphics->worldtrans, &transform, MatrixOrderPrepend);

    if (is_metafile_graphics(graphics))
        METAFILE_BeginContainer((GpMetafile*)graphics->image, dstrect, srcrect, unit, container->contid);

    return Ok;
}

GpStatus WINGDIPAPI GdipBeginContainerI(GpGraphics *graphics, GDIPCONST GpRect *dstrect, GDIPCONST GpRect *srcrect, GpUnit unit, GraphicsContainer *state)
{
    GpRectF dstrectf, srcrectf;

    TRACE("(%p, %p, %p, %d, %p)\n", graphics, dstrect, srcrect, unit, state);

    if (!dstrect || !srcrect)
        return InvalidParameter;

    dstrectf.X = dstrect->X;
    dstrectf.Y = dstrect->Y;
    dstrectf.Width = dstrect->Width;
    dstrectf.Height = dstrect->Height;

    srcrectf.X = srcrect->X;
    srcrectf.Y = srcrect->Y;
    srcrectf.Width = srcrect->Width;
    srcrectf.Height = srcrect->Height;

    return GdipBeginContainer(graphics, &dstrectf, &srcrectf, unit, state);
}

GpStatus WINGDIPAPI GdipComment(GpGraphics *graphics, UINT sizeData, GDIPCONST BYTE *data)
{
    FIXME("(%p, %d, %p): stub\n", graphics, sizeData, data);
    return NotImplemented;
}

static GpStatus end_container(GpGraphics *graphics, GraphicsContainerType type,
    GraphicsContainer state)
{
    GpStatus sts;
    GraphicsContainerItem *container, *container2;

    if(!graphics)
        return InvalidParameter;

    LIST_FOR_EACH_ENTRY(container, &graphics->containers, GraphicsContainerItem, entry){
        if(container->contid == state && container->type == type)
            break;
    }

    /* did not find a matching container */
    if(&container->entry == &graphics->containers)
        return Ok;

    sts = restore_container(graphics, container);
    if(sts != Ok)
        return sts;

    /* remove all of the containers on top of the found container */
    LIST_FOR_EACH_ENTRY_SAFE(container, container2, &graphics->containers, GraphicsContainerItem, entry){
        if(container->contid == state)
            break;
        list_remove(&container->entry);
        delete_container(container);
    }

    list_remove(&container->entry);
    delete_container(container);

    if (is_metafile_graphics(graphics)) {
        if (type == BEGIN_CONTAINER)
            METAFILE_EndContainer((GpMetafile*)graphics->image, state);
        else
            METAFILE_RestoreGraphics((GpMetafile*)graphics->image, state);
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipEndContainer(GpGraphics *graphics, GraphicsContainer state)
{
    TRACE("(%p, %x)\n", graphics, state);
    return end_container(graphics, BEGIN_CONTAINER, state);
}

GpStatus WINGDIPAPI GdipRestoreGraphics(GpGraphics *graphics, GraphicsState state)
{
    TRACE("(%p, %x)\n", graphics, state);
    return end_container(graphics, SAVE_GRAPHICS, state);
}

GpStatus WINGDIPAPI GdipScaleWorldTransform(GpGraphics *graphics, REAL sx,
    REAL sy, GpMatrixOrder order)
{
    GpStatus stat;

    TRACE("(%p, %.2f, %.2f, %d)\n", graphics, sx, sy, order);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics)) {
        stat = METAFILE_ScaleWorldTransform((GpMetafile*)graphics->image, sx, sy, order);

        if (stat != Ok)
            return stat;
    }

    return GdipScaleMatrix(&graphics->worldtrans, sx, sy, order);
}

GpStatus WINGDIPAPI GdipSetClipGraphics(GpGraphics *graphics, GpGraphics *srcgraphics,
    CombineMode mode)
{
    TRACE("(%p, %p, %d)\n", graphics, srcgraphics, mode);

    if(!graphics || !srcgraphics)
        return InvalidParameter;

    return GdipCombineRegionRegion(graphics->clip, srcgraphics->clip, mode);
}

GpStatus WINGDIPAPI GdipSetCompositingMode(GpGraphics *graphics,
    CompositingMode mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(graphics->compmode == mode)
        return Ok;

    if (is_metafile_graphics(graphics))
    {
        GpStatus stat;

        stat = METAFILE_AddSimpleProperty((GpMetafile*)graphics->image,
                EmfPlusRecordTypeSetCompositingMode, mode);
        if(stat != Ok)
            return stat;
    }

    graphics->compmode = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetCompositingQuality(GpGraphics *graphics,
    CompositingQuality quality)
{
    TRACE("(%p, %d)\n", graphics, quality);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(graphics->compqual == quality)
        return Ok;

    if (is_metafile_graphics(graphics))
    {
        GpStatus stat;

        stat = METAFILE_AddSimpleProperty((GpMetafile*)graphics->image,
                EmfPlusRecordTypeSetCompositingQuality, quality);
        if(stat != Ok)
            return stat;
    }

    graphics->compqual = quality;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetInterpolationMode(GpGraphics *graphics,
    InterpolationMode mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics || mode == InterpolationModeInvalid || mode > InterpolationModeHighQualityBicubic)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (mode == InterpolationModeDefault || mode == InterpolationModeLowQuality)
        mode = InterpolationModeBilinear;

    if (mode == InterpolationModeHighQuality)
        mode = InterpolationModeHighQualityBicubic;

    if (mode == graphics->interpolation)
        return Ok;

    if (is_metafile_graphics(graphics))
    {
        GpStatus stat;

        stat = METAFILE_AddSimpleProperty((GpMetafile*)graphics->image,
                EmfPlusRecordTypeSetInterpolationMode, mode);
        if (stat != Ok)
            return stat;
    }

    graphics->interpolation = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPageScale(GpGraphics *graphics, REAL scale)
{
    GpStatus stat;

    TRACE("(%p, %.2f)\n", graphics, scale);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(scale <= 0.0)
        return InvalidParameter;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_SetPageTransform((GpMetafile*)graphics->image, graphics->unit, scale);
        if (stat != Ok)
            return stat;
    }

    graphics->scale = scale;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPageUnit(GpGraphics *graphics, GpUnit unit)
{
    GpStatus stat;

    TRACE("(%p, %d)\n", graphics, unit);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(unit == UnitWorld || unit > UnitMillimeter)
        return InvalidParameter;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_SetPageTransform((GpMetafile*)graphics->image, unit, graphics->scale);
        if (stat != Ok)
            return stat;
    }

    graphics->unit = unit;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPixelOffsetMode(GpGraphics *graphics, PixelOffsetMode
    mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(graphics->pixeloffset == mode)
        return Ok;

    if (is_metafile_graphics(graphics))
    {
        GpStatus stat;

        stat = METAFILE_AddSimpleProperty((GpMetafile*)graphics->image,
                EmfPlusRecordTypeSetPixelOffsetMode, mode);
        if(stat != Ok)
            return stat;
    }

    graphics->pixeloffset = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetRenderingOrigin(GpGraphics *graphics, INT x, INT y)
{
    GpStatus stat;

    TRACE("(%p,%i,%i)\n", graphics, x, y);

    if (!graphics)
        return InvalidParameter;

    if (graphics->origin_x == x && graphics->origin_y == y)
        return Ok;

    if (is_metafile_graphics(graphics))
    {
         stat = METAFILE_SetRenderingOrigin((GpMetafile *)graphics->image, x, y);
         if (stat != Ok)
             return stat;
    }

    graphics->origin_x = x;
    graphics->origin_y = y;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetRenderingOrigin(GpGraphics *graphics, INT *x, INT *y)
{
    TRACE("(%p,%p,%p)\n", graphics, x, y);

    if (!graphics || !x || !y)
        return InvalidParameter;

    *x = graphics->origin_x;
    *y = graphics->origin_y;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetSmoothingMode(GpGraphics *graphics, SmoothingMode mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(graphics->smoothing == mode)
        return Ok;

    if (is_metafile_graphics(graphics))
    {
         GpStatus stat;
         BOOL antialias = (mode != SmoothingModeDefault &&
                 mode != SmoothingModeNone && mode != SmoothingModeHighSpeed);

         stat = METAFILE_AddSimpleProperty((GpMetafile*)graphics->image,
                 EmfPlusRecordTypeSetAntiAliasMode, (mode << 1) + antialias);
         if(stat != Ok)
             return stat;
    }

    graphics->smoothing = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetTextContrast(GpGraphics *graphics, UINT contrast)
{
    TRACE("(%p, %d)\n", graphics, contrast);

    if(!graphics)
        return InvalidParameter;

    graphics->textcontrast = contrast;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetTextRenderingHint(GpGraphics *graphics,
    TextRenderingHint hint)
{
    TRACE("(%p, %d)\n", graphics, hint);

    if(!graphics || hint > TextRenderingHintClearTypeGridFit)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(graphics->texthint == hint)
        return Ok;

    if (is_metafile_graphics(graphics)) {
        GpStatus stat;

        stat = METAFILE_AddSimpleProperty((GpMetafile*)graphics->image,
                EmfPlusRecordTypeSetTextRenderingHint, hint);
        if(stat != Ok)
            return stat;
    }

    graphics->texthint = hint;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetWorldTransform(GpGraphics *graphics, GpMatrix *matrix)
{
    GpStatus stat;

    TRACE("(%p, %s)\n", graphics, debugstr_matrix(matrix));

    if(!graphics || !matrix)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics)) {
        stat = METAFILE_SetWorldTransform((GpMetafile*)graphics->image, matrix);

        if (stat != Ok)
            return stat;
    }

    graphics->worldtrans = *matrix;

    return Ok;
}

GpStatus WINGDIPAPI GdipTranslateWorldTransform(GpGraphics *graphics, REAL dx,
    REAL dy, GpMatrixOrder order)
{
    GpStatus stat;

    TRACE("(%p, %.2f, %.2f, %d)\n", graphics, dx, dy, order);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics)) {
        stat = METAFILE_TranslateWorldTransform((GpMetafile*)graphics->image, dx, dy, order);

        if (stat != Ok)
            return stat;
    }

    return GdipTranslateMatrix(&graphics->worldtrans, dx, dy, order);
}

/*****************************************************************************
 * GdipSetClipHrgn [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipSetClipHrgn(GpGraphics *graphics, HRGN hrgn, CombineMode mode)
{
    GpRegion *region;
    GpStatus status;
    GpMatrix transform;

    TRACE("(%p, %p, %d)\n", graphics, hrgn, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    /* hrgn is in gdi32 device units */
    status = GdipCreateRegionHrgn(hrgn, &region);

    if (status == Ok)
    {
        status = get_graphics_transform(graphics, CoordinateSpaceDevice, WineCoordinateSpaceGdiDevice, &transform);

        if (status == Ok)
            status = GdipTransformRegion(region, &transform);

        if (status == Ok)
            status = GdipCombineRegionRegion(graphics->clip, region, mode);

        GdipDeleteRegion(region);
    }
    return status;
}

GpStatus WINGDIPAPI GdipSetClipPath(GpGraphics *graphics, GpPath *path, CombineMode mode)
{
    GpStatus status;
    GpPath *clip_path;

    TRACE("(%p, %p, %d)\n", graphics, path, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        status = METAFILE_SetClipPath((GpMetafile*)graphics->image, path, mode);
        if (status != Ok)
            return status;
    }

    status = GdipClonePath(path, &clip_path);
    if (status == Ok)
    {
        GpMatrix world_to_device;

        get_graphics_transform(graphics, CoordinateSpaceDevice,
                               CoordinateSpaceWorld, &world_to_device);
        status = GdipTransformPath(clip_path, &world_to_device);
        if (status == Ok)
            GdipCombineRegionPath(graphics->clip, clip_path, mode);

        GdipDeletePath(clip_path);
    }
    return status;
}

GpStatus WINGDIPAPI GdipSetClipRect(GpGraphics *graphics, REAL x, REAL y,
                                    REAL width, REAL height,
                                    CombineMode mode)
{
    GpStatus status;
    GpRectF rect;
    GpRegion *region;

    TRACE("(%p, %.2f, %.2f, %.2f, %.2f, %d)\n", graphics, x, y, width, height, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        status = METAFILE_SetClipRect((GpMetafile*)graphics->image, x, y, width, height, mode);
        if (status != Ok)
            return status;
    }

    set_rect(&rect, x, y, width, height);
    status = GdipCreateRegionRect(&rect, &region);
    if (status == Ok)
    {
        GpMatrix world_to_device;
        BOOL identity;

        get_graphics_transform(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, &world_to_device);
        status = GdipIsMatrixIdentity(&world_to_device, &identity);
        if (status == Ok && !identity)
            status = GdipTransformRegion(region, &world_to_device);
        if (status == Ok)
            status = GdipCombineRegionRegion(graphics->clip, region, mode);

        GdipDeleteRegion(region);
    }
    return status;
}

GpStatus WINGDIPAPI GdipSetClipRectI(GpGraphics *graphics, INT x, INT y,
                                     INT width, INT height,
                                     CombineMode mode)
{
    TRACE("(%p, %d, %d, %d, %d, %d)\n", graphics, x, y, width, height, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipSetClipRect(graphics, (REAL)x, (REAL)y, (REAL)width, (REAL)height, mode);
}

GpStatus WINGDIPAPI GdipSetClipRegion(GpGraphics *graphics, GpRegion *region,
                                      CombineMode mode)
{
    GpStatus status;
    GpRegion *clip;

    TRACE("(%p, %p, %d)\n", graphics, region, mode);

    if(!graphics || !region)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        status = METAFILE_SetClipRegion((GpMetafile*)graphics->image, region, mode);
        if (status != Ok)
            return status;
    }

    status = GdipCloneRegion(region, &clip);
    if (status == Ok)
    {
        GpMatrix world_to_device;
        BOOL identity;

        get_graphics_transform(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, &world_to_device);
        status = GdipIsMatrixIdentity(&world_to_device, &identity);
        if (status == Ok && !identity)
            status = GdipTransformRegion(clip, &world_to_device);
        if (status == Ok)
            status = GdipCombineRegionRegion(graphics->clip, clip, mode);

        GdipDeleteRegion(clip);
    }
    return status;
}

GpStatus WINGDIPAPI GdipDrawPolygon(GpGraphics *graphics,GpPen *pen,GDIPCONST GpPointF *points,
    INT count)
{
    GpStatus status;
    GpPath* path;

    TRACE("(%p, %p, %d)\n", graphics, points, count);

    if(!graphics || !pen || count<=0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    status = GdipCreatePath(FillModeAlternate, &path);
    if (status != Ok) return status;

    status = GdipAddPathPolygon(path, points, count);
    if (status == Ok)
        status = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);

    return status;
}

GpStatus WINGDIPAPI GdipDrawPolygonI(GpGraphics *graphics,GpPen *pen,GDIPCONST GpPoint *points,
    INT count)
{
    GpStatus ret;
    GpPointF *ptf;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(count <= 0) return InvalidParameter;
    ptf = malloc(sizeof(GpPointF) * count);
    if (!ptf) return OutOfMemory;

    for(i = 0;i < count; i++){
        ptf[i].X = (REAL)points[i].X;
        ptf[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawPolygon(graphics,pen,ptf,count);
    free(ptf);

    return ret;
}

GpStatus WINGDIPAPI GdipGetDpiX(GpGraphics *graphics, REAL* dpi)
{
    TRACE("(%p, %p)\n", graphics, dpi);

    if(!graphics || !dpi)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *dpi = graphics->xres;
    return Ok;
}

GpStatus WINGDIPAPI GdipGetDpiY(GpGraphics *graphics, REAL* dpi)
{
    TRACE("(%p, %p)\n", graphics, dpi);

    if(!graphics || !dpi)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *dpi = graphics->yres;
    return Ok;
}

GpStatus WINGDIPAPI GdipMultiplyWorldTransform(GpGraphics *graphics, GDIPCONST GpMatrix *matrix,
    GpMatrixOrder order)
{
    GpMatrix m;
    GpStatus ret;

    TRACE("(%p, %s, %d)\n", graphics, debugstr_matrix(matrix), order);

    if(!graphics || !matrix)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        ret = METAFILE_MultiplyWorldTransform((GpMetafile*)graphics->image, matrix, order);

        if (ret != Ok)
            return ret;
    }

    m = graphics->worldtrans;

    ret = GdipMultiplyMatrix(&m, matrix, order);
    if(ret == Ok)
        graphics->worldtrans = m;

    return ret;
}

/* Color used to fill bitmaps so we can tell which parts have been drawn over by gdi32. */
static const COLORREF DC_BACKGROUND_KEY = 0x0d0b0c;

GpStatus WINGDIPAPI GdipGetDC(GpGraphics *graphics, HDC *hdc)
{
    GpStatus stat=Ok;

    TRACE("(%p, %p)\n", graphics, hdc);

    if(!graphics || !hdc)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_GetDC((GpMetafile*)graphics->image, hdc);
    }
    else if (graphics->owndc)
    {
        graphics->temp_hdc = GetDC(graphics->hwnd);
        if (!graphics->temp_hdc)
            return OutOfMemory;
        *hdc = graphics->temp_hdc;
    }
    else if (!graphics->hdc ||
        (graphics->image && graphics->image->type == ImageTypeBitmap))
    {
        /* Create a fake HDC and fill it with a constant color. */
        HDC temp_hdc;
        HBITMAP hbitmap;
        GpRectF bounds;
        BITMAPINFOHEADER bmih;
        int i;

        stat = get_graphics_bounds(graphics, &bounds);
        if (stat != Ok)
            return stat;

        graphics->temp_hbitmap_width = bounds.Width;
        graphics->temp_hbitmap_height = bounds.Height;

        bmih.biSize = sizeof(bmih);
        bmih.biWidth = graphics->temp_hbitmap_width;
        bmih.biHeight = -graphics->temp_hbitmap_height;
        bmih.biPlanes = 1;
        bmih.biBitCount = 32;
        bmih.biCompression = BI_RGB;
        bmih.biSizeImage = 0;
        bmih.biXPelsPerMeter = 0;
        bmih.biYPelsPerMeter = 0;
        bmih.biClrUsed = 0;
        bmih.biClrImportant = 0;

        hbitmap = CreateDIBSection(NULL, (BITMAPINFO*)&bmih, DIB_RGB_COLORS,
            (void**)&graphics->temp_bits, NULL, 0);
        if (!hbitmap)
            return GenericError;

        if (!graphics->temp_hdc)
        {
            temp_hdc = CreateCompatibleDC(0);
        }
        else
        {
            temp_hdc = graphics->temp_hdc;
        }

        if (!temp_hdc)
        {
            DeleteObject(hbitmap);
            return GenericError;
        }

        for (i=0; i<(graphics->temp_hbitmap_width * graphics->temp_hbitmap_height); i++)
            ((DWORD*)graphics->temp_bits)[i] = DC_BACKGROUND_KEY;

        SelectObject(temp_hdc, hbitmap);

        graphics->temp_hbitmap = hbitmap;
        *hdc = graphics->temp_hdc = temp_hdc;
    }
    else
    {
        *hdc = graphics->hdc;
    }

    if (stat == Ok)
        graphics->busy = TRUE;

    return stat;
}

GpStatus WINGDIPAPI GdipReleaseDC(GpGraphics *graphics, HDC hdc)
{
    GpStatus stat=Ok;

    TRACE("(%p, %p)\n", graphics, hdc);

    if(!graphics || !hdc || !graphics->busy)
        return InvalidParameter;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_ReleaseDC((GpMetafile*)graphics->image, hdc);
    }
    else if (graphics->owndc)
    {
        ReleaseDC(graphics->hwnd, graphics->temp_hdc);
        graphics->temp_hdc = NULL;
    }
    else if (graphics->temp_hdc == hdc)
    {
        DWORD* pos;
        int i;

        /* Find the pixels that have changed, and mark them as opaque. */
        pos = (DWORD*)graphics->temp_bits;
        for (i=0; i<(graphics->temp_hbitmap_width * graphics->temp_hbitmap_height); i++)
        {
            if (*pos != DC_BACKGROUND_KEY)
            {
                *pos |= 0xff000000;
            }
            pos++;
        }

        /* Write the changed pixels to the real target. */
        alpha_blend_pixels(graphics, 0, 0, graphics->temp_bits,
            graphics->temp_hbitmap_width, graphics->temp_hbitmap_height,
            graphics->temp_hbitmap_width * 4, PixelFormat32bppARGB);

        /* Clean up. */
        DeleteObject(graphics->temp_hbitmap);
        graphics->temp_hbitmap = NULL;
    }
    else if (hdc != graphics->hdc)
    {
        stat = InvalidParameter;
    }

    if (stat == Ok)
        graphics->busy = FALSE;

    return stat;
}

GpStatus WINGDIPAPI GdipGetClip(GpGraphics *graphics, GpRegion *region)
{
    GpRegion *clip;
    GpStatus status;
    GpMatrix device_to_world;

    TRACE("(%p, %p)\n", graphics, region);

    if(!graphics || !region)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if((status = GdipCloneRegion(graphics->clip, &clip)) != Ok)
        return status;

    get_graphics_transform(graphics, CoordinateSpaceWorld, CoordinateSpaceDevice, &device_to_world);
    status = GdipTransformRegion(clip, &device_to_world);
    if (status != Ok)
    {
        GdipDeleteRegion(clip);
        return status;
    }

    /* free everything except root node and header */
    delete_element(&region->node);
    memcpy(region, clip, sizeof(GpRegion));
    free(clip);

    return Ok;
}

GpStatus gdi_transform_acquire(GpGraphics *graphics)
{
    if (graphics->gdi_transform_acquire_count == 0 && has_gdi_dc(graphics))
    {
        HDC hdc;
        GpStatus stat;

        stat = gdi_dc_acquire(graphics, &hdc);
        if (stat != Ok)
            return stat;

        graphics->gdi_transform_save = SaveDC(hdc);
        ModifyWorldTransform(hdc, NULL, MWT_IDENTITY);
        SetGraphicsMode(hdc, GM_COMPATIBLE);
        SetMapMode(hdc, MM_TEXT);
        SetWindowOrgEx(hdc, 0, 0, NULL);
        SetViewportOrgEx(hdc, 0, 0, NULL);
    }
    graphics->gdi_transform_acquire_count++;
    return Ok;
}

GpStatus gdi_transform_release(GpGraphics *graphics)
{
    if (graphics->gdi_transform_acquire_count <= 0)
    {
        ERR("called without matching gdi_transform_acquire\n");
        return GenericError;
    }
    if (graphics->gdi_transform_acquire_count == 1 && graphics->hdc)
    {
        RestoreDC(graphics->hdc, graphics->gdi_transform_save);
        gdi_dc_release(graphics, graphics->hdc);
    }
    graphics->gdi_transform_acquire_count--;
    return Ok;
}

GpStatus get_graphics_transform(GpGraphics *graphics, GpCoordinateSpace dst_space,
        GpCoordinateSpace src_space, GpMatrix *matrix)
{
    GpStatus stat = Ok;
    REAL scale_x, scale_y;

    GdipSetMatrixElements(matrix, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);

    if (dst_space != src_space)
    {
        if(graphics->unit != UnitDisplay)
        {
            scale_x = units_to_pixels(graphics->scale, graphics->unit, graphics->xres, graphics->printer_display);
            scale_y = units_to_pixels(graphics->scale, graphics->unit, graphics->yres, graphics->printer_display);
        }
        else
        {
            scale_x = units_to_pixels(1.0, graphics->unit, graphics->xres, graphics->printer_display);
            scale_y = units_to_pixels(1.0, graphics->unit, graphics->yres, graphics->printer_display);
        }

        if (dst_space < src_space)
        {
            /* transform towards world space */
            switch ((int)src_space)
            {
            case WineCoordinateSpaceGdiDevice:
            {
                GpMatrix gdixform = graphics->gdi_transform;
                stat = GdipInvertMatrix(&gdixform);
                if (stat != Ok)
                    break;
                memcpy(matrix->matrix, gdixform.matrix, sizeof(matrix->matrix));
                if (dst_space == CoordinateSpaceDevice)
                    break;
                /* else fall-through */
            }
            case CoordinateSpaceDevice:
                GdipScaleMatrix(matrix, 1.0/scale_x, 1.0/scale_y, MatrixOrderAppend);
                if (dst_space == CoordinateSpacePage)
                    break;
                /* else fall-through */
            case CoordinateSpacePage:
            {
                GpMatrix inverted_transform = graphics->worldtrans;
                stat = GdipInvertMatrix(&inverted_transform);
                if (stat == Ok)
                    GdipMultiplyMatrix(matrix, &inverted_transform, MatrixOrderAppend);
                break;
            }
            }
        }
        else
        {
            /* transform towards device space */
            switch ((int)src_space)
            {
            case CoordinateSpaceWorld:
                memcpy(matrix->matrix, &graphics->worldtrans, sizeof(matrix->matrix));
                if (dst_space == CoordinateSpacePage)
                    break;
                /* else fall-through */
            case CoordinateSpacePage:
                GdipScaleMatrix(matrix, scale_x, scale_y, MatrixOrderAppend);
                if (dst_space == CoordinateSpaceDevice)
                    break;
                /* else fall-through */
            case CoordinateSpaceDevice:
            {
                GdipMultiplyMatrix(matrix, &graphics->gdi_transform, MatrixOrderAppend);
                break;
            }
            }
        }
    }
    return stat;
}

GpStatus gdip_transform_points(GpGraphics *graphics, GpCoordinateSpace dst_space,
                               GpCoordinateSpace src_space, GpPointF *points, INT count)
{
    GpMatrix matrix;
    GpStatus stat;

    stat = get_graphics_transform(graphics, dst_space, src_space, &matrix);
    if (stat != Ok) return stat;

    return GdipTransformMatrixPoints(&matrix, points, count);
}

GpStatus WINGDIPAPI GdipTransformPoints(GpGraphics *graphics, GpCoordinateSpace dst_space,
                                        GpCoordinateSpace src_space, GpPointF *points, INT count)
{
    if(!graphics || !points || count <= 0 || (UINT)dst_space > CoordinateSpaceDevice ||
       (UINT)src_space > CoordinateSpaceDevice)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    TRACE("(%p, %d, %d, %p, %d)\n", graphics, dst_space, src_space, points, count);

    if (src_space == dst_space) return Ok;

    return gdip_transform_points(graphics, dst_space, src_space, points, count);
}

GpStatus WINGDIPAPI GdipTransformPointsI(GpGraphics *graphics, GpCoordinateSpace dst_space,
                                         GpCoordinateSpace src_space, GpPoint *points, INT count)
{
    GpPointF *pointsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %d, %d, %p, %d)\n", graphics, dst_space, src_space, points, count);

    if(count <= 0)
        return InvalidParameter;

    pointsF = malloc(sizeof(GpPointF) * count);
    if(!pointsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    ret = GdipTransformPoints(graphics, dst_space, src_space, pointsF, count);

    if(ret == Ok)
        for(i = 0; i < count; i++){
            points[i].X = gdip_round(pointsF[i].X);
            points[i].Y = gdip_round(pointsF[i].Y);
        }
    free(pointsF);

    return ret;
}

HPALETTE WINGDIPAPI GdipCreateHalftonePalette(void)
{
    static int calls;

    TRACE("\n");

    if (!calls++)
      FIXME("stub\n");

    return NULL;
}

/*****************************************************************************
 * GdipTranslateClip [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipTranslateClip(GpGraphics *graphics, REAL dx, REAL dy)
{
    GpStatus stat;

    TRACE("(%p, %.2f, %.2f)\n", graphics, dx, dy);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_OffsetClip((GpMetafile *)graphics->image, dx, dy);
        if (stat != Ok)
            return stat;
    }

    return GdipTranslateRegion(graphics->clip, dx, dy);
}

/*****************************************************************************
 * GdipTranslateClipI [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipTranslateClipI(GpGraphics *graphics, INT dx, INT dy)
{
    TRACE("(%p, %d, %d)\n", graphics, dx, dy);

    return GdipTranslateClip(graphics, dx, dy);
}

/*****************************************************************************
 * GdipMeasureDriverString [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipMeasureDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                            GDIPCONST GpFont *font, GDIPCONST PointF *positions,
                                            INT flags, GDIPCONST GpMatrix *matrix, RectF *boundingBox)
{
    static const INT unsupported_flags = ~(DriverStringOptionsCmapLookup|DriverStringOptionsRealizedAdvance);
    HFONT hfont;
    HDC hdc;
    REAL min_x, min_y, max_x, max_y, x, y;
    int i;
    TEXTMETRICW textmetric;
    const WORD *glyph_indices;
    WORD *dynamic_glyph_indices=NULL;
    REAL rel_width, rel_height, ascent, descent;
    GpPointF pt[3];

    TRACE("(%p %p %d %p %p %d %s %p)\n", graphics, text, length, font, positions, flags, debugstr_matrix(matrix), boundingBox);

    if (!graphics || !text || !font || !positions || !boundingBox)
        return InvalidParameter;

    if (length == -1)
        length = lstrlenW(text);

    if (length == 0)
        set_rect(boundingBox, 0.0f, 0.0f, 0.0f, 0.0f);

    if (flags & unsupported_flags)
        FIXME("Ignoring flags %x\n", flags & unsupported_flags);

    get_font_hfont(graphics, font, NULL, &hfont, NULL, matrix);

    hdc = CreateCompatibleDC(0);
    SelectObject(hdc, hfont);

    GetTextMetricsW(hdc, &textmetric);

    pt[0].X = 0.0;
    pt[0].Y = 0.0;
    pt[1].X = 1.0;
    pt[1].Y = 0.0;
    pt[2].X = 0.0;
    pt[2].Y = 1.0;
    if (matrix)
    {
        GpMatrix xform = *matrix;
        GdipTransformMatrixPoints(&xform, pt, 3);
    }
    gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, pt, 3);
    rel_width = hypotf(pt[1].Y - pt[0].Y, pt[1].X - pt[0].X);
    rel_height = hypotf(pt[2].Y - pt[0].Y, pt[2].X - pt[0].X);

    if (flags & DriverStringOptionsCmapLookup)
    {
        glyph_indices = dynamic_glyph_indices = malloc(sizeof(WORD) * length);
        if (!glyph_indices)
        {
            DeleteDC(hdc);
            DeleteObject(hfont);
            return OutOfMemory;
        }

        GetGlyphIndicesW(hdc, text, length, dynamic_glyph_indices, 0);
    }
    else
        glyph_indices = text;

    min_x = max_x = x = positions[0].X;
    min_y = max_y = y = positions[0].Y;

    ascent = textmetric.tmAscent / rel_height;
    descent = textmetric.tmDescent / rel_height;

    for (i=0; i<length; i++)
    {
        int char_width;
        ABC abc;

        if (!(flags & DriverStringOptionsRealizedAdvance))
        {
            x = positions[i].X;
            y = positions[i].Y;
        }

        GetCharABCWidthsW(hdc, glyph_indices[i], glyph_indices[i], &abc);
        char_width = abc.abcA + abc.abcB + abc.abcC;

        if (min_y > y - ascent) min_y = y - ascent;
        if (max_y < y + descent) max_y = y + descent;
        if (min_x > x) min_x = x;

        x += char_width / rel_width;

        if (max_x < x) max_x = x;
    }

    free(dynamic_glyph_indices);
    DeleteDC(hdc);
    DeleteObject(hfont);

    boundingBox->X = min_x;
    boundingBox->Y = min_y;
    boundingBox->Width = max_x - min_x;
    boundingBox->Height = max_y - min_y;

    return Ok;
}

static GpStatus GDI32_GdipDrawDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                           GDIPCONST GpFont *font, GDIPCONST GpStringFormat *format,
                                           GDIPCONST GpBrush *brush, GDIPCONST PointF *positions,
                                           INT flags, GDIPCONST GpMatrix *matrix)
{
    INT save_state;
    GpPointF pt, *real_positions=NULL;
    INT *eto_positions=NULL;
    HFONT hfont;
    LOGFONTW lfw;
    UINT eto_flags=0;
    GpStatus status;
    HDC hdc;
    HRGN hrgn;

    if (!(flags & DriverStringOptionsCmapLookup))
        eto_flags |= ETO_GLYPH_INDEX;

    if (!(flags & DriverStringOptionsRealizedAdvance) && length > 1)
    {
        real_positions = malloc(sizeof(*real_positions) * length);
        eto_positions = malloc(sizeof(*eto_positions) * 2 * (length - 1));
        if (!real_positions || !eto_positions)
        {
            free(real_positions);
            free(eto_positions);
            return OutOfMemory;
        }
    }

    status = gdi_dc_acquire(graphics, &hdc);
    if (status != Ok)
    {
        free(real_positions);
        free(eto_positions);
        return status;
    }

    save_state = SaveDC(hdc);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, get_gdi_brush_color(brush));

    status = get_clip_hrgn(graphics, &hrgn);

    if (status == Ok)
    {
        ExtSelectClipRgn(hdc, hrgn, RGN_COPY);
        DeleteObject(hrgn);
    }

    pt = positions[0];
    gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, &pt, 1);

    get_font_hfont(graphics, font, format, &hfont, &lfw, matrix);

    if (!(flags & DriverStringOptionsRealizedAdvance) && length > 1)
    {
        GpMatrix rotation;
        INT i;

        eto_flags |= ETO_PDY;

        memcpy(real_positions, positions, sizeof(PointF) * length);

        gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, real_positions, length);

        GdipSetMatrixElements(&rotation, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
        GdipRotateMatrix(&rotation, lfw.lfEscapement / 10.0, MatrixOrderAppend);
        GdipTransformMatrixPoints(&rotation, real_positions, length);

        for (i = 0; i < (length - 1); i++)
        {
            eto_positions[i*2] = gdip_round(real_positions[i+1].X) - gdip_round(real_positions[i].X);
            eto_positions[i*2+1] = gdip_round(real_positions[i].Y) - gdip_round(real_positions[i+1].Y);
        }
    }

    SelectObject(hdc, hfont);

    SetTextAlign(hdc, TA_BASELINE|TA_LEFT);

    gdi_transform_acquire(graphics);

    ExtTextOutW(hdc, gdip_round(pt.X), gdip_round(pt.Y), eto_flags, NULL, text, length, eto_positions);

    gdi_transform_release(graphics);

    RestoreDC(hdc, save_state);

    DeleteObject(hfont);

    free(real_positions);
    free(eto_positions);

    gdi_dc_release(graphics, hdc);

    return Ok;
}

static GpStatus SOFTWARE_GdipDrawDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                        GDIPCONST GpFont *font, GDIPCONST GpStringFormat *format,
                                        GDIPCONST GpBrush *brush, GDIPCONST PointF *positions,
                                        INT flags, GDIPCONST GpMatrix *matrix)
{
    static const INT unsupported_flags = ~(DriverStringOptionsCmapLookup|DriverStringOptionsRealizedAdvance);
    GpStatus stat;
    PointF *real_positions, real_position;
    POINT *pti;
    HFONT hfont;
    HDC hdc;
    int min_x=INT_MAX, min_y=INT_MAX, max_x=INT_MIN, max_y=INT_MIN, i, x, y;
    DWORD max_glyphsize=0;
    GLYPHMETRICS glyphmetrics;
    static const MAT2 identity = {{0,1}, {0,0}, {0,0}, {0,1}};
    BYTE *glyph_mask;
    BYTE *text_mask;
    int text_mask_stride;
    BYTE *pixel_data;
    int pixel_data_stride;
    GpRect pixel_area;
    UINT ggo_flags = GGO_GRAY8_BITMAP;

    if (length <= 0)
        return Ok;

    if (!(flags & DriverStringOptionsCmapLookup))
        ggo_flags |= GGO_GLYPH_INDEX;

    if (flags & unsupported_flags)
        FIXME("Ignoring flags %x\n", flags & unsupported_flags);

    pti = malloc(sizeof(POINT) * length);
    if (!pti)
        return OutOfMemory;

    if (flags & DriverStringOptionsRealizedAdvance)
    {
        real_position = positions[0];

        gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, &real_position, 1);
        round_points(pti, &real_position, 1);
    }
    else
    {
        real_positions = malloc(sizeof(PointF) * length);
        if (!real_positions)
        {
            free(pti);
            return OutOfMemory;
        }

        memcpy(real_positions, positions, sizeof(PointF) * length);

        gdip_transform_points(graphics, WineCoordinateSpaceGdiDevice, CoordinateSpaceWorld, real_positions, length);
        round_points(pti, real_positions, length);

        free(real_positions);
    }

    get_font_hfont(graphics, font, format, &hfont, NULL, matrix);

    hdc = CreateCompatibleDC(0);
    SelectObject(hdc, hfont);

    /* Get the boundaries of the text to be drawn */
    for (i=0; i<length; i++)
    {
        DWORD glyphsize;
        int left, top, right, bottom;

        glyphsize = GetGlyphOutlineW(hdc, text[i], ggo_flags,
            &glyphmetrics, 0, NULL, &identity);

        if (glyphsize == GDI_ERROR)
        {
            ERR("GetGlyphOutlineW failed\n");
            free(pti);
            DeleteDC(hdc);
            DeleteObject(hfont);
            return GenericError;
        }

        if (glyphsize > max_glyphsize)
            max_glyphsize = glyphsize;

        if (glyphsize != 0)
        {
            left = pti[i].x + glyphmetrics.gmptGlyphOrigin.x;
            top = pti[i].y - glyphmetrics.gmptGlyphOrigin.y;
            right = pti[i].x + glyphmetrics.gmptGlyphOrigin.x + glyphmetrics.gmBlackBoxX;
            bottom = pti[i].y - glyphmetrics.gmptGlyphOrigin.y + glyphmetrics.gmBlackBoxY;

            if (left < min_x) min_x = left;
            if (top < min_y) min_y = top;
            if (right > max_x) max_x = right;
            if (bottom > max_y) max_y = bottom;
        }

        if (i+1 < length && (flags & DriverStringOptionsRealizedAdvance) == DriverStringOptionsRealizedAdvance)
        {
            pti[i+1].x = pti[i].x + glyphmetrics.gmCellIncX;
            pti[i+1].y = pti[i].y + glyphmetrics.gmCellIncY;
        }
    }

    if (max_glyphsize == 0)
    {
        /* Nothing to draw. */
        free(pti);
        DeleteDC(hdc);
        DeleteObject(hfont);
        return Ok;
    }

    glyph_mask = calloc(1, max_glyphsize);
    text_mask = calloc(1, (max_x - min_x) * (max_y - min_y));
    text_mask_stride = max_x - min_x;

    if (!(glyph_mask && text_mask))
    {
        free(glyph_mask);
        free(text_mask);
        free(pti);
        DeleteDC(hdc);
        DeleteObject(hfont);
        return OutOfMemory;
    }

    /* Generate a mask for the text */
    for (i=0; i<length; i++)
    {
        DWORD ret;
        int left, top, stride;

        ret = GetGlyphOutlineW(hdc, text[i], ggo_flags,
            &glyphmetrics, max_glyphsize, glyph_mask, &identity);

        if (ret == GDI_ERROR || ret == 0)
            continue; /* empty glyph */

        left = pti[i].x + glyphmetrics.gmptGlyphOrigin.x;
        top = pti[i].y - glyphmetrics.gmptGlyphOrigin.y;
        stride = (glyphmetrics.gmBlackBoxX + 3) & (~3);

        for (y=0; y<glyphmetrics.gmBlackBoxY; y++)
        {
            BYTE *glyph_val = glyph_mask + y * stride;
            BYTE *text_val = text_mask + (left - min_x) + (top - min_y + y) * text_mask_stride;
            for (x=0; x<glyphmetrics.gmBlackBoxX; x++)
            {
                *text_val = min(64, *text_val + *glyph_val);
                glyph_val++;
                text_val++;
            }
        }
    }

    free(pti);
    DeleteDC(hdc);
    DeleteObject(hfont);
    free(glyph_mask);

    /* get the brush data */
    pixel_data = calloc((max_x - min_x) * (max_y - min_y), 4);
    if (!pixel_data)
    {
        free(text_mask);
        return OutOfMemory;
    }

    pixel_area.X = min_x;
    pixel_area.Y = min_y;
    pixel_area.Width = max_x - min_x;
    pixel_area.Height = max_y - min_y;
    pixel_data_stride = pixel_area.Width * 4;

    stat = brush_fill_pixels(graphics, (GpBrush*)brush, (DWORD*)pixel_data, &pixel_area, pixel_area.Width);
    if (stat != Ok)
    {
        free(text_mask);
        free(pixel_data);
        return stat;
    }

    /* multiply the brush data by the mask */
    for (y=0; y<pixel_area.Height; y++)
    {
        BYTE *text_val = text_mask + text_mask_stride * y;
        BYTE *pixel_val = pixel_data + pixel_data_stride * y + 3;
        for (x=0; x<pixel_area.Width; x++)
        {
            *pixel_val = (*pixel_val) * (*text_val) / 64;
            text_val++;
            pixel_val+=4;
        }
    }

    free(text_mask);

    gdi_transform_acquire(graphics);

    /* draw the result */
    stat = alpha_blend_pixels(graphics, min_x, min_y, pixel_data, pixel_area.Width,
        pixel_area.Height, pixel_data_stride, PixelFormat32bppARGB);

    gdi_transform_release(graphics);

    free(pixel_data);

    return stat;
}

static GpStatus draw_driver_string(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                   GDIPCONST GpFont *font, GDIPCONST GpStringFormat *format,
                                   GDIPCONST GpBrush *brush, GDIPCONST PointF *positions,
                                   INT flags, GDIPCONST GpMatrix *matrix)
{
    GpStatus stat = NotImplemented;

    if (length == -1)
        length = lstrlenW(text);

    if (is_metafile_graphics(graphics))
        return METAFILE_DrawDriverString((GpMetafile*)graphics->image, text, length, font,
            format, brush, positions, flags, matrix);

    if (has_gdi_dc(graphics) && !graphics->alpha_hdc &&
        brush->bt == BrushTypeSolidColor &&
        (((GpSolidFill*)brush)->color & 0xff000000) == 0xff000000)
        stat = GDI32_GdipDrawDriverString(graphics, text, length, font, format,
                                          brush, positions, flags, matrix);
    if (stat == NotImplemented)
        stat = SOFTWARE_GdipDrawDriverString(graphics, text, length, font, format,
                                             brush, positions, flags, matrix);
    return stat;
}

/*****************************************************************************
 * GdipDrawDriverString [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipDrawDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                         GDIPCONST GpFont *font, GDIPCONST GpBrush *brush,
                                         GDIPCONST PointF *positions, INT flags,
                                         GDIPCONST GpMatrix *matrix )
{
    TRACE("(%p %s %p %p %p %d %s)\n", graphics, debugstr_wn(text, length), font, brush, positions, flags, debugstr_matrix(matrix));

    if (!graphics || !text || !font || !brush || !positions)
        return InvalidParameter;

    return draw_driver_string(graphics, text, length, font, NULL,
                              brush, positions, flags, matrix);
}

/*****************************************************************************
 * GdipIsVisibleClipEmpty [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipIsVisibleClipEmpty(GpGraphics *graphics, BOOL *res)
{
    GpStatus stat;
    GpRegion* rgn;

    TRACE("(%p, %p)\n", graphics, res);

    if((stat = GdipCreateRegion(&rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, rgn)) != Ok)
        goto cleanup;

    stat = GdipIsEmptyRegion(rgn, graphics, res);

cleanup:
    GdipDeleteRegion(rgn);
    return stat;
}

GpStatus WINGDIPAPI GdipResetPageTransform(GpGraphics *graphics)
{
    GpStatus stat;

    TRACE("(%p)\n", graphics);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (is_metafile_graphics(graphics))
    {
        stat = METAFILE_SetPageTransform((GpMetafile*)graphics->image, UnitDisplay, 1.0);
        if (stat != Ok)
            return stat;
    }

    graphics->scale = 1.0;
    graphics->unit = UnitDisplay;

    return Ok;
}

GpStatus WINGDIPAPI GdipGraphicsSetAbort(GpGraphics *graphics, GdiplusAbort *pabort)
{
    TRACE("(%p, %p)\n", graphics, pabort);

    if (!graphics)
        return InvalidParameter;

    if (pabort)
        FIXME("Abort callback is not supported.\n");

    return Ok;
}
