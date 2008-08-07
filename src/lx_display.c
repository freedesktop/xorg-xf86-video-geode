/* Copyright (c) 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Neither the name of the Advanced Micro Devices, Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "geode.h"
#include "xf86Crtc.h"
#include "cim/cim_defs.h"
#include "cim/cim_regs.h"

typedef struct _LXOutputPrivateRec
{
    int video_enable;
    unsigned long video_flags;
    GeodeMemPtr rotate_mem;
} LXCrtcPrivateRec, *LXCrtcPrivatePtr;

static void
lx_enable_dac_power(ScrnInfoPtr pScrni, int option)
{
    GeodeRec *pGeode = GEODEPTR(pScrni);

    df_set_crt_enable(DF_CRT_ENABLE);

    /* Turn off the DAC if we don't need the CRT */

    if (option && (!(pGeode->Output & OUTPUT_CRT))) {
	unsigned int misc = READ_VID32(DF_VID_MISC);

	misc |= DF_DAC_POWER_DOWN;
	WRITE_VID32(DF_VID_MISC, misc);
    }

    if (pGeode->Output & OUTPUT_PANEL)
	df_set_panel_enable(1);
}

static void
lx_disable_dac_power(ScrnInfoPtr pScrni, int option)
{
    GeodeRec *pGeode = GEODEPTR(pScrni);

    if (pGeode->Output & OUTPUT_PANEL)
	df_set_panel_enable(0);

    if (pGeode->Output & OUTPUT_CRT) {

	/* Wait for the panel to finish its procedure */

	if (pGeode->Output & OUTPUT_PANEL)
	    while ((READ_VID32(DF_POWER_MANAGEMENT) & 2) == 0) ;
	df_set_crt_enable(option);
    }
}

static void
lx_set_panel_mode(VG_DISPLAY_MODE * mode, DisplayModePtr pMode)
{
    mode->mode_width = mode->panel_width = pMode->HDisplay;
    mode->mode_height = mode->panel_height = pMode->VDisplay;

    mode->hactive = pMode->HDisplay;
    mode->hblankstart = pMode->HDisplay;
    mode->hsyncstart = pMode->HSyncStart;
    mode->hsyncend = pMode->HSyncEnd;
    mode->hblankend = pMode->HTotal;
    mode->htotal = pMode->HTotal;

    mode->vactive = pMode->VDisplay;
    mode->vblankstart = pMode->VDisplay;
    mode->vsyncstart = pMode->VSyncStart;
    mode->vsyncend = pMode->VSyncEnd;
    mode->vblankend = pMode->VTotal;
    mode->vtotal = pMode->VTotal;

    mode->vactive_even = pMode->VDisplay;
    mode->vblankstart_even = pMode->VDisplay;
    mode->vsyncstart_even = pMode->VSyncStart;
    mode->vsyncend_even = pMode->VSyncEnd;
    mode->vblankend_even = pMode->VTotal;
    mode->vtotal_even = pMode->VTotal;

    mode->frequency = (int)((pMode->Clock / 1000.0) * 0x10000);
}

static void
lx_set_crt_mode(VG_DISPLAY_MODE * mode, DisplayModePtr pMode)
{
    mode->mode_width = mode->panel_width = pMode->HDisplay;
    mode->mode_height = mode->panel_height = pMode->VDisplay;

    mode->hactive = pMode->CrtcHDisplay;
    mode->hblankstart = pMode->CrtcHBlankStart;
    mode->hsyncstart = pMode->CrtcHSyncStart;
    mode->hsyncend = pMode->CrtcHSyncEnd;
    mode->hblankend = pMode->CrtcHBlankEnd;
    mode->htotal = pMode->CrtcHTotal;

    mode->vactive = pMode->CrtcVDisplay;
    mode->vblankstart = pMode->CrtcVBlankStart;
    mode->vsyncstart = pMode->CrtcVSyncStart;
    mode->vsyncend = pMode->CrtcVSyncEnd;
    mode->vblankend = pMode->CrtcVBlankEnd;
    mode->vtotal = pMode->CrtcVTotal;

    mode->vactive_even = pMode->CrtcVDisplay;
    mode->vblankstart_even = pMode->CrtcVBlankStart;
    mode->vsyncstart_even = pMode->CrtcVSyncStart;
    mode->vsyncend_even = pMode->CrtcVSyncEnd;
    mode->vblankend_even = pMode->CrtcVBlankEnd;
    mode->vtotal_even = pMode->CrtcVTotal;

    mode->frequency = (int)((pMode->Clock / 1000.0) * 0x10000);
}

static int
lx_set_mode(ScrnInfoPtr pScrni, DisplayModePtr pMode, int bpp)
{
    GeodeRec *pGeode = GEODEPTR(pScrni);
    VG_DISPLAY_MODE mode;
    int hsync, vsync;
    int ret;

    memset(&mode, 0, sizeof(mode));

    /* Cimarron purposely swaps the sync when panels are enabled -this is
     * presumably to allow for "default" panels which are normally active
     * low, so we need to swizzle the flags
     */

    hsync = (pMode->Flags & V_NHSYNC) ? 1 : 0;
    vsync = (pMode->Flags & V_NVSYNC) ? 1 : 0;

    if (pGeode->Output & OUTPUT_PANEL) {
	hsync = !vsync;
	vsync = !vsync;
    }

    mode.flags |= (hsync) ? VG_MODEFLAG_NEG_HSYNC : 0;
    mode.flags |= (vsync) ? VG_MODEFLAG_NEG_VSYNC : 0;

    mode.flags |= pGeode->Output & OUTPUT_CRT ? VG_MODEFLAG_CRT_AND_FP : 0;

    if (pGeode->Output & OUTPUT_PANEL) {
	mode.flags |= VG_MODEFLAG_PANELOUT;
	if (pGeode->Output & OUTPUT_CRT)
	    mode.flags |= VG_MODEFLAG_CRT_AND_FP;
    }

    if (pGeode->Output & OUTPUT_PANEL && pGeode->Scale)
	lx_set_panel_mode(&mode, pGeode->panelMode);
    else
	lx_set_crt_mode(&mode, pMode);

    mode.src_width = pMode->HDisplay;
    mode.src_height = pMode->VDisplay;

    ret = vg_set_custom_mode(&mode, bpp);
    return (ret == CIM_STATUS_OK) ? 0 : -1;
}

static void
lx_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
    ScrnInfoPtr pScrni = crtc->scrn;
    GeodeRec *pGeode = GEODEPTR(pScrni);

    if (pGeode->Output & OUTPUT_DCON) {
	if (DCONDPMSSet(pScrni, mode))
	    return;
    }

    switch (mode) {
    case DPMSModeOn:
	lx_enable_dac_power(pScrni, 1);
	break;

    case DPMSModeStandby:
	lx_disable_dac_power(pScrni, DF_CRT_STANDBY);
	break;

    case DPMSModeSuspend:
	lx_disable_dac_power(pScrni, DF_CRT_SUSPEND);
	break;

    case DPMSModeOff:
	lx_disable_dac_power(pScrni, DF_CRT_DISABLE);
	break;
    }
}

static Bool
lx_crtc_lock(xf86CrtcPtr crtc)
{
    /* Wait until the GPU is idle */
    gp_wait_until_idle();
    return TRUE;
}

static void
lx_crtc_unlock(xf86CrtcPtr crtc)
{
    /* Nothing to do here */
}

static void
lx_crtc_prepare(xf86CrtcPtr crtc)
{
    LXCrtcPrivatePtr lx_crtc = crtc->driver_private;

    /* Disable the video */
    df_get_video_enable(&lx_crtc->video_enable, &lx_crtc->video_flags);

    if (lx_crtc->video_enable)
	df_set_video_enable(0, 0);

    /* Turn off compression */
    vg_set_compression_enable(0);

    /* Hide the cursor */
    crtc->funcs->hide_cursor(crtc);

    /* Turn off the display */
    crtc->funcs->dpms(crtc, DPMSModeOff);
}

static Bool
lx_crtc_mode_fixup(xf86CrtcPtr crtc, DisplayModePtr mode,
    DisplayModePtr adjusted_mode)
{
    return TRUE;
}

static void
lx_crtc_mode_set(xf86CrtcPtr crtc, DisplayModePtr mode,
    DisplayModePtr adjusted_mode, int x, int y)
{
    ScrnInfoPtr pScrni = crtc->scrn;
    GeodeRec *pGeode = GEODEPTR(pScrni);
    DF_VIDEO_SOURCE_PARAMS vs_odd, vs_even;

    df_get_video_source_configuration(&vs_odd, &vs_even);

    /* Note - the memory gets adjusted when virtualX/virtualY
     * gets changed - so we don't need to worry about it here
     */

    if (lx_set_mode(pScrni, adjusted_mode, pScrni->bitsPerPixel))
	ErrorF("ERROR!  Unable to set the mode!\n");

    /* The output gets turned in in the output code as
     * per convention */

    vg_set_display_pitch(pGeode->Pitch);
    gp_set_bpp(pScrni->bitsPerPixel);

    /* FIXME: Whats up with X and Y?  Does that come into play
     * here? */

    vg_set_display_offset(0);
    df_configure_video_source(&vs_odd, &vs_even);

    vg_wait_vertical_blank();
}

static void
lx_crtc_commit(xf86CrtcPtr crtc)
{
    LXCrtcPrivatePtr lx_crtc = crtc->driver_private;
    ScrnInfoPtr pScrni = crtc->scrn;
    GeodeRec *pGeode = GEODEPTR(pScrni);

    /* Turn back on the sreen */
    crtc->funcs->dpms(crtc, DPMSModeOn);

    /* Turn on compression */

    if (pGeode->Compression) {
	vg_configure_compression(&(pGeode->CBData));
	vg_set_compression_enable(1);
    }

    /* Load the cursor */
    if (crtc->scrn->pScreen != NULL)
	xf86_reload_cursors(crtc->scrn->pScreen);

    /* Renable the video */

    if (lx_crtc->video_enable)
	df_set_video_enable(lx_crtc->video_enable, lx_crtc->video_flags);

    lx_crtc->video_enable = 0;
    lx_crtc->video_flags = 0;
}

static void
lx_crtc_gamma_set(xf86CrtcPtr crtc, CARD16 * red, CARD16 * green,
    CARD16 * blue, int size)
{
    unsigned int dcfg;
    int i;

    assert(size == 256);

    for (i = 0; i < 256; i++) {
	unsigned int val = (*red << 8) | *green | (*blue >> 8);

	df_set_video_palette_entry(i, val);
    }

    /* df_set_video_palette_entry automatically turns on
     * gamma for video - if this gets called, we assume that
     * RandR wants it set for graphics, so reverse cimarron
     */

    dcfg = READ_VID32(DF_DISPLAY_CONFIG);
    dcfg &= ~DF_DCFG_GV_PAL_BYP;
    WRITE_VID32(DF_DISPLAY_CONFIG, dcfg);
}

static void *
lx_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
    ScrnInfoPtr pScrni = crtc->scrn;
    GeodePtr pGeode = GEODEPTR(pScrni);
    LXCrtcPrivatePtr lx_crtc = crtc->driver_private;
    unsigned int rpitch, size;

    rpitch = pScrni->displayWidth * (pScrni->bitsPerPixel / 8);
    size = rpitch * height;

    lx_crtc->rotate_mem = GeodeAllocOffscreen(pGeode, size, 4);

    if (lx_crtc->rotate_mem == NULL) {
	xf86DrvMsg(pScrni->scrnIndex, X_ERROR,
	    "Couldn't allocate shadow memory for rotated CRTC\n");
	return NULL;
    }

    memset(pGeode->FBBase + lx_crtc->rotate_mem->offset, 0, size);
    return pGeode->FBBase + lx_crtc->rotate_mem->offset;
}

static PixmapPtr
lx_crtc_shadow_create(xf86CrtcPtr crtc, void *data, int width, int height)
{
    ScrnInfoPtr pScrni = crtc->scrn;
    PixmapPtr rpixmap;
    unsigned int rpitch;

    if (!data)
	data = lx_crtc_shadow_allocate(crtc, width, height);

    rpitch = pScrni->displayWidth * (pScrni->bitsPerPixel / 8);

    rpixmap = GetScratchPixmapHeader(pScrni->pScreen,
	width, height, pScrni->depth, pScrni->bitsPerPixel, rpitch, data);

    if (rpixmap == NULL) {
	xf86DrvMsg(pScrni->scrnIndex, X_ERROR,
	    "Couldn't allocate shadow pixmap for rotated CRTC\n");
    }

    return rpixmap;
}

static void
lx_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr rpixmap, void *data)
{
    ScrnInfoPtr pScrni = crtc->scrn;
    GeodeRec *pGeode = GEODEPTR(pScrni);
    LXCrtcPrivatePtr lx_crtc = crtc->driver_private;

    if (rpixmap)
	FreeScratchPixmapHeader(rpixmap);

    if (data) {
	gp_wait_until_idle();
	GeodeFreeOffscreen(pGeode, lx_crtc->rotate_mem);
	lx_crtc->rotate_mem = NULL;
    }
}

static void
lx_crtc_set_cursor_colors(xf86CrtcPtr crtc, int bg, int fg)
{
    vg_set_mono_cursor_colors(bg, fg);
}

static void
lx_crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
    VG_PANNING_COORDINATES panning;

    /* FIXME: Do I need to worry about rotation adjustment here? */

    switch (crtc->rotation) {
    case RR_Rotate_0:
	x += 31;
	y += 31;
    }

    vg_set_cursor_position(x, y, &panning);
}

static void
lx_crtc_show_cursor(xf86CrtcPtr crtc)
{
    vg_set_cursor_enable(1);
}

static void
lx_crtc_hide_cursor(xf86CrtcPtr crtc)
{
    vg_set_cursor_enable(0);
}

static void
lx_crtc_load_cursor_image(xf86CrtcPtr crtc, unsigned char *src)
{
    ScrnInfoPtr pScrni = crtc->scrn;

    LXLoadCursorImage(pScrni, src);
}

static const xf86CrtcFuncsRec lx_crtc_funcs = {
    .dpms = lx_crtc_dpms,
    .lock = lx_crtc_lock,
    .unlock = lx_crtc_unlock,
    .mode_fixup = lx_crtc_mode_fixup,
    .prepare = lx_crtc_prepare,
    .mode_set = lx_crtc_mode_set,
    .commit = lx_crtc_commit,
    .gamma_set = lx_crtc_gamma_set,
    .shadow_create = lx_crtc_shadow_create,
    .shadow_allocate = lx_crtc_shadow_allocate,
    .shadow_destroy = lx_crtc_shadow_destroy,
    .set_cursor_colors = lx_crtc_set_cursor_colors,
    .set_cursor_position = lx_crtc_set_cursor_position,
    .show_cursor = lx_crtc_show_cursor,
    .hide_cursor = lx_crtc_hide_cursor,
    .load_cursor_image = lx_crtc_load_cursor_image,
};

void
LXSetupCrtc(ScrnInfoPtr pScrni)
{
    xf86CrtcPtr crtc;
    LXCrtcPrivatePtr lxpriv;

    crtc = xf86CrtcCreate(pScrni, &lx_crtc_funcs);

    if (crtc == NULL) {
	ErrorF("ERROR - xf86CrtcCreate() fail %x\n", crtc);
	return;
    }

    lxpriv = xnfcalloc(sizeof(LXCrtcPrivateRec), 1);

    if (!lxpriv) {
	xf86CrtcDestroy(crtc);
	ErrorF("unable to allocate memory for lxpriv\n");
	return;
    }

    crtc->driver_private = lxpriv;
}