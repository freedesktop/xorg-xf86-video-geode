/*
 * Copyright (c) 2007 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Neither the name of the Advanced Micro Devices, Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 */

/* TODO:
   Support a8 as a source or destination?
   convert !a8 or !a4 masks?
   support multiple pass operations?
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "amd.h"
#include "cim_defs.h"
#include "cim_regs.h"

#include "amd_blend.h"

static const struct exa_format_t {
  int exa;
  int bpp;
  int fmt;
  int alphabits;
} lx_exa_formats[] = {
  {PICT_a8r8g8b8, 32, CIMGP_SOURCE_FMT_8_8_8_8,   8 },
  {PICT_x8r8g8b8, 32, CIMGP_SOURCE_FMT_8_8_8_8,   0 },
  {PICT_x8b8g8r8, 32, CIMGP_SOURCE_FMT_32BPP_BGR, 0 },
  {PICT_a4r4g4b4, 16, CIMGP_SOURCE_FMT_4_4_4_4,   4 },
  {PICT_a1r5g5b5, 16, CIMGP_SOURCE_FMT_1_5_5_5,   1 },
  {PICT_r5g6b5,   16, CIMGP_SOURCE_FMT_0_5_6_5,   0 },
  {PICT_b5g6r5,   16, CIMGP_SOURCE_FMT_16BPP_BGR, 0 },
  {PICT_x1r5g5b5, 16, CIMGP_SOURCE_FMT_1_5_5_5,   0 },
  {PICT_x1b5g5r5, 16, CIMGP_SOURCE_FMT_15BPP_BGR, 0 },
  {PICT_r3g3b2,    8, CIMGP_SOURCE_FMT_3_3_2,     0 }
};

/* This is a chunk of memory we use for scratch space */

#define COMP_TYPE_MASK 0
#define COMP_TYPE_ONEPASS 1
#define COMP_TYPE_TWOPASS 3

static struct {
  int type;

  unsigned int srcOffset;
  unsigned int srcPitch;
  unsigned int srcBpp;
  unsigned int srcWidth, srcHeight;
  PixmapPtr srcPixmap;

  unsigned int srcColor;
  int op;
  int repeat;
  unsigned int fourBpp;
  unsigned int bufferOffset;
  struct exa_format_t *srcFormat;
  struct exa_format_t *dstFormat;
} exaScratch;

static const int SDfn[16] = {
    0x00, 0x88, 0x44, 0xCC, 0x22, 0xAA, 0x66, 0xEE,
    0x11, 0x99, 0x55, 0xDD, 0x33, 0xBB, 0x77, 0xFF
};

static const int SDfn_PM[16] = {
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
    0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA
};

/* These functions check to see if we can safely prefetch the memory
 * for the blt, or if we have to wait the previous blt to complete.
 * One function is for the fill, and the other is for the copy because
 * they have different requirements based on ROP
 */

static int lx0  = -1, ly0 = -1, lx1 = -1, ly1 = -1;

static int lx_fill_flags(int x0, int y0, int w, int h, int rop)
{
	int x1 = x0 + w, y1 = y0 + h;
	int n = ((rop^(rop>>1))&0x55) == 0 || /* no dst */
		x0 >= lx1 || y0 >= ly1 ||     /* rght/below */
		x1 <= lx0 || y1 <= ly0 ?      /* left/above */
		0 : CIMGP_BLTFLAGS_HAZARD;

	lx0 = x0;
	ly0 = y0;
	lx1 = x1;
	ly1 = y1;

	return n;
}

static int lx_copy_flags(int x0, int y0, int x1, int y1, int w, int h,
	int rop)
{
   int x2 = x1+w, y2 = y1+h;

   /* dst not hazzard and src not hazzard */
   int n = ( ((rop^(rop>>1))&0x55) == 0 ||
              x1 >= lx1 || y1 >= ly1 ||
              x2 <= lx0 || y2 <= ly0 ) &&
           ( ((rop^(rop>>2))&0x33) == 0 ||
              x0 >= lx1 || y0 >= ly1 ||
              x0+w <= lx0 || y0+h <= ly0 ) ?
       0 : CIMGP_BLTFLAGS_HAZARD;

   lx0 = x1;
   ly0 = y1;
   lx1 = x2;
   ly1 = y2;

   return n;
}

/* These are borrowed from the exa engine - they should be made global
   and available to drivers, but until then....
*/

/* exaGetPixelFromRGBA (exa_render.c) */

static Bool
_GetPixelFromRGBA(CARD32	*pixel,
		    CARD16	red,
		    CARD16	green,
		    CARD16	blue,
		    CARD16	alpha,
		    CARD32	format)
{
    int rbits, bbits, gbits, abits;
    int rshift, bshift, gshift, ashift;

    *pixel = 0;

    if (!PICT_FORMAT_COLOR(format))
	return FALSE;

    rbits = PICT_FORMAT_R(format);
    gbits = PICT_FORMAT_G(format);
    bbits = PICT_FORMAT_B(format);
    abits = PICT_FORMAT_A(format);

    if (PICT_FORMAT_TYPE(format) == PICT_TYPE_ARGB) {
	bshift = 0;
	gshift = bbits;
	rshift = gshift + gbits;
	ashift = rshift + rbits;
    } else {  /* PICT_TYPE_ABGR */
	rshift = 0;
	gshift = rbits;
	bshift = gshift + gbits;
	ashift = bshift + bbits;
    }

    *pixel |=  ( blue >> (16 - bbits)) << bshift;
    *pixel |=  (  red >> (16 - rbits)) << rshift;
    *pixel |=  (green >> (16 - gbits)) << gshift;
    *pixel |=  (alpha >> (16 - abits)) << ashift;

    return TRUE;
}

/* exaGetRGBAFromPixel (exa_render.c) */

static Bool
_GetRGBAFromPixel(CARD32	pixel,
		    CARD16	*red,
		    CARD16	*green,
		    CARD16	*blue,
		    CARD16	*alpha,
		    CARD32	format)
{
    int rbits, bbits, gbits, abits;
    int rshift, bshift, gshift, ashift;

    if (!PICT_FORMAT_COLOR(format))
	return FALSE;

    rbits = PICT_FORMAT_R(format);
    gbits = PICT_FORMAT_G(format);
    bbits = PICT_FORMAT_B(format);
    abits = PICT_FORMAT_A(format);

    if (PICT_FORMAT_TYPE(format) == PICT_TYPE_ARGB) {
	bshift = 0;
	gshift = bbits;
	rshift = gshift + gbits;
	ashift = rshift + rbits;
    } else {  /* PICT_TYPE_ABGR */
	rshift = 0;
	gshift = rbits;
	bshift = gshift + gbits;
	ashift = bshift + bbits;
    }

    *red = ((pixel >> rshift ) & ((1 << rbits) - 1)) << (16 - rbits);
    while (rbits < 16) {
	*red |= *red >> rbits;
	rbits <<= 1;
    }

    *green = ((pixel >> gshift ) & ((1 << gbits) - 1)) << (16 - gbits);
    while (gbits < 16) {
	*green |= *green >> gbits;
	gbits <<= 1;
    }

    *blue = ((pixel >> bshift ) & ((1 << bbits) - 1)) << (16 - bbits);
    while (bbits < 16) {
	*blue |= *blue >> bbits;
	bbits <<= 1;
    }

    if (abits) {
	*alpha = ((pixel >> ashift ) & ((1 << abits) - 1)) << (16 - abits);
	while (abits < 16) {
	    *alpha |= *alpha >> abits;
	    abits <<= 1;
	}
    } else
	*alpha = 0xffff;

    return TRUE;
}

static unsigned int lx_get_source_color(PicturePtr pSrc, int x, int y, int dstFormat)
{
  FbBits *bits;
  FbStride stride;
  int bpp, xoff, yoff;

  CARD32 in, out;
  CARD16 red, green, blue, alpha;

  fbGetDrawable (pSrc->pDrawable, bits, stride, bpp, xoff, yoff);
  
  bits += (y * stride) + (x * (bpp >> 3));

  /* Read the source value */

  switch(bpp) {
  case 32:
  case 24:    
    in = (CARD32) *((CARD32 *) bits);
    break;
    
  case 16:
    in = (CARD32) *((CARD16 *) bits);
    break;

  case 8:
    in = (CARD32) *((CARD8 *) bits);
    break;
  }

  _GetRGBAFromPixel(in, &red, &blue, &green, &alpha, pSrc->format);
  _GetPixelFromRGBA(&out, red, blue, green, alpha, dstFormat);

  return out;
}
 
static Bool
lx_prepare_solid(PixmapPtr pxMap, int alu, Pixel planemask, Pixel fg)
{
  int pitch = exaGetPixmapPitch(pxMap);
  int op = (planemask == ~0U) ? SDfn[alu] : SDfn_PM[alu];

  gp_declare_blt(0);
  gp_set_bpp(pxMap->drawable.bitsPerPixel);
  
  gp_set_raster_operation(op);

  if (planemask != ~0U)
    gp_set_solid_pattern(planemask);

  exaScratch.op = op;

  gp_set_solid_source(fg);
  gp_set_strides(pitch, pitch);
  gp_write_parameters();
  return TRUE;
}

static void
lx_do_solid(PixmapPtr pxMap, int x1, int y1, int x2, int y2)
{
  int bpp = (pxMap->drawable.bitsPerPixel + 7) / 8;
  int pitch = exaGetPixmapPitch(pxMap);
  unsigned int offset = exaGetPixmapOffset(pxMap) + (pitch * y1) + (bpp * x1);

  gp_declare_blt(lx_fill_flags(x1, y1,x2-x1,y2-y1, exaScratch.op));
  gp_pattern_fill(offset, x2-x1, y2-y1);
}

static Bool
lx_prepare_copy( PixmapPtr pxSrc, PixmapPtr pxDst, int dx, int dy,
		 int alu, Pixel planemask)
{
  int dpitch = exaGetPixmapPitch(pxDst);
  int op = (planemask == ~0U) ? SDfn[alu] : SDfn_PM[alu];

  gp_declare_blt(0);
  gp_set_bpp(pxDst->drawable.bitsPerPixel);

  gp_set_raster_operation(op);

  if (planemask != ~0U)
    gp_set_solid_pattern(planemask);

  exaScratch.srcOffset = exaGetPixmapOffset(pxSrc);
  exaScratch.srcPitch = exaGetPixmapPitch(pxSrc);
  exaScratch.srcBpp = (pxSrc->drawable.bitsPerPixel + 7) / 8;

  exaScratch.op = op;

  gp_set_strides(dpitch, exaScratch.srcPitch);
  gp_write_parameters();
  return TRUE;
}

static void
lx_do_copy(PixmapPtr pxDst, int srcX, int srcY,
	   int dstX, int dstY, int w, int h)
{
  int dstBpp = (pxDst->drawable.bitsPerPixel + 7) / 8;
  int dstPitch = exaGetPixmapPitch(pxDst);
  unsigned int srcOffset, dstOffset;
  int flags = 0;

  gp_declare_blt(lx_copy_flags(srcX, srcY, dstX, dstY,w,h, exaScratch.op));

  //gp_declare_blt(0);

  srcOffset = exaScratch.srcOffset + (exaScratch.srcPitch * srcY) +
    (exaScratch.srcBpp) * srcX;

  dstOffset = exaGetPixmapOffset(pxDst) + (dstPitch * dstY) +
    (dstBpp * dstX);

  flags = 0;

  if (dstX > srcX)
    flags |= CIMGP_NEGXDIR;

  if (dstY > srcY)
    flags |= CIMGP_NEGYDIR;

  gp_screen_to_screen_blt(dstOffset, srcOffset, w, h, flags);
}

/* Composite operations

These are the simplest - one pass operations - if there is no format or
mask, the we can make these happen pretty fast

                       Operation  Type  Channel   Alpha
PictOpClear            0          2     0         3
PictOpSrc              0          3     0         3
PictOpDst              0          3     1         3
PictOpOver             2          0     0         3
PictOpOverReverse      2          0     1         3
PictOpIn               0          1     0         3
PictOpInReverse        0          1     1         3
PictOpOut              1          0     0         3
PictOpOutReverse       1          0     1         3
PictOpAdd              2          2     0         3

The following require multiple passes
PictOpAtop
PictOpXor
*/

struct blend_ops_t {
  int operation;
  int type;
  int channel;
} lx_alpha_ops[] = {
  /* PictOpClear */
  { CIMGP_ALPHA_TIMES_A, CIMGP_CONSTANT_ALPHA, CIMGP_CHANNEL_A_SOURCE }, { },
  /* PictOpSrc */
  { CIMGP_ALPHA_TIMES_A, CIMGP_ALPHA_EQUALS_ONE, CIMGP_CHANNEL_A_SOURCE }, { },
  /* PictOpDst */
  { CIMGP_ALPHA_TIMES_A, CIMGP_ALPHA_EQUALS_ONE, CIMGP_CHANNEL_A_DEST }, { },
  /* PictOpOver*/
  { CIMGP_ALPHA_A_PLUS_BETA_B, CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_SOURCE },
  { },
  /* PictOpOverReverse */
  { CIMGP_ALPHA_A_PLUS_BETA_B, CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_DEST }, 
  { },
  /* PictOpIn */
  { CIMGP_ALPHA_TIMES_A, CIMGP_CHANNEL_B_ALPHA, CIMGP_CHANNEL_A_SOURCE }, { },
  /* PictOpInReverse */
  { CIMGP_ALPHA_TIMES_A, CIMGP_CHANNEL_B_ALPHA, CIMGP_CHANNEL_A_DEST }, { },
  /* PictOpOut */
  { CIMGP_BETA_TIMES_B, CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_SOURCE }, { },
  /* PictOpOutReverse */
  { CIMGP_BETA_TIMES_B, CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_DEST }, { },
  /* SrcAtop */
  { CIMGP_ALPHA_TIMES_A, CIMGP_CHANNEL_B_ALPHA, CIMGP_CHANNEL_A_DEST },
  { CIMGP_BETA_TIMES_B,  CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_SOURCE },
  /* SrcAtopReverse */
  { CIMGP_ALPHA_TIMES_A, CIMGP_CHANNEL_B_ALPHA, CIMGP_CHANNEL_A_SOURCE },
  { CIMGP_BETA_TIMES_B,  CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_DEST },
  /* Xor */
  { CIMGP_BETA_TIMES_B, CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_SOURCE },
  { CIMGP_BETA_TIMES_B, CIMGP_CHANNEL_A_ALPHA, CIMGP_CHANNEL_A_SOURCE },
  /* PictOpAdd */
  { CIMGP_A_PLUS_BETA_B, CIMGP_CONSTANT_ALPHA, CIMGP_CHANNEL_A_SOURCE }, { }
};


#define ARRAY_SIZE(a) (sizeof((a)) / (sizeof(*(a))))

static const struct exa_format_t *lx_get_format(PicturePtr p)
{
  int i;
  unsigned int format = p->format;

  for(i = 0; i < ARRAY_SIZE(lx_exa_formats); i++) {

    if (lx_exa_formats[i].bpp < PICT_FORMAT_BPP(format))
      break;
    else if (lx_exa_formats[i].bpp != PICT_FORMAT_BPP(format))
      continue;

    if (lx_exa_formats[i].exa == format)
      return (&lx_exa_formats[i]);
  }

#if 0
  ErrorF("Couldn't match on format %x\n", format);
  ErrorF("BPP = %d, type = %d, ARGB(%d,%d,%d,%d)n",
	 PICT_FORMAT_BPP(format),
	 PICT_FORMAT_TYPE(format),
	 PICT_FORMAT_A(format),
	 PICT_FORMAT_R(format),
	 PICT_FORMAT_G(format),
	 PICT_FORMAT_B(format));
#endif

  return NULL;
}

static Bool lx_check_composite(int op, PicturePtr pSrc, PicturePtr pMsk,
			PicturePtr pDst)
{
  GeodeRec *pGeode = GEODEPTR_FROM_PICTURE(pDst);

  /* Check that the operation is supported */

  if (op > PictOpAdd)
    return FALSE;

  if (usesPasses(op)) {
	if (pGeode->exaBfrOffset == 0 || !pMsk)
		return FALSE;
  }

  /* Check that the filter matches what we support */

  switch(pSrc->filter) {
  case PictFilterNearest:
  case PictFilterFast:
  case PictFilterGood:
  case PictFilterBest:
    break;

  default:
    ErrorF("invalid filter %d\n", pSrc->filter);
    return FALSE;
  }

  /* We don't handle transforms */

  if (pSrc->transform)
    return FALSE;


  /* XXX - I don't understand PICT_a8 enough - so I'm punting */

  if (pSrc->format == PICT_a8 || pDst->format == PICT_a8)
    return FALSE;

  return TRUE;
}

static Bool lx_prepare_composite(int op, PicturePtr pSrc, PicturePtr pMsk,
		     PicturePtr pDst, PixmapPtr pxSrc, PixmapPtr pxMsk,
		     PixmapPtr pxDst)
{
  GeodeRec *pGeode = GEODEPTR_FROM_PIXMAP(pxDst);
  const struct exa_format_t *srcFmt, *dstFmt;

  /* Get the formats for the source and destination */

  if ((srcFmt = lx_get_format(pSrc)) == NULL) {
	ErrorF("EXA: Invalid source format %x\n", pSrc->format);
        return FALSE;
  }

  if ((dstFmt = lx_get_format(pDst)) == NULL) {
	ErrorF("EXA:  Invalid destination format %x\n", pDst->format);
	return FALSE;
  }

  /* Make sure operations that need alpha bits have them */
  /* If a mask is enabled, the alpha will come from there */

  if (!pMsk && (!srcFmt->alphabits && usesSrcAlpha(op))) {
    ErrorF("EXA:  Source needs alpha bits\n");
    return FALSE;
  }

  if (!pMsk && (!dstFmt->alphabits && usesDstAlpha(op))) {
    ErrorF("EXA: Dest needs alpha bits\n");
    return FALSE;
  }

  /* FIXME:  See a way around this! */

  if (srcFmt->alphabits == 0 && dstFmt->alphabits != 0)
    return FALSE;

  /* Set up the scratch buffer with the information we need */

  exaScratch.srcFormat = (struct exa_format_t *) srcFmt;
  exaScratch.dstFormat = (struct exa_format_t *) dstFmt;
  exaScratch.op = op;
  exaScratch.repeat = pSrc->repeat;
  exaScratch.bufferOffset = pGeode->exaBfrOffset;


  if (pMsk && op != PictOpClear) {
    unsigned int srcColor;
    struct blend_ops_t *opPtr = &lx_alpha_ops[op * 2];
    int direction = (opPtr->channel == CIMGP_CHANNEL_A_SOURCE) ? 0 : 1;

    /* We can only do masks with a 8bpp or a 4bpp mask */

    if (pMsk->format != PICT_a8 && pMsk->format != PICT_a4)
      return FALSE;

    /* Direction 0 indicates src->dst, 1 indiates dst->src */

    if (((direction == 0) && (pxSrc->drawable.bitsPerPixel < 16)) ||
	((direction == 1) && (pxDst->drawable.bitsPerPixel < 16))) {
      ErrorF("Can't do mask blending with less then 16bpp\n");
      return FALSE;
    }

    /* Get the source color */

    if (direction == 0)
      exaScratch.srcColor = lx_get_source_color(pSrc, 0, 0, pDst->format);
    else
      exaScratch.srcColor = lx_get_source_color(pDst, 0, 0, pSrc->format);

    /* FIXME:  What to do here? */

    if (pSrc->pDrawable->width != 1 || pSrc->pDrawable->height != 1)
      return FALSE;

    /* Save off the info we need (reuse the source values to save space) */

    exaScratch.type = COMP_TYPE_MASK;

    exaScratch.srcOffset = exaGetPixmapOffset(pxMsk);
    exaScratch.srcPitch = exaGetPixmapPitch(pxMsk);
    exaScratch.srcBpp = (pxMsk->drawable.bitsPerPixel + 7) / 8;

    exaScratch.srcWidth = pMsk->pDrawable->width;
    exaScratch.srcHeight = pMsk->pDrawable->height;

    /* Flag to indicate if this a 8BPP or a 4BPP mask */
    exaScratch.fourBpp = (pxMsk->drawable.bitsPerPixel == 4) ? 1 : 0;

    /* If the direction is reversed, then remember the source */

    if (direction == 1)
      exaScratch.srcPixmap = pxSrc;
  }
  else {
    if (usesPasses(op))
      exaScratch.type = COMP_TYPE_TWOPASS;
    else
      exaScratch.type = COMP_TYPE_ONEPASS;

    exaScratch.srcOffset = exaGetPixmapOffset(pxSrc);
    exaScratch.srcPitch = exaGetPixmapPitch(pxSrc);
    exaScratch.srcBpp = (pxSrc->drawable.bitsPerPixel + 7) / 8;

    exaScratch.srcWidth = pSrc->pDrawable->width;
    exaScratch.srcHeight = pSrc->pDrawable->height;
  }

  return TRUE;
}

static int lx_get_bpp_from_format(int format) {

  switch(format) {
  case CIMGP_SOURCE_FMT_8_8_8_8:
  case CIMGP_SOURCE_FMT_32BPP_BGR:
    return 32;

  case CIMGP_SOURCE_FMT_4_4_4_4:
    return 12;

  case CIMGP_SOURCE_FMT_0_5_6_5:
  case CIMGP_SOURCE_FMT_16BPP_BGR:
    return 16;

  case CIMGP_SOURCE_FMT_1_5_5_5:
  case CIMGP_SOURCE_FMT_15BPP_BGR:
    return 15;

  case CIMGP_SOURCE_FMT_3_3_2:
    return 8;
  }

  return 0;
}

/* BGR needs to be set in the source for it to take - so adjust the source
 * to enable BGR if the two formats are different, and disable it if they
 * are the same
 */

static void lx_set_source_format(int srcFormat, int dstFormat)
{
  if (!(srcFormat & 0x10) && (dstFormat & 0x10))
    gp_set_source_format(srcFormat | 0x10);
  else if ((srcFormat & 0x10) && (dstFormat & 0x10))
    gp_set_source_format(srcFormat & ~0x10);
  else
    gp_set_source_format(srcFormat);
}

/* If we are converting colors and we need the channel A alpha,
 * then use a special alpha type that preserves the alpha before
 * converting the format
 */

static inline int get_op_type(struct exa_format_t *src,
			      struct exa_format_t *dst, int type)
{
	return (type == CIMGP_CHANNEL_A_ALPHA &&
		src->alphabits != dst->alphabits) ?
			CIMGP_CONVERTED_ALPHA : type;
}

/* Note - this is the preferred onepass method.  The other will remain
 * ifdefed out until such time that we are sure its not needed
 */

#if 1

static void
lx_composite_onepass(PixmapPtr pxDst, unsigned long dstOffset,
	unsigned long srcOffset, int width, int height)
{
  struct blend_ops_t *opPtr;
  int apply, type;

  opPtr = &lx_alpha_ops[exaScratch.op * 2];

  apply = (exaScratch.dstFormat->alphabits != 0 &&
	   exaScratch.srcFormat->alphabits != 0) ?
    CIMGP_APPLY_BLEND_TO_ALL : CIMGP_APPLY_BLEND_TO_RGB;

  gp_declare_blt(0);
  gp_set_bpp(lx_get_bpp_from_format(exaScratch.dstFormat->fmt));
  gp_set_strides(exaGetPixmapPitch(pxDst), exaScratch.srcPitch);

  lx_set_source_format(exaScratch.srcFormat->fmt, exaScratch.dstFormat->fmt);

  type = get_op_type(exaScratch.srcFormat, exaScratch.dstFormat, opPtr->type);

  gp_set_alpha_operation(opPtr->operation, type,
			 opPtr->channel, apply, 0);

  gp_screen_to_screen_convert(dstOffset, srcOffset, width, height, 0);
}

#else

/* XXX - For now, we assume that the conversion will fit */

static void
lx_composite_onepass(PixmapPtr pxDst, unsigned long dstOffset,
		unsigned long srcOffset, int width, int height)
{
  struct blend_ops_t *opPtr;
  int apply, type;
  int sbpp = lx_get_bpp_from_format(exaScratch.srcFormat->fmt);

  /* Copy the destination into the scratch buffer */

  gp_declare_blt(0);

  gp_set_bpp(sbpp);

  gp_set_source_format(exaScratch.dstFormat->fmt);

  gp_set_raster_operation(0xCC);
  gp_set_strides(exaScratch.srcPitch, exaGetPixmapPitch(pxDst));
  gp_screen_to_screen_convert(exaScratch.bufferOffset, dstOffset, width,
			      height, 0);

  /* Do the blend */

  opPtr = &lx_alpha_ops[exaScratch.op * 2];
  apply = (exaScratch.srcFormat->alphabits == 0) ?
    CIMGP_APPLY_BLEND_TO_RGB : CIMGP_APPLY_BLEND_TO_ALL;

  gp_declare_blt (0);
  gp_set_bpp(sbpp);

  type = get_op_type(exaScratch.srcFormat, exaScrach.dstFormat, opPtr->type);

  gp_set_alpha_operation(opPtr->operation, type, opPtr->channel,
			 apply, 0);

  gp_set_strides(exaScratch.srcPitch, exaScratch.srcPitch);
  gp_screen_to_screen_blt(exaScratch.bufferOffset, srcOffset,
			  width, height, 0);

  /* And copy back */

  gp_declare_blt(0);
  gp_set_bpp(pxDst->drawable.bitsPerPixel);
  gp_set_source_format (exaScratch.srcFormat->fmt);
  gp_set_raster_operation(0xCC);
  gp_set_strides(exaGetPixmapPitch(pxDst), exaScratch.srcPitch);
  gp_screen_to_screen_convert(dstOffset, exaScratch.bufferOffset,
			      width, height, 0);
}

#endif

#if 0

lx_composite_convert(PixmapPtr pxDst, unsigned long dstOffset,
		     unsigned long srcOffset, int width, int height) 
{
  /* Step 1 - copy the destination into the scratch buffer */

  ErrorF("Convert\n");

  gp_declare_blt(0);
  gp_set_bpp(lx_get_bpp_from_format(exaScratch.dstFormat->fmt));

  gp_set_raster_operation(0xCC);
  gp_set_strides(exaGetPixmapPitch(pxDst), exaGetPixmapPitch(pxDst));

  gp_screen_to_screen_blt(exaScratch.bufferOffset, dstOffset, width, height, 0);

  /* Step 2 - Do the original blend */

  lx_composite_onepass(pxDst, exaScratch.bufferOffset, srcOffset, width, height);
  
  /* Step 3 - copy back and fixup the alpha */
  gp_declare_blt(0);
  gp_set_bpp(lx_get_bpp_from_format(exaScratch.dstFormat->fmt));
  gp_set_strides(exaGetPixmapPitch(pxDst), exaGetPixmapPitch(pxDst));

  /* FIXME: Does this alpha value need to be changed for the mode? */

  gp_set_alpha_operation(CIMGP_ALPHA_TIMES_A, CIMGP_CONSTANT_ALPHA, 
	 		 CIMGP_CHANNEL_A_SOURCE, CIMGP_APPLY_BLEND_TO_ALPHA, 
			 1);
 
  gp_screen_to_screen_blt(dstOffset, exaScratch.bufferOffset, width, height, 0);
}

#endif

/* This function handles the multipass blend functions */

static void
lx_composite_multipass(PixmapPtr pxDst, unsigned long dstOffset, unsigned long srcOffset, int width, int height)
{
  struct blend_ops_t *opPtr;
  int sbpp = lx_get_bpp_from_format(exaScratch.srcFormat->fmt);
  int apply, type;

  /* Wait until the GP is idle - this will ensure that the scratch buffer
   * isn't occupied */

  gp_wait_until_idle();

  /* Copy the destination to the scratch buffer, and convert it to the
   * source format */

  gp_declare_blt(0);

  gp_set_bpp(sbpp);
  gp_set_source_format(exaScratch.dstFormat->fmt);
  gp_set_raster_operation(0xCC);
  gp_set_strides(exaScratch.srcPitch, exaGetPixmapPitch(pxDst));
  gp_screen_to_screen_convert(exaScratch.bufferOffset, dstOffset,
  width, height, 0);

  /* Do the first blend from the source to the scratch buffer */

  gp_declare_blt(CIMGP_BLTFLAGS_HAZARD);
  gp_set_bpp(sbpp);
  gp_set_source_format(exaScratch.srcFormat->fmt);
  gp_set_strides(exaScratch.srcPitch, exaScratch.srcPitch);

  opPtr = &lx_alpha_ops[exaScratch.op * 2];

  apply = (exaScratch.srcFormat->alphabits == 0) ?
    CIMGP_APPLY_BLEND_TO_RGB : CIMGP_APPLY_BLEND_TO_ALL;

  /* If we're destroying the source alpha bits, then make sure we
   * use the alpha before the color conversion
   */

  gp_screen_to_screen_blt(exaScratch.bufferOffset, srcOffset, width, height, 0);

  /* Finally, do the second blend back to the destination */

  opPtr = &lx_alpha_ops[(exaScratch.op * 2) + 1];

  apply = (exaScratch.dstFormat->alphabits == 0) ?
    CIMGP_APPLY_BLEND_TO_RGB : CIMGP_APPLY_BLEND_TO_ALL;

  gp_declare_blt(CIMGP_BLTFLAGS_HAZARD);
  gp_set_bpp(lx_get_bpp_from_format(exaScratch.dstFormat->fmt));

  lx_set_source_format(exaScratch.srcFormat->fmt, exaScratch.dstFormat->fmt);

  type = get_op_type(exaScratch.srcFormat, exaScratch.dstFormat, opPtr->type);

  gp_set_alpha_operation(opPtr->operation, type, opPtr->channel,
			 apply, 0);

  gp_screen_to_screen_convert(dstOffset, exaScratch.bufferOffset,
  width, height, 0);
}

static void
lx_do_composite_mask(PixmapPtr pxDst, unsigned long dstOffset,
			unsigned int maskOffset, int width, int height)
{
  GeodeRec *pGeode = GEODEPTR_FROM_PIXMAP(pxDst);
  unsigned char *data = pGeode->FBBase + maskOffset;

  struct blend_ops_t *opPtr = &lx_alpha_ops[exaScratch.op * 2];

  gp_declare_blt (0);

  gp_set_source_format(exaScratch.srcFormat->fmt);
  gp_set_strides(exaGetPixmapPitch(pxDst), exaScratch.srcPitch);
  gp_set_bpp(lx_get_bpp_from_format(exaScratch.dstFormat->fmt));
  gp_set_solid_source (exaScratch.srcColor);

  gp_blend_mask_blt(dstOffset, 0, width, height, maskOffset,
		exaScratch.srcPitch, opPtr->operation,
		    exaScratch.fourBpp);
}

#define GetPixmapOffset(px, x, y) ( exaGetPixmapOffset((px)) + \
  (exaGetPixmapPitch((px)) * (y)) + \
  ((((px)->drawable.bitsPerPixel + 7) / 8) * (x)) )

static void
lx_do_composite(PixmapPtr pxDst, int srcX, int srcY, int maskX,
		int maskY, int dstX, int dstY, int width, int height) {

  struct blend_ops_t *opPtr = &lx_alpha_ops[exaScratch.op * 2];

  unsigned int dstOffset, srcOffset;

  unsigned int opX = dstX;
  unsigned int opY = dstY;
  unsigned int opWidth = width;
  unsigned int opHeight = height;

  if (exaScratch.type == COMP_TYPE_MASK)
    srcOffset = exaScratch.srcOffset + (maskY * exaScratch.srcPitch) +
      (maskX * exaScratch.srcBpp);
  else
    srcOffset = exaScratch.srcOffset + (srcY * exaScratch.srcPitch) +
      (srcX * exaScratch.srcBpp);

  /* Adjust the width / height of the operation the size of the source */

  if (exaScratch.srcWidth < width)
    opWidth = exaScratch.srcWidth;

  if (exaScratch.srcHeight < height)
    opHeight = exaScratch.srcHeight;

  while(1) {
    dstOffset = GetPixmapOffset(pxDst, opX, opY);

    switch(exaScratch.type) {
    case COMP_TYPE_MASK: {
      int direction = (opPtr->channel == CIMGP_CHANNEL_A_SOURCE) ? 0 : 1;

      if (direction == 1) {
	dstOffset = GetPixmapOffset(exaScratch.srcPixmap, dstX,dstY);
	lx_do_composite_mask(exaScratch.srcPixmap, dstOffset, srcOffset,
		opWidth, opHeight);
      }
      else {
	lx_do_composite_mask(pxDst, dstOffset, srcOffset, opWidth, opHeight);
      }
    }
      break;

    case COMP_TYPE_ONEPASS:
      lx_composite_onepass(pxDst, dstOffset, srcOffset, opWidth, opHeight);
      break;

    case COMP_TYPE_TWOPASS:
      lx_composite_multipass(pxDst, dstOffset, srcOffset, opWidth, opHeight);
      break;
    }

    if (!exaScratch.repeat)
      break;

    opX += opWidth;

    if (opX >= dstX + width) {
      opX = dstX;
      opY += opHeight;

      if (opY >= dstY + height)
	break;
    }

    opWidth = ((dstX + width) - opX) > exaScratch.srcWidth ?
      exaScratch.srcWidth : (dstX + width) - opX;
    opHeight  = ((dstY + height) - opY) > exaScratch.srcHeight ?
      exaScratch.srcHeight : (dstY + height) - opY;
  }
}

static void lx_wait_marker(ScreenPtr PScreen, int marker)
{
  gp_wait_until_idle();
}

static void lx_done(PixmapPtr ptr)
{
}

static Bool lx_upload(PixmapPtr pDst, int x, int y, int w, int h,
		      char *src, int src_pitch)
{
  char *dst = pDst->devPrivate.ptr;
  int dpitch = exaGetPixmapPitch(pDst);
  int bpp = pDst->drawable.bitsPerPixel;
  GeodeRec *pGeode = GEODEPTR_FROM_PIXMAP(pDst);
  unsigned long offset;

  dst += (y * dpitch) + (x * (bpp >> 3));

  gp_declare_blt(0);

  gp_set_bpp(bpp);
  gp_set_raster_operation(0xCC);
  gp_set_strides(dpitch, src_pitch);
  gp_set_solid_pattern(0);

  offset = ((unsigned long) dst) - ((unsigned long) pGeode->FBBase);
  gp_color_bitmap_to_screen_blt(offset, 0, w, h, (unsigned char *)src, src_pitch);
  return TRUE;
}

static Bool lx_download(PixmapPtr pSrc, int x, int y, int w, int h,
			char *dst, int dst_pitch)
{
  char *src = pSrc->devPrivate.ptr;
  int spitch = exaGetPixmapPitch(pSrc);
  int bpp = pSrc->drawable.bitsPerPixel;

  src += (y * spitch) + (x * (bpp >> 3));

  geode_memory_to_screen_blt((unsigned long)src, (unsigned long)dst,
			     spitch, dst_pitch, w, h, bpp);
  return TRUE;
}


Bool LXExaInit(ScreenPtr pScreen)
{
  ScrnInfoPtr pScrni = xf86Screens[pScreen->myNum];
  GeodeRec *pGeode = GEODEPTR(pScrni);
  ExaDriverPtr pExa = pGeode->pExa;

  pExa->exa_major = EXA_VERSION_MAJOR;
  pExa->exa_minor = EXA_VERSION_MINOR;

  pExa->WaitMarker = lx_wait_marker;

  pExa->UploadToScreen = lx_upload;
  //pExa->DownloadFromScreen = lx_download;

  pExa->PrepareSolid = lx_prepare_solid;
  pExa->Solid = lx_do_solid;
  pExa->DoneSolid = lx_done;

  pExa->PrepareCopy = lx_prepare_copy;
  pExa->Copy = lx_do_copy;
  pExa->DoneCopy = lx_done;

  /* Composite */
  pExa->CheckComposite = lx_check_composite;
  pExa->PrepareComposite = lx_prepare_composite;
  pExa->Composite = lx_do_composite;
  pExa->DoneComposite = lx_done;

  return exaDriverInit(pScreen, pGeode->pExa);
}
