#ifndef DOLPHIN_GXGEOMETRY_H
#define DOLPHIN_GXGEOMETRY_H

#include <dolphin/gx/GXEnum.h>

#ifdef __cplusplus
extern "C" {
#endif

void GXSetVtxDesc(GXAttr attr, GXAttrType type);
void GXSetSourceVtxDesc(GXAttr attr, GXAttrType type);
void GXSetVtxDescv(GXVtxDescList* list);
void GXClearVtxDesc(void);
void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list);
void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
void GXSetNumTexGens(u8 nTexGens);
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize,
                       u32 postmtx);
void GXSetLineWidth(u8 width, GXTexOffset texOffsets);
void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets);
void GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable);
#ifdef TARGET_PC
void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le);
static inline void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride) {
  GXSetArray(attr, data, size, stride, false);
}
#define GXSETARRAY(attr, data, size, stride, le) GXSetArray((attr), (data), (size), (stride), (le))
/**
 * Writer, asynchronous decoder only: sets the array with its bytes copied into the stream
 * (GX_LOAD_AURORA_ARRAYDATA) instead of a pointer the decoder dereferences later. False when the
 * decoder is synchronous, a display list is being recorded or the array is too large: then use GXSetArray.
 */
bool GXSetArrayCopy(GXAttr attr, const void* data, u32 size, u8 stride);
#else
void GXSetArray(GXAttr attr, const void* data, u8 stride);
#define GXSETARRAY(attr, data, size, stride, le) GXSetArray((attr), (data), (stride))
#endif
void GXInvalidateVtxCache(void);

static inline void GXSetTexCoordGen(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx) {
  GXSetTexCoordGen2(dst_coord, func, src_param, mtx, GX_FALSE, GX_PTIDENTITY);
}

#ifdef __cplusplus
}
#endif

#endif
