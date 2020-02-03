#include "basetypes.h"
#include <assert.h>
#include "byteswap.h"
#include "ExecuteMEM.h"
#include "InstructionCache.h"
#include "mpe.h"
#include "NuonMemoryMap.h"
#include "NuonEnvironment.h"

#define XTILEMASK (~(0xFFFF0000UL << (16 - BilinearInfo_XTile(control))))
#define YTILEMASK (~(0xFFFF0000UL << (16 - BilinearInfo_YTile(control))))

#define MIP(mip_me) (((uint32)(mip_me)) >> BilinearInfo_XYMipmap(control))
#define SIGNMIP(mip_me) (((int32)(mip_me)) >> BilinearInfo_XYMipmap(control))

static const int32 pixel_type_width[16] = {
-2,//Type 0: MPEG Pixel (macroblock size of 16 bytes)
-1,//Type 1: 4 bit (must be accessed in groups of four)
1, //Type 2: 16 bit
0, //Type 3: 8 bit (must be accessed in groups of two)
2, //Type 4: 32 bit
2, //Type 5: 16 bit + 16 bit Z
3, //Type 6: 32 bit + 32 bit Z
0, //Type 7: Reserved
0, //Type 8: Byte
1, //Type 9: Word
2, //Type 10: Scalar
3, //Type 12: Short Vector
4, //Type 13: Vector
0, //Type 14: Reserved
0  //Type 15: Reserved
};

extern NuonEnvironment nuonEnv;

static uint32 mipped_xoffset = 0;
structBilinearAddressInfo bilinearAddressInfo;

static uint16 mirrorLookup[65536];

static uint8 satColY[1024];
static uint8 satColCrCb[1024];
static uint8 satColCrCbChnorm[1024];

void GenerateMirrorLookupTable()
{
    uint8 mirrorLookup8[256];
    for (uint32 i = 0; i <= 0xFF; i++)
    {
        uint8 mirror = 0;
        mirror |= ((i & 0x01) << 7) & 0x80;
        mirror |= ((i & 0x02) << 5) & 0x40;
        mirror |= ((i & 0x04) << 3) & 0x20;
        mirror |= ((i & 0x08) << 1) & 0x10;
        mirror |= ((i & 0x10) >> 1) & 0x08;
        mirror |= ((i & 0x20) >> 3) & 0x04;
        mirror |= ((i & 0x40) >> 5) & 0x02;
        mirror |= ((i & 0x80) >> 7) & 0x01;

        mirrorLookup8[i] = mirror;
    }

    for (uint32 i = 0; i <= 0xFFFF; i++)
    {
        union {
            struct { uint8 u8[2]; };
            uint16 u16;
        } xtmp;
#ifdef LITTLE_ENDIAN
        xtmp.u8[1] = mirrorLookup8[i&0xFF];
        xtmp.u8[0] = mirrorLookup8[(i>>8)&0xFF];
#else
        xtmp.u8[0] = mirrorLookup8[i&0xFF];
        xtmp.u8[1] = mirrorLookup8[(i>>8)&0xFF];
#endif
        mirrorLookup[i] = xtmp.u16;
    }
}

inline void SaturateColorComponentsOrg(uint32 &Y, uint32 &Cr, uint32 &Cb, const bool bChnorm)
{
  uint32 YLookup[] = {(Y >> (16 + 13 - 7)) & 0xFFUL,0xFFUL,0xFFUL,0xFFUL}; //!! was X,FF,0,0 before
  Y = YLookup[Y >> (16 + 14)];

  switch(Cr >> (16+14))
  {
    case 0:
      //If chnorm bit is set, clamp to 0x7F
      if(bChnorm && ((Cr>>16) > 0x1FFFu))
        Cr = 0x7F;
      else
        Cr = (Cr >> (16 + 13 - 7)) & 0xFFUL;
      break;
    case 1:
      //clamp to 0x7F or 0xFF
      Cr = bChnorm ? 0x7F : 0xFF;
      break;
    case 2:
      //clamp to 0x80 or 0x00
      Cr = bChnorm ? 0x80 : 0x00;
      break;
    case 3:
      //Clamp to 0x80 or 0x00
      if(bChnorm && ((Cr>>16) < 0xE000u))
        Cr = 0x80;
      else
      {
        if(bChnorm)
          Cr = (Cr >> (16 + 13 - 7)) & 0xFFUL;
        else
          Cr = 0x00;
      }
      break;
  }

  switch(Cb >> (16+14))
  {
    case 0:
      //If chnorm bit is set, clamp to 0x7F
      if(bChnorm && ((Cb>>16) > 0x1FFFu))
        Cb = 0x7F;
      else
        Cb = (Cb >> (16 + 13 - 7)) & 0xFFUL;
      break;
    case 1:
      //clamp to 0x7F or 0xFF
      Cb = bChnorm ? 0x7F : 0xFF;
      break;
    case 2:
      //clamp to 0x80 or 0x00
      Cb = bChnorm ? 0x80 : 0x00;
      break;
    case 3:
      //Clamp to 0x80 or 0x00
      if(bChnorm && ((Cb>>16) < 0xE000u))
        Cb = 0x80;
      else
      {
        if(bChnorm)
          Cb = (Cb >> (16 + 13 - 7)) & 0xFFUL;
        else
          Cb = 0x00;
      }
      break;
  }

  if(bChnorm)
  {
    Cr = (Cr + 0x80) & 0xFFUL;
    Cb = (Cb + 0x80) & 0xFFUL;
  }
}

inline void SaturateColorComponents(uint32& Y, uint32& Cr, uint32& Cb, const bool bChnorm)
{
  /*uint32 Y2 = Y;
  uint32 Cr2 = Cr;
  uint32 Cb2 = Cb;
  SaturateColorComponentsOrg(Y2, Cr2, Cb2, bChnorm);*/

  Y = satColY[Y >> 22];
  if (bChnorm)
  {
    Cr = satColCrCbChnorm[Cr >> 22];
    Cb = satColCrCbChnorm[Cb >> 22];
  }
  else
  {
    Cr = satColCrCb[Cr >> 22];
    Cb = satColCrCb[Cb >> 22];
  }

  /*assert(Y2 == Y);
  assert(Cr2 == Cr);
  assert(Cb2 == Cb);*/
}

void GenerateSaturateColorTables()
{
  for (uint32 i = 0; i < 1024; ++i)
  {
    uint32 Y  = i << 22;
    uint32 Cr = i << 22;
    uint32 Cb = i << 22;
    SaturateColorComponentsOrg(Y, Cr, Cb, true);
    satColCrCbChnorm[i] = Cr;
    Y  = i << 22;
    Cr = i << 22;
    Cb = i << 22;
    SaturateColorComponentsOrg(Y, Cr, Cb, false);
    satColY[i] = Y;
    satColCrCb[i] = Cr;
  }
}

inline void CalculateBilinearAddress(const MPE &mpe, uint32 * const pOffsetAddress, const uint32 control, uint32 x, uint32 y)
{
  if(BilinearInfo_XRev(control))
    x = (x&0xFFFF0000u) | mirrorLookup[x&0xFFFFu];

  if(BilinearInfo_YRev(control))
    y = (y&0xFFFF0000u) | mirrorLookup[y&0xFFFFu];

  //*pOffsetAddress = (((MIP(y) & SIGNMIP(YTILEMASK)) >> 16) * MIP(bi->xy_width) + ((MIP(x) & SIGNMIP(XTILEMASK)) >> 16));
  //mipped_xoffset = ((MIP(x) & SIGNMIP(XTILEMASK)) >> 16);
  //*pOffsetAddress = (((MIP(y) & SIGNMIP(YTILEMASK)) >> 16) * MIP(BilinearInfo_XYWidth(control)) + mipped_xoffset);
  mipped_xoffset = (MIP(x) & SIGNMIP(XTILEMASK)) >> 16;
  *pOffsetAddress = (((MIP(y) & SIGNMIP(YTILEMASK)) >> 16) * MIP(BilinearInfo_XYWidth(control)) + mipped_xoffset);
}

void GetBilinearAddress()
{
  const uint32 control = bilinearAddressInfo.control;
  const int32 pixwidth = BilinearInfo_PixelWidth(pixel_type_width,control);

  if(BilinearInfo_XRev(control))
    bilinearAddressInfo.x = (bilinearAddressInfo.x&0xFFFF0000u) | mirrorLookup[bilinearAddressInfo.x&0xFFFFu];

  if(BilinearInfo_YRev(control))
    bilinearAddressInfo.y = (bilinearAddressInfo.y&0xFFFF0000u) | mirrorLookup[bilinearAddressInfo.y&0xFFFFu];

  //*pOffsetAddress = (((MIP(y) & SIGNMIP(YTILEMASK)) >> 16) * MIP(bi->xy_width) + ((MIP(x) & SIGNMIP(XTILEMASK)) >> 16));
  //bilinearAddressInfo.mipped_xoffset = ((MIP(x) & SIGNMIP(XTILEMASK)) >> 16);
  //*pOffsetAddress = (((MIP(y) & SIGNMIP(YTILEMASK)) >> 16) * MIP(BilinearInfo_XYWidth(control)) + bilinearAddressInfo.mipped_xoffset);
  bilinearAddressInfo.mipped_xoffset = (MIP((bilinearAddressInfo.x)) & SIGNMIP(XTILEMASK)) >> 16;
  uint32 address = (((MIP((bilinearAddressInfo.y)) & SIGNMIP(YTILEMASK)) >> 16) * MIP(BilinearInfo_XYWidth(control)) + bilinearAddressInfo.mipped_xoffset);

  if(pixwidth >= 0)
  {
    //Everything but 4-bit pixels and MPEG
    address <<= pixel_type_width[(control >> 20) & 0x0FUL];
  }
  else
  {
    //4-bit pixels
    address >>= 1;
  }

  bilinearAddressInfo.offset_address = (bilinearAddressInfo.base & 0xFFFFFFFCu) + address;
}

void Execute_Mirror(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 x = pRegs[nuance.fields[FIELD_MEM_FROM]];

  x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
  x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
  x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
  x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));

  mpe.regs[nuance.fields[FIELD_MEM_TO]] = ((x >> 16) | (x << 16));
}

void Execute_MV_SImmediate(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  mpe.regs[nuance.fields[FIELD_MEM_TO]] = nuance.fields[FIELD_MEM_FROM];
}

void Execute_MV_SScalar(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  mpe.regs[nuance.fields[FIELD_MEM_TO]] = pRegs[nuance.fields[FIELD_MEM_FROM]];
}

void Execute_MV_V(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 src = nuance.fields[FIELD_MEM_FROM];
  const uint32 dest = nuance.fields[FIELD_MEM_TO];

  mpe.regs[dest    ] = pRegs[src];
  mpe.regs[dest + 1] = pRegs[src + 1];
  mpe.regs[dest + 2] = pRegs[src + 2];
  mpe.regs[dest + 3] = pRegs[src + 3];
}

void Execute_PopVector(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 dest_vector[4];

  const uint32* const srcPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  dest_vector[0] = srcPtr[0];
  dest_vector[1] = srcPtr[1];
  dest_vector[2] = srcPtr[2];
  dest_vector[3] = srcPtr[3];

  uint32* const destPtr = (uint32 *)&(mpe.regs[nuance.fields[FIELD_MEM_TO]]);

  SwapVectorBytes(dest_vector);

  destPtr[0] = dest_vector[0];
  destPtr[1] = dest_vector[1];
  destPtr[2] = dest_vector[2];
  destPtr[3] = dest_vector[3];

  mpe.sp += 16;
}

void Execute_PopVectorRz(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 dest_vector[4];

  const uint32* const srcPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  dest_vector[0] = srcPtr[0];
  dest_vector[1] = srcPtr[1];
  dest_vector[2] = srcPtr[2];
  dest_vector[3] = srcPtr[3];

  uint32* const destPtr = (uint32 *)&mpe.regs[nuance.fields[FIELD_MEM_TO]];

  SwapVectorBytes(dest_vector);

  destPtr[0] = dest_vector[0];
  destPtr[1] = dest_vector[1];
  destPtr[2] = dest_vector[2];
  mpe.rz = dest_vector[3];

  mpe.sp += 16;
}

void Execute_PopScalarRzi1(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 dest_vector[4];

  const uint32* const srcPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  dest_vector[0] = srcPtr[0];
  dest_vector[1] = srcPtr[1];
  dest_vector[2] = srcPtr[2];
  dest_vector[3] = srcPtr[3];

  SwapVectorBytes(dest_vector);

  mpe.regs[nuance.fields[FIELD_MEM_TO]] = dest_vector[0];
  mpe.cc = dest_vector[1];
  mpe.rzi1 = dest_vector[2];
  mpe.rz = dest_vector[3];

  mpe.sp += 16;
}

void Execute_PopScalarRzi2(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 dest_vector[4];

  const uint32* const srcPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  dest_vector[0] = srcPtr[0];
  dest_vector[1] = srcPtr[1];
  dest_vector[2] = srcPtr[2];
  dest_vector[3] = srcPtr[3];

  SwapVectorBytes(dest_vector);

  mpe.regs[nuance.fields[FIELD_MEM_TO]] = dest_vector[0];
  mpe.cc = dest_vector[1];
  mpe.rzi2 = dest_vector[2];
  mpe.rz = dest_vector[3];

  mpe.sp += 16;
}

void Execute_PushVector(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 src_vector[4];

  mpe.sp -= 16;

  const uint32* const pVal = &(pRegs[nuance.fields[FIELD_MEM_FROM]]);

  src_vector[0] = pVal[0];
  src_vector[1] = pVal[1];
  src_vector[2] = pVal[2];
  src_vector[3] = pVal[3];

  uint32* const destPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  SwapVectorBytes(src_vector);

  destPtr[0] = src_vector[0];
  destPtr[1] = src_vector[1];
  destPtr[2] = src_vector[2];
  destPtr[3] = src_vector[3];
}

void Execute_PushVectorRz(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 src_vector[4];

  mpe.sp -= 16;

  const uint32* const pVal = &(pRegs[nuance.fields[FIELD_MEM_FROM]]);

  src_vector[0] = pVal[0];
  src_vector[1] = pVal[1];
  src_vector[2] = pVal[2];
  src_vector[3] = pRegs[RZ_REG+0];

  uint32* const destPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  SwapVectorBytes(src_vector);

  destPtr[0] = src_vector[0];
  destPtr[1] = src_vector[1];
  destPtr[2] = src_vector[2];
  destPtr[3] = src_vector[3];
}

void Execute_PushScalarRzi1(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 src_vector[4];

  mpe.sp -= 16;

  src_vector[0] = pRegs[nuance.fields[FIELD_MEM_FROM]];
  src_vector[1] = mpe.tempCC;
  src_vector[2] = pRegs[RZ_REG+1];
  src_vector[3] = pRegs[RZ_REG+0];

  uint32* const destPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  SwapVectorBytes(src_vector);

  destPtr[0] = src_vector[0];
  destPtr[1] = src_vector[1];
  destPtr[2] = src_vector[2];
  destPtr[3] = src_vector[3];
}

void Execute_PushScalarRzi2(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 src_vector[4];

  mpe.sp -= 16;

  src_vector[0] = pRegs[nuance.fields[FIELD_MEM_FROM]];
  src_vector[1] = mpe.tempCC;
  src_vector[2] = pRegs[RZ_REG+2];
  src_vector[3] = pRegs[RZ_REG+0];

  uint32* const destPtr = (uint32 *)&(mpe.dtrom[mpe.sp & MPE_VALID_MEMORY_MASK]);

  SwapVectorBytes(src_vector);

  destPtr[0] = src_vector[0];
  destPtr[1] = src_vector[1];
  destPtr[2] = src_vector[2];
  destPtr[3] = src_vector[3];
}

void Execute_LoadScalarControlRegisterAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  mpe.regs[nuance.fields[FIELD_MEM_TO]] = mpe.ReadControlRegister(nuance.fields[FIELD_MEM_FROM] - MPE_CTRL_BASE, pRegs);
}

void Execute_LoadByteAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 data = *((uint8 *)nuance.fields[FIELD_MEM_POINTER]);
  data <<= 24;

  mpe.regs[nuance.fields[FIELD_MEM_TO]] = data;
}

void Execute_LoadWordAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 data = ((uint32)(*((uint8 *)nuance.fields[FIELD_MEM_POINTER]))) << 24;
  data |= ((uint32)( *((uint8 *)(nuance.fields[FIELD_MEM_POINTER]+1)) )) << 16;

  mpe.regs[nuance.fields[FIELD_MEM_TO]] = data;
}

void Execute_LoadScalarAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 data = *((uint32 *)nuance.fields[FIELD_MEM_POINTER]);
  SwapScalarBytes(&data);
  mpe.regs[nuance.fields[FIELD_MEM_TO]] = data;
}

void Execute_LoadScalarLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 dest = nuance.fields[FIELD_MEM_TO];
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_FROM]];

  if((address < MPE_CTRL_BASE) || (address >= MPE_RESV_BASE))
  {
    uint32 data;
    //The reserved area from $00000000 to $1FFFFFFF might always return zero
    //Freefall has a routine which will purposely read from $0 instead of skipping directly to
    //the epilogue code
    if(address >= MPE_ADDR_SPACE_BASE)
    {
      data = *((uint32 *)(nuonEnv.GetPointerToMemory(mpe,address & 0xFFFFFFFC)));
      SwapScalarBytes(&data);
    }
    else
    {
      data = 0;
    }

    mpe.regs[dest] = data;
  }
  else
  {
    mpe.regs[dest] = mpe.ReadControlRegister(address - MPE_CTRL_BASE, pRegs);
  }
}

void Execute_LoadVectorAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 * const srcPtr = (uint32 *)nuance.fields[FIELD_MEM_POINTER];

  uint32* const destPtr = &mpe.regs[nuance.fields[FIELD_MEM_TO]];

  destPtr[0] = srcPtr[0];
  destPtr[1] = srcPtr[1];
  destPtr[2] = srcPtr[2];
  destPtr[3] = srcPtr[3];

  SwapVectorBytes(destPtr);
}

void Execute_LoadVectorControlRegisterAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 dest = nuance.fields[FIELD_MEM_TO];
  const uint32 address = nuance.fields[FIELD_MEM_FROM];

  mpe.regs[dest    ] = mpe.ReadControlRegister(address      - MPE_CTRL_BASE, pRegs);
  mpe.regs[dest + 1] = mpe.ReadControlRegister(address + 4  - MPE_CTRL_BASE, pRegs);
  mpe.regs[dest + 2] = mpe.ReadControlRegister(address + 8  - MPE_CTRL_BASE, pRegs);
  mpe.regs[dest + 3] = mpe.ReadControlRegister(address + 12 - MPE_CTRL_BASE, pRegs);
}

void LoadPixelAbsolute(void)
{
  const uint32 control = bilinearAddressInfo.control;
  const void* const memPtr = bilinearAddressInfo.pPixelData;
  uint32* const regs = bilinearAddressInfo.pRegs;
  const uint32 pixType = BilinearInfo_XYType(control);
  const bool bChnorm = BilinearInfo_XYChnorm(control);

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
    {
      //4 bit
      //The initial xoffset is guaranteed to start at the first pixel of a group of four.  
      //This means that for even values of X, the pixel bits to be extracted are always [7:4]
      //and for odd values of X, the pixel bits to be extracted are [3:0]
      const uint32 pixelData32 = (*((uint8 *)memPtr) >> (4 - ((bilinearAddressInfo.mipped_xoffset & 1) << 2))) & 0x0FUL;
      regs[0] = (bilinearAddressInfo.clutBase & 0xFFFFFFC0UL) | (pixelData32 << 2);
      regs[1] = 0;
      regs[2] = 0;
      return;
    }
    case 0x2:
    case 0x5:
    {
      //16
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      regs[0] = (pixelData32 >> 2) & (0xFCUL << 22);
      regs[1] = (pixelData32 << 4) & (0xF8UL << 22);
      regs[2] = (pixelData32 << 9) & (0xF8UL << 22);

      if(bChnorm)
      {
        regs[1] = (regs[1] - 0x20000000UL) & 0xFE000000UL;
        regs[2] = (regs[2] - 0x20000000UL) & 0xFE000000UL;
      }
      return;
    }
    case 0x3:
    {
      //8 bit
      const uint32 pixelData32 = *((uint8 *)memPtr);
      regs[0] = (bilinearAddressInfo.clutBase & 0xFFFFFC00UL) | (pixelData32 << 2);
      regs[1] = 0;
      regs[2] = 0;
      return;
    }
    case 0x4:
    case 0x6:
    {
      //32 bit or 32+32Z (both behave the same for LD_P)
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      regs[0] = ((pixelData32 >> 2)) & (0xFFUL << 22);
      regs[1] = ((pixelData32 << 6)) & (0xFFUL << 22);
      regs[2] = ((pixelData32 << 14)) & (0xFFUL << 22);

      if(bChnorm)
      {
        regs[1] = (regs[1] - 0x20000000UL) & 0xFFC00000UL;
        regs[2] = (regs[2] - 0x20000000UL) & 0xFFC00000UL;
      }
      return;
    }
  }
}

void Execute_LoadPixelAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  void * const memPtr = (void *)((uint32 *)nuance.fields[FIELD_MEM_POINTER]);
  const uint32 dest = nuance.fields[FIELD_MEM_TO];

  uint32 pixType;
  bool bChnorm;
  if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_UV)
  {
    pixType = BilinearInfo_XYType(pRegs[UVC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[UVC_REG]);
  }
  else if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_XY)
  {
    pixType = BilinearInfo_XYType(pRegs[XYC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[XYC_REG]);
  }
  else
  {
    pixType = BilinearInfo_XYType(mpe.linpixctl);
    bChnorm = BilinearInfo_XYChnorm(mpe.linpixctl);
  }

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
    {
      //4 bit
      //The initial xoffset is guaranteed to start at the first pixel of a group of four.  
      //This means that for even values of X, the pixel bits to be extracted are always [7:4]
      //and for odd values of X, the pixel bits to be extracted are [3:0]
      const uint32 pixelData32 = (*((uint8 *)memPtr) >> (4 - ((mipped_xoffset & 1) << 2))) & 0x0FUL;
      mpe.regs[dest  ] = (mpe.clutbase & 0xFFFFFFC0UL) | (pixelData32 << 2);
      mpe.regs[dest+1] = 0;
      mpe.regs[dest+2] = 0;
      return;
    }
    case 0x2:
    case 0x5:
    {
      //16
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      mpe.regs[dest  ] = (pixelData32 >> 2) & (0xFCUL << 22);
      mpe.regs[dest+1] = (pixelData32 << 4) & (0xF8UL << 22);
      mpe.regs[dest+2] = (pixelData32 << 9) & (0xF8UL << 22);

      if(bChnorm)
      {
        mpe.regs[dest+1] = (mpe.regs[dest+1] - 0x20000000UL) & 0xFE000000UL;
        mpe.regs[dest+2] = (mpe.regs[dest+2] - 0x20000000UL) & 0xFE000000UL;
      }

      return;
    }
    case 0x3:
    {
      //8 bit
      const uint32 pixelData32 = *((uint8 *)memPtr);
      mpe.regs[dest  ] = (mpe.clutbase & 0xFFFFFC00UL) | (pixelData32 << 2);
      mpe.regs[dest+1] = 0;
      mpe.regs[dest+2] = 0;
      return;
    }
    case 0x4:
    case 0x6:
    {
      //32 bit or 32+32Z (both behave the same for LD_P)
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      mpe.regs[dest  ] = ((pixelData32 >> 2)) & (0xFFUL << 22);
      mpe.regs[dest+1] = ((pixelData32 << 6)) & (0xFFUL << 22);
      mpe.regs[dest+2] = ((pixelData32 << 14)) & (0xFFUL << 22);

      if(bChnorm)
      {
        mpe.regs[dest+1] = (mpe.regs[dest+1] - 0x20000000UL) & 0xFFC00000UL;
        mpe.regs[dest+2] = (mpe.regs[dest+2] - 0x20000000UL) & 0xFFC00000UL;
      }

      return;
    }
  }
}

void LoadPixelZAbsolute(void)
{
  const uint32 control = bilinearAddressInfo.control;
  const void* const memPtr = bilinearAddressInfo.pPixelData;
  uint32* const regs = bilinearAddressInfo.pRegs;
  const uint32 pixType = BilinearInfo_XYType(control);
  const bool bChnorm = BilinearInfo_XYChnorm(control);

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
      //4 bit
      return;
    case 0x2:
    {
      //16
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      regs[0] = (pixelData32 >> 2) & (0xFCUL << 22);
      regs[1] = (pixelData32 << 4) & (0xF8UL << 22);
      regs[2] = (pixelData32 << 9) & (0xF8UL << 22);

      if(bChnorm)
      {
        regs[1] = (regs[1] - 0x20000000UL) & 0xFE000000UL;
        regs[2] = (regs[2] - 0x20000000UL) & 0xFE000000UL;
      }

      return;
    }
    case 0x5:
    {
      //16+16Z
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      regs[0] = (pixelData32 >> 2) & (0xFCUL << 22);
      regs[1] = (pixelData32 << 4) & (0xF8UL << 22);
      regs[2] = (pixelData32 << 9) & (0xF8UL << 22);
      regs[3] = (pixelData32 << 16);

      if(bChnorm)
      {
        regs[1] = (regs[1] - 0x20000000UL) & 0xFE000000UL;
        regs[2] = (regs[2] - 0x20000000UL) & 0xFE000000UL;
      }

      return;
    }
    case 0x3:
      //8 bit
      return;
    case 0x4:
    {
      //32 bit
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      regs[0] = (pixelData32 >> 2) & (0xFFUL << 22);
      regs[1] = (pixelData32 << 6) & (0xFFUL << 22);
      regs[2] = (pixelData32 << 14) & (0xFFUL << 22);
      regs[3] = (pixelData32 << 24);

      if(bChnorm)
      {
        regs[1] = (regs[1] - 0x20000000UL) & 0xFFC00000UL;
        regs[2] = (regs[2] - 0x20000000UL) & 0xFFC00000UL;
      }

      return;
    }
    case 0x6:
    {
      uint32 pixelData32 = *((uint32 *)memPtr);
      uint32 zData32 = *(((uint32 *)memPtr) + 1);
      SwapScalarBytes(&pixelData32);
      SwapScalarBytes(&zData32);
      regs[0] = (pixelData32 >> 2) & (0xFFUL << 22);
      regs[1] = (pixelData32 << 6) & (0xFFUL << 22);
      regs[2] = (pixelData32 << 14) & (0xFFUL << 22);
      regs[3] = zData32;

      if(bChnorm)
      {
        regs[1] = (regs[1] - 0x20000000UL) & 0xFFC00000UL;
        regs[2] = (regs[2] - 0x20000000UL) & 0xFFC00000UL;
      }

      return;
    }
  }
}

void Execute_LoadPixelZAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  void * const memPtr = (void *)((uint32 *)nuance.fields[FIELD_MEM_POINTER]);
  const uint32 dest = nuance.fields[FIELD_MEM_TO];
  
  uint32 pixType;
  bool bChnorm;
  if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_UV)
  {
    pixType = BilinearInfo_XYType(pRegs[UVC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[UVC_REG]);
  }
  else if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_XY)
  {
    pixType = BilinearInfo_XYType(pRegs[XYC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[XYC_REG]);
  }
  else
  {
    pixType = BilinearInfo_XYType(mpe.linpixctl);
    bChnorm = BilinearInfo_XYChnorm(mpe.linpixctl);
  }

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
      //4 bit
      return;
    case 0x2:
    {
      //16
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      mpe.regs[dest  ] = (pixelData32 >> 2) & (0xFCUL << 22);
      mpe.regs[dest+1] = (pixelData32 << 4) & (0xF8UL << 22);
      mpe.regs[dest+2] = (pixelData32 << 9) & (0xF8UL << 22);

      if(bChnorm)
      {
        mpe.regs[dest+1] = (mpe.regs[dest+1] - 0x20000000UL) & 0xFE000000UL;
        mpe.regs[dest+2] = (mpe.regs[dest+2] - 0x20000000UL) & 0xFE000000UL;
      }

      return;
    }
    case 0x5:
    {
      //16+16Z
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);
      mpe.regs[dest  ] = (pixelData32 >> 2) & (0xFCUL << 22);
      mpe.regs[dest+1] = (pixelData32 << 4) & (0xF8UL << 22);
      mpe.regs[dest+2] = (pixelData32 << 9) & (0xF8UL << 22);
      mpe.regs[dest+3] = (pixelData32 << 16);

      if(bChnorm)
      {
        mpe.regs[dest+1] = (mpe.regs[dest+1] - 0x20000000UL) & 0xFE000000UL;
        mpe.regs[dest+2] = (mpe.regs[dest+2] - 0x20000000UL) & 0xFE000000UL;
      }

      return;
    }
    case 0x3:
      //8 bit
      return;
    case 0x4:
    {
      //32 bit
      uint32 pixelData32 = *((uint32 *)memPtr);
      SwapScalarBytes(&pixelData32);

      mpe.regs[dest  ] = (pixelData32 >> 2) & (0xFFUL << 22);
      mpe.regs[dest+1] = (pixelData32 << 6) & (0xFFUL << 22);
      mpe.regs[dest+2] = (pixelData32 << 14) & (0xFFUL << 22);
      mpe.regs[dest+3] = (pixelData32 << 24);

      if(bChnorm)
      {
        mpe.regs[dest+1] = (mpe.regs[dest+1] - 0x20000000UL) & 0xFFC00000UL;
        mpe.regs[dest+2] = (mpe.regs[dest+2] - 0x20000000UL) & 0xFFC00000UL;
      }

      return;
    }
    case 0x6:
    {
      uint32 pixelData32 = *((uint32 *)memPtr);
      uint32 zData32 = *(((uint32 *)memPtr) + 1);
      SwapScalarBytes(&pixelData32);
      SwapScalarBytes(&zData32);
      mpe.regs[dest  ] = (pixelData32 >> 2) & (0xFFUL << 22);
      mpe.regs[dest+1] = (pixelData32 << 6) & (0xFFUL << 22);
      mpe.regs[dest+2] = (pixelData32 << 14) & (0xFFUL << 22);
      mpe.regs[dest+3] = zData32;

      if(bChnorm)
      {
        mpe.regs[dest+1] = (mpe.regs[dest+1] - 0x20000000UL) & 0xFFC00000UL;
        mpe.regs[dest+2] = (mpe.regs[dest+2] - 0x20000000UL) & 0xFFC00000UL;
      }
      return;
    }
  }
}

void Execute_LoadByteLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_FROM]];

  const uint32 data = *((uint8 *)(nuonEnv.GetPointerToMemory(mpe,address)));
  mpe.regs[nuance.fields[FIELD_MEM_TO]] = data << 24;
}

void Execute_LoadByteBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadByteAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadByteAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadByteBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadByteAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadByteAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadWordLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 dest = nuance.fields[FIELD_MEM_TO];
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_FROM]];

  const uint8* const memPtr = (uint8 *)(nuonEnv.GetPointerToMemory(mpe,address & 0xFFFFFFFE));
  uint32 data = ((uint32)(*memPtr)) << 24;
  data |= ((uint32)(*(memPtr + 1))) << 16;

  mpe.regs[dest] = data;
}

void Execute_LoadWordBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadWordAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadWordAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadWordBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadWordAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadWordAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadScalarBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadScalarAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadScalarAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadScalarBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadScalarAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadScalarAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadShortVectorAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 data[4];
  const uint32 dest = nuance.fields[FIELD_MEM_TO];
  const uint8 * const ptr = (uint8 *)nuance.fields[FIELD_MEM_POINTER];

  data[0] = *((uint32 *)(ptr + 0));
  data[1] = *((uint32 *)(ptr + 2));
  data[2] = *((uint32 *)(ptr + 4));
  data[3] = *((uint32 *)(ptr + 6));
  SwapVectorBytes(data);
  data[0] &= 0xFFFF0000;
  data[1] &= 0xFFFF0000;
  data[2] &= 0xFFFF0000;
  data[3] &= 0xFFFF0000;

  mpe.regs[dest    ] = data[0];
  mpe.regs[dest + 1] = data[1];
  mpe.regs[dest + 2] = data[2];
  mpe.regs[dest + 3] = data[3];
}

void Execute_LoadShortVectorLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 data[4];
  const uint32 dest = nuance.fields[FIELD_MEM_TO];
  const uint8 * const ptr = (uint8 *)(nuonEnv.GetPointerToMemory(mpe,pRegs[nuance.fields[FIELD_MEM_FROM]] & 0xFFFFFFF8));
  
  data[0] = *((uint32 *)(ptr + 0));
  data[1] = *((uint32 *)(ptr + 2));
  data[2] = *((uint32 *)(ptr + 4));
  data[3] = *((uint32 *)(ptr + 6));
  SwapVectorBytes(data);
  data[0] &= 0xFFFF0000;
  data[1] &= 0xFFFF0000;
  data[2] &= 0xFFFF0000;
  data[3] &= 0xFFFF0000;

  mpe.regs[dest    ] = data[0];
  mpe.regs[dest + 1] = data[1];
  mpe.regs[dest + 2] = data[2];
  mpe.regs[dest + 3] = data[3];
}

void Execute_LoadShortVectorBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadShortVectorAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadShortVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadShortVectorBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadShortVectorAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)mpe.GetPointerToMemoryBank(address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadShortVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadVectorLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_FROM]];
  uint32 * const destPtr = &mpe.regs[nuance.fields[FIELD_MEM_TO]];
  const uint32 * const srcPtr = (uint32 *)(nuonEnv.GetPointerToMemory(mpe,address & 0xFFFFFFF0));

  if((address < MPE_CTRL_BASE) || (address >= MPE_RESV_BASE))
  {
    destPtr[0] = srcPtr[0];
    destPtr[1] = srcPtr[1];
    destPtr[2] = srcPtr[2];
    destPtr[3] = srcPtr[3];
    SwapVectorBytes(destPtr);
  }
  else
  {
    destPtr[0] = mpe.ReadControlRegister(address      - MPE_CTRL_BASE, pRegs);
    destPtr[1] = mpe.ReadControlRegister(address + 4  - MPE_CTRL_BASE, pRegs);
    destPtr[2] = mpe.ReadControlRegister(address + 8  - MPE_CTRL_BASE, pRegs);
    destPtr[3] = mpe.ReadControlRegister(address + 12 - MPE_CTRL_BASE, pRegs);
  }
}

void Execute_LoadVectorBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadVectorAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadVectorBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_HANDLER] = (size_t)Execute_LoadVectorAbsolute;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  Execute_LoadVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadPixelLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  address = pRegs[nuance.fields[FIELD_MEM_FROM]];
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_LINEAR_INDIRECT;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_LoadPixelAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadPixelBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  const int32 pixwidth = BilinearInfo_PixelWidth(pixel_type_width, pRegs[UVC_REG]);
  if(pixwidth >= 0)
  {
    address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixwidth);
  }
  else
  {
    //type1: 4-bit pixels
    address = (mpe.uvbase & 0xFFFFFFFC) + (address >> 1);
  }
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_LoadPixelAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadPixelBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  const int32 pixwidth = BilinearInfo_PixelWidth(pixel_type_width, pRegs[XYC_REG]);
  if(pixwidth >= 0)
  {
    address = (mpe.xybase & 0xFFFFFFFC) + (address << pixwidth);
  }
  else
  {
    //type1: 4-bit pixels
    address = (mpe.xybase & 0xFFFFFFFC) + (address >> 1);
  }
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_LoadPixelAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadPixelZLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_FROM]];
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_LINEAR_INDIRECT;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_LoadPixelZAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadPixelZBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << BilinearInfo_PixelWidth(pixel_type_width,pRegs[UVC_REG]));
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_LoadPixelZAbsolute(mpe,pRegs,newNuance);
}

void Execute_LoadPixelZBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << BilinearInfo_PixelWidth(pixel_type_width,pRegs[XYC_REG]));
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_TO] = nuance.fields[FIELD_MEM_TO];
  newNuance.fields[FIELD_MEM_FROM] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_LoadPixelZAbsolute(mpe,pRegs,newNuance);
}

void Execute_StoreScalarImmediate(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 * const destPtr = (uint32 *)nuance.fields[FIELD_MEM_POINTER];
  *destPtr = nuance.fields[FIELD_MEM_FROM];
  SwapScalarBytes(destPtr);
}

void Execute_StoreScalarAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 * const destPtr = (uint32 *)nuance.fields[FIELD_MEM_POINTER];
  *destPtr = pRegs[nuance.fields[FIELD_MEM_FROM]];
  SwapScalarBytes(destPtr);
}

void Execute_StoreScalarControlRegisterImmediate(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 reg = nuance.fields[FIELD_MEM_TO];
  //normal control register write
  mpe.WriteControlRegister(reg - MPE_CTRL_BASE, nuance.fields[FIELD_MEM_FROM]);
}

void Execute_StoreScalarLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_TO]] & 0xFFFFFFFC;
  if((address < MPE_ITAGS_BASE) || (address >= MPE_RESV_BASE))
  {
    uint32* const destPtr = (uint32 *)(nuonEnv.GetPointerToMemory(mpe,address));
    *destPtr = pRegs[nuance.fields[FIELD_MEM_FROM]];
    SwapScalarBytes(destPtr);
  }
  else
  {
    if((address & 0xFFFF0000) == MPE_ITAGS_BASE)
    {
      mpe.bInvalidateInstructionCaches = true;
    }
    else
    {
      mpe.WriteControlRegister(address - MPE_CTRL_BASE, pRegs[nuance.fields[FIELD_MEM_FROM]]);
    }
  }
}

void Execute_StoreScalarBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StoreScalarAbsolute(mpe,pRegs,newNuance);
}

void Execute_StoreScalarBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StoreScalarAbsolute(mpe,pRegs,newNuance);
}

void Execute_StoreScalarControlRegisterAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 reg = nuance.fields[FIELD_MEM_TO];
  if(reg != 0x20500FF0)
  {
    mpe.WriteControlRegister(reg - MPE_CTRL_BASE, pRegs[nuance.fields[FIELD_MEM_FROM]]);
  }
  else
  {
    //syscall
  }
}

void Execute_StoreVectorControlRegisterAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = nuance.fields[FIELD_MEM_TO];
  const uint32 * const srcPtr = &(pRegs[nuance.fields[FIELD_MEM_FROM]]);

  mpe.WriteControlRegister(address - MPE_CTRL_BASE, srcPtr[0]);
  mpe.WriteControlRegister(address + 4 - MPE_CTRL_BASE, srcPtr[1]);
  mpe.WriteControlRegister(address + 8 - MPE_CTRL_BASE, srcPtr[2]);
  mpe.WriteControlRegister(address + 12 - MPE_CTRL_BASE, srcPtr[3]);
}

void StorePixelAbsolute(void)
{
  const uint32 control = bilinearAddressInfo.control;
  const uint32 * const regs = bilinearAddressInfo.pRegs;

  void * const memPtr = (void *)bilinearAddressInfo.pPixelData;

  const uint32 pixType = BilinearInfo_XYType(control);
  const bool bChnorm = BilinearInfo_XYChnorm(control);

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
      //4 bit
      return;
    case 0x2:
    case 0x5:
    {
      //16 bit
      uint32 y32  = regs[0];
      uint32 cr32 = regs[1];
      uint32 cb32 = regs[2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint16 pixelData16 = ((y32 & 0xFC) << (15-7)) | ((cr32 & 0xF8) << (9-7)) | ((cb32 & 0xF8) >> 3);
      SwapWordBytes(&pixelData16);
      *((uint16 *)memPtr) = pixelData16;
      return;
    }
    case 0x3:
      //8 bit
      return;
    case 0x4:
    case 0x6:
    {
      //32 bit
      uint32 y32  = regs[0];
      uint32 cr32 = regs[1];
      uint32 cb32 = regs[2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint32 pixelData32 = *((uint32*)memPtr);
      SwapScalarBytes(&pixelData32);
      pixelData32 = (y32 << 24) | (cr32 << 16) | (cb32 << 8) | (pixelData32 & 0xFF);
      SwapScalarBytes(&pixelData32);
      *((uint32 *)memPtr) = pixelData32;
    }
  }
}

void Execute_StorePixelAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 src = nuance.fields[FIELD_MEM_FROM];
  const uint32 address = nuance.fields[FIELD_MEM_TO];
  void * const memPtr = (void *)(nuonEnv.GetPointerToMemory(mpe,address));

  uint32 pixType;
  bool bChnorm;
  if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_UV)
  {
    pixType = BilinearInfo_XYType(pRegs[UVC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[UVC_REG]);
  }
  else if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_XY)
  {
    pixType = BilinearInfo_XYType(pRegs[XYC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[XYC_REG]);
  }
  else
  {
    pixType = BilinearInfo_XYType(mpe.linpixctl);
    bChnorm = BilinearInfo_XYChnorm(mpe.linpixctl);
  }

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
      //4 bit
      return;
    case 0x2:
    case 0x5:
    {
      //16 bit
      uint32 y32  = pRegs[src  ];
      uint32 cr32 = pRegs[src+1];
      uint32 cb32 = pRegs[src+2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint16 pixelData16 = ((y32 & 0xFC) << (15-7)) | ((cr32 & 0xF8) << (9-7)) | ((cb32 & 0xF8) >> 3);
      SwapWordBytes(&pixelData16);
      *((uint16 *)memPtr) = pixelData16;
      return;
    }
    case 0x3:
      //8 bit
      return;
    case 0x4:
    case 0x6:
    {
      //32 bit
      uint32 y32  = pRegs[src  ];
      uint32 cr32 = pRegs[src+1];
      uint32 cb32 = pRegs[src+2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint32 pixelData32 = *((uint32*)memPtr);
      SwapScalarBytes(&pixelData32);
      pixelData32 = (y32 << 24) | (cr32 << 16) | (cb32 << 8) | (pixelData32 & 0xFF);
      SwapScalarBytes(&pixelData32);
      *((uint32 *)memPtr) = pixelData32;
    }
  }
}

void StorePixelZAbsolute(void)
{
  const uint32 control = bilinearAddressInfo.control;
  const uint32 * const regs = bilinearAddressInfo.pRegs;

  void * const memPtr = (void *)bilinearAddressInfo.pPixelData;

  const uint32 pixType = BilinearInfo_XYType(control);
  const bool bChnorm = BilinearInfo_XYChnorm(control);

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
      //4 bit
      return;
    case 0x2:
    {
      //16 bit
      uint32 y32  = regs[0];
      uint32 cr32 = regs[1];
      uint32 cb32 = regs[2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint16 pixelData16 = ((y32 & 0xFC) << (15-7)) | ((cr32 & 0xF8) << (9-7)) | ((cb32 & 0xF8) >> 3);
      SwapWordBytes(&pixelData16);
      *((uint16 *)memPtr) = pixelData16;
      return;
    }
    case 0x5:
    {
      //16 bit
      uint32 y32  = regs[0];
      uint32 cr32 = regs[1];
      uint32 cb32 = regs[2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint16 pixelData16 = ((y32 & 0xFC) << (15-7)) | ((cr32 & 0xF8) << (9-7)) | ((cb32 & 0xF8) >> 3);
      SwapWordBytes(&pixelData16);
      uint16 z16 = regs[3] >> 16;
      SwapWordBytes(&z16);
      *((uint16 *)memPtr) = pixelData16;
      *((uint16 *)memPtr + 1) = z16;
      return;
    }
    case 0x3:
      //8 bit
      return;
    case 0x4:
    {
      //32 bit
      uint32 y32  = regs[0];
      uint32 cr32 = regs[1];
      uint32 cb32 = regs[2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint32 pixelData32 = (y32 << 24) | (cr32 << 16) | (cb32 << 8) | (regs[3] >> 24);
      SwapScalarBytes(&pixelData32);
      *((uint32 *)memPtr) = pixelData32;
      return;
    }
    case 0x6:
    {
      //32+32Z
      uint32 y32  = regs[0];
      uint32 cr32 = regs[1];
      uint32 cb32 = regs[2];
      //uint32 z32 = pRegs[src+3];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint32 pixelData32 = (y32 << 24) | (cr32 << 16) | (cb32 << 8);
      SwapScalarBytes(&pixelData32);
      //SwapScalarBytes(&z32);
      *((uint32 *)memPtr) = pixelData32;
      //*(((uint32 *)memPtr) + 1) = z32;
      return;
    }
  }
}

void Execute_StorePixelZAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 src = nuance.fields[FIELD_MEM_FROM];
  const uint32 address = nuance.fields[FIELD_MEM_TO];
  //const uint32* const srcPtr = &(pRegs[src]);
  //uint16* const destPtr = (uint16 *)nuance.fields[FIELD_MEM_POINTER];

  void * const memPtr = (void *)(nuonEnv.GetPointerToMemory(mpe,address));

  uint32 pixType;
  bool bChnorm;
  if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_UV)
  {
    pixType = BilinearInfo_XYType(pRegs[UVC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[UVC_REG]);
  }
  else if(nuance.fields[FIELD_MEM_INFO] & MEM_INFO_BILINEAR_XY)
  {
    pixType = BilinearInfo_XYType(pRegs[XYC_REG]);
    bChnorm = BilinearInfo_XYChnorm(pRegs[XYC_REG]);
  }
  else
  {
    pixType = BilinearInfo_XYType(mpe.linpixctl);
    bChnorm = BilinearInfo_XYChnorm(mpe.linpixctl);
  }

  switch(pixType)
  {
    case 0x0:
      //MPEG
      return;
    case 0x1:
      //4 bit
      return;
    case 0x2:
    {
      //16 bit
      uint32 y32  = pRegs[src  ];
      uint32 cr32 = pRegs[src+1];
      uint32 cb32 = pRegs[src+2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint16 pixelData16 = ((y32 & 0xFC) << (15-7)) | ((cr32 & 0xF8) << (9-7)) | ((cb32 & 0xF8) >> 3);
      SwapWordBytes(&pixelData16);
      *((uint16 *)memPtr) = pixelData16;
      return;
    }
    case 0x5:
    {
      //16 bit
      uint32 y32  = pRegs[src  ];
      uint32 cr32 = pRegs[src+1];
      uint32 cb32 = pRegs[src+2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint16 pixelData16 = ((y32 & 0xFC) << (15-7)) | ((cr32 & 0xF8) << (9-7)) | ((cb32 & 0xF8) >> 3);
      SwapWordBytes(&pixelData16);
      uint16 z16 = pRegs[src+3] >> 16;
      SwapWordBytes(&z16);
      *((uint16 *)memPtr) = pixelData16;
      *((uint16 *)memPtr + 1) = z16;
      return;
    }
    case 0x3:
      //8 bit
      return;
    case 0x4:
    {
      //32 bit
      uint32 y32  = pRegs[src  ];
      uint32 cr32 = pRegs[src+1];
      uint32 cb32 = pRegs[src+2];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint32 pixelData32 = (y32 << 24) | (cr32 << 16) | (cb32 << 8) | (pRegs[src+3] >> 24);
      SwapScalarBytes(&pixelData32);
      *((uint32 *)memPtr) = pixelData32;
      return;
    }
    case 0x6:
    {
      //32+32Z
      uint32 y32  = pRegs[src  ];
      uint32 cr32 = pRegs[src+1];
      uint32 cb32 = pRegs[src+2];
      //uint32 z32 = pRegs[src+3];
      SaturateColorComponents(y32, cr32, cb32, bChnorm);

      uint32 pixelData32 = (y32 << 24) | (cr32 << 16) | (cb32 << 8);
      SwapScalarBytes(&pixelData32);
      //SwapScalarBytes(&z32);
      *((uint32 *)memPtr) = pixelData32;
      //*(((uint32 *)memPtr) + 1) = z32;
      return;
    }
  }
}

void Execute_StoreShortVectorAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const int32* const srcPtr = (int32 *)&pRegs[nuance.fields[FIELD_MEM_FROM]];
  int16 * const destPtr = (int16 *)nuance.fields[FIELD_MEM_POINTER];

  destPtr[0] = srcPtr[0] >> 16UL;
  destPtr[1] = srcPtr[1] >> 16UL;
  destPtr[2] = srcPtr[2] >> 16UL;
  destPtr[3] = srcPtr[3] >> 16UL;
  SwapShortVectorBytes((uint16 *)destPtr);
}

void Execute_StoreShortVectorLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32* const srcPtr = &pRegs[nuance.fields[FIELD_MEM_FROM]];
  uint16* const destPtr = (uint16 *)(nuonEnv.GetPointerToMemory(mpe,pRegs[nuance.fields[FIELD_MEM_TO]] & 0xFFFFFFF8));

  destPtr[0] = srcPtr[0] >> 16UL; //!! why is this unsigned and above signed
  destPtr[1] = srcPtr[1] >> 16UL;
  destPtr[2] = srcPtr[2] >> 16UL;
  destPtr[3] = srcPtr[3] >> 16UL;
  SwapShortVectorBytes(destPtr);
}

void Execute_StoreShortVectorBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StoreShortVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_StoreShortVectorBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StoreShortVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_StoreVectorAbsolute(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 *const destPtr = (uint32 *)nuance.fields[FIELD_MEM_POINTER];
  const uint32 src = nuance.fields[FIELD_MEM_FROM];
  destPtr[0] = pRegs[src];
  destPtr[1] = pRegs[src+1];
  destPtr[2] = pRegs[src+2];
  destPtr[3] = pRegs[src+3];
  SwapVectorBytes(destPtr);
}

void Execute_StoreVectorLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_TO]] & 0xFFFFFFF0;
  const uint32 * const srcPtr = &pRegs[nuance.fields[FIELD_MEM_FROM]];

  if((address < MPE_ITAGS_BASE) || (address >= MPE_RESV_BASE))
  {
    uint32 * const destPtr = (uint32 *)(nuonEnv.GetPointerToMemory(mpe,address));
    destPtr[0] = srcPtr[0];
    destPtr[1] = srcPtr[1];
    destPtr[2] = srcPtr[2];
    destPtr[3] = srcPtr[3];
    SwapVectorBytes(destPtr);
  }
  else
  {
    if((address & 0xFFFF0000) == MPE_ITAGS_BASE)
    {
      mpe.bInvalidateInstructionCaches = true;
    }
    else
    {
      mpe.WriteControlRegister(address      - MPE_CTRL_BASE, srcPtr[0]);
      mpe.WriteControlRegister(address + 4  - MPE_CTRL_BASE, srcPtr[1]);
      mpe.WriteControlRegister(address + 8  - MPE_CTRL_BASE, srcPtr[2]);
      mpe.WriteControlRegister(address + 12 - MPE_CTRL_BASE, srcPtr[3]);
    }
  }
}

void Execute_StoreVectorBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[UVC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StoreVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_StoreVectorBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << pixel_type_width[(pRegs[XYC_REG] >> 20) & 0x0FUL]);
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StoreVectorAbsolute(mpe,pRegs,newNuance);
}

void Execute_StorePixelLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_TO]];
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_LINEAR_INDIRECT;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StorePixelAbsolute(mpe,pRegs,newNuance);
}

void Execute_StorePixelBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << BilinearInfo_PixelWidth(pixel_type_width,pRegs[UVC_REG]));
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StorePixelAbsolute(mpe,pRegs,newNuance);
}

void Execute_StorePixelBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << BilinearInfo_PixelWidth(pixel_type_width,pRegs[XYC_REG]));
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StorePixelAbsolute(mpe,pRegs,newNuance);
}

void Execute_StorePixelZLinear(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  const uint32 address = pRegs[nuance.fields[FIELD_MEM_TO]];
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_LINEAR_INDIRECT;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StorePixelZAbsolute(mpe,pRegs,newNuance);
}

void Execute_StorePixelZBilinearUV(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[UVC_REG],pRegs[INDEX_REG+REG_U],pRegs[INDEX_REG+REG_V]);
  address = (mpe.uvbase & 0xFFFFFFFC) + (address << BilinearInfo_PixelWidth(pixel_type_width,pRegs[UVC_REG]));
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_UV;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StorePixelZAbsolute(mpe,pRegs,newNuance);
}

void Execute_StorePixelZBilinearXY(MPE &mpe, const uint32 pRegs[48], const Nuance &nuance)
{
  uint32 address;
  CalculateBilinearAddress(mpe,&address,pRegs[XYC_REG],pRegs[INDEX_REG+REG_X],pRegs[INDEX_REG+REG_Y]);
  address = (mpe.xybase & 0xFFFFFFFC) + (address << BilinearInfo_PixelWidth(pixel_type_width,pRegs[XYC_REG]));
  Nuance newNuance;
  newNuance.fields[FIELD_MEM_INFO] = MEM_INFO_BILINEAR_XY;
  newNuance.fields[FIELD_MEM_FROM] = nuance.fields[FIELD_MEM_FROM];
  newNuance.fields[FIELD_MEM_TO] = address;
  newNuance.fields[FIELD_MEM_POINTER] = (size_t)nuonEnv.GetPointerToMemory(mpe,address);
  Execute_StorePixelZAbsolute(mpe,pRegs,newNuance);
}
