/*
*      Copyright (C) 2005-2014 Team XBMC
*      http://www.xbmc.org
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with XBMC; see the file COPYING.  If not, see
*  <http://www.gnu.org/licenses/>.
*
*/

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
  #include "system.h"
#endif

#if defined(HAS_MARVELL_DOVE)

#include "utils/log.h"
#include "filesystem/File.h"
#include "threads/SystemClock.h"
#include "settings/GUISettings.h"

#include "XBTF.h"
#include "JpegHwDecVMETA.h"


#if 0
static void writeBitmap(const char *fileName,
                        unsigned numBits, const unsigned char *pixels,
                        unsigned width, unsigned height, unsigned pitch)
{
#pragma pack(1)
  struct
  {
    unsigned short  bfType;
    unsigned        bfSize;
    unsigned short  bfReserved1;
    unsigned short  bfReserved2;
    unsigned        bfOffBits;

    unsigned        biSize;
    unsigned        biWidth;
    unsigned        biHeight;
    unsigned short  biPlanes;
    unsigned short  biBitCount;
    unsigned        biCompression;
    unsigned        biSizeImage;
    int             biXPelsPerMeter;
    int             biYPelsPerMeter;
    unsigned        biClrUsed;
    unsigned        biClrImportant;
  } bmi;
#pragma pack()

  FILE        *fp;
  unsigned    u, w;

  memset( &bmi, 0, sizeof( bmi ) );
  bmi.biSize   = 40;
  bmi.biWidth  = width;
  bmi.biHeight = height;
  bmi.biPlanes = 1;
  bmi.biBitCount = ( unsigned short )numBits;

  w = ((( bmi.biBitCount * width ) + 31 ) & ~31 ) / 8;

  bmi.bfType = 'B' | ( 'M' << 8 );
  bmi.bfSize = sizeof( bmi ) + ( height * w );
  bmi.bfOffBits = sizeof( bmi );

  if(( fp = fopen( fileName, "wb" ) ) != NULL )
  {
    fwrite( &bmi, sizeof( bmi ), 1, fp );
    for( u = 1; u <= bmi.biHeight; ++u )
        fwrite( pixels + (bmi.biHeight - u) * pitch, w, 1, fp );
    fclose( fp );
  }
}
#endif




#define CLIP_RANGE  384 // worst case: BT.709, 320 should be sufficient

static uint32_t     clipByte1[CLIP_RANGE + 256 + CLIP_RANGE];
static uint32_t     clipByte0[CLIP_RANGE + 256 + CLIP_RANGE];
static uint32_t     clipByte2[CLIP_RANGE + 256 + CLIP_RANGE];
static uint32_t     clipByte1a[CLIP_RANGE + 256 + CLIP_RANGE];

static int32_t      yuvTab[3][256];
static bool         tabInit = false;


static void initClipTables()
{
  for (int i = 0; i < CLIP_RANGE + 256 + CLIP_RANGE; ++i)
  {
    uint32_t c;

    if (i < CLIP_RANGE)
      c = 0;
    else if (i >= CLIP_RANGE + 256)
      c = 255;
    else
      c = i - CLIP_RANGE;

    clipByte0[i]  = ( c <<  0 );                        // byte 0
    clipByte1[i]  = ( c <<  8 );                        // byte 1 for RGB
    clipByte1a[i] = ( c <<  8 ) | 0xff000000;           // byte 1 for BGRA
    clipByte2[i]  = ( c << 16 );                        // byte 2
  }
}


static void initYuvTables(int mode)
{
  static double coeffTable[][5] =
  {
    { 1.000, 1.402, 1.772, -0.34414, -0.71414 },        // JPEG
    { 1.164, 1.596, 2.018, -0.813,   -0.391   },        // BT.601
    { 1.164, 1.793, 2.115, -0.534,   -0.213   },        // BT.709
  };

  int   cy  = (int)(65536 * coeffTable[mode][0]);
  int   crv = (int)(65536 * coeffTable[mode][1]);
  int   cbu = (int)(65536 * coeffTable[mode][2]);
  int   cgu = (int)(65536 * coeffTable[mode][3]);
  int   cgv = (int)(65536 * coeffTable[mode][4]);

  for (int i = 0; i < 256; ++i)
  {
    if (mode == 0)
      yuvTab[0][i] = i + CLIP_RANGE;
    else
      yuvTab[0][i] = ((cy * (i-16) + 32768) >> 16 ) + CLIP_RANGE;

    yuvTab[1][i] = ((crv * (i-128) + 32768) & 0xffff0000) |
                    (((cgv * (i-128) + 32768) >> 16) & 0x0000ffff);

    yuvTab[2][i] = ((cbu * (i-128) + 32768) & 0xffff0000) |
                    (((cgu * (i-128) + 32768) >> 16) & 0x0000ffff);
  }
}


bool CJpegHwDecVMeta::Init()
{
  m_pCbTable = 0;
  m_pDecState = 0;

  memset(&m_input, 0, sizeof(m_input));
  memset(&m_picture, 0, sizeof(m_picture));
  memset(&m_VDecParSet, 0, sizeof(m_VDecParSet));

  if (!tabInit)
  {
    initClipTables();
    initYuvTables(0);
    tabInit = true;
  }

  if (m_HwLock.IsOwner() && m_DllVMETA.Load() && m_DllMiscGen.Load())
  {
    SetHardwareClock(g_guiSettings.GetInt("videoscreen.vmeta_clk"));

    if (m_DllMiscGen.miscInitGeneralCallbackTable(&m_pCbTable) == 0)
    {
      IppCodecStatus retCodec;

      m_VDecParSet.opt_fmt  = IPP_YCbCr422I;
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_JPEG;

      retCodec = m_DllVMETA.DecoderInitAlloc_Vmeta(&m_VDecParSet, m_pCbTable, &m_pDecState);
      if (retCodec == IPP_STATUS_NOERR)
        return true;

      CLog::Log(LOGERROR, "%s: DecoderInitAlloc_Vmeta failed (%d)", __FUNCTION__, retCodec);
    }
  }
  
  return false;
}


void CJpegHwDecVMeta::Dispose()
{
  if (m_pDecState)
  {
    m_DllVMETA.DecodeSendCmd_Vmeta(IPPVC_STOP_DECODE_STREAM, NULL, NULL, m_pDecState);

    DecodePopBuffers(IPP_VMETA_BUF_TYPE_PIC, ReleaseStorage);
    DecodePopBuffers(IPP_VMETA_BUF_TYPE_STRM, ReleaseBuffer);

    m_DllVMETA.DecoderFree_Vmeta(&m_pDecState);
  }

  FreeBuffer(0);

  if (m_pCbTable)
    m_DllMiscGen.miscFreeGeneralCallbackTable(&m_pCbTable);

  if (m_HwLock.IsOwner())
  {
    m_DllMiscGen.Unload();
    m_DllVMETA.Unload();

    SetHardwareClock(VMETA_CLK_500);
  }
}


unsigned int CJpegHwDecVMeta::FirstScale()
{
  return 2;
}


unsigned int CJpegHwDecVMeta::NextScale(unsigned int currScale, int direction)
{
  return (direction < 0) ?  (currScale >> (-direction)) : (currScale << direction);
}


unsigned char *CJpegHwDecVMeta::ReallocBuffer(unsigned char *buffer, unsigned int size)
{
  unsigned char         *pOldBuf  = m_input.pBuf;
  unsigned int          nOldSize  = m_input.nBufSize;

  (void)buffer;

  // add 256 bytes for padding
  size = (size + 256 + StreamBufAlloc - 1) & ~(StreamBufAlloc - 1);

  //CLog::Log(LOGNOTICE, "%s: %d -> %d", __FUNCTION__, nOldSize, size);
  if (size > nOldSize)
  {
    m_input.nPhyAddr = 0;
    m_input.pBuf = (Ipp8u *)m_DllVMETA.vdec_os_api_dma_alloc_cached(
                                       size, VMETA_STRM_BUF_ALIGN, &m_input.nPhyAddr);
    m_input.nBufSize = size;

    if (pOldBuf)
    {
      if (m_input.pBuf)
        memcpy(m_input.pBuf, pOldBuf, nOldSize);

      m_DllVMETA.vdec_os_api_dma_free(pOldBuf);
    }
  }
  
#if 0
  CLog::Log(LOGNOTICE, "%s: @%08x %d phys %08x", 
            __FUNCTION__, m_input.pBuf, m_input.nBufSize, m_input.nPhyAddr);
#endif

  return m_input.pBuf;
}


void CJpegHwDecVMeta::FreeBuffer(unsigned char *buffer)
{
  (void)buffer;

  //CLog::Log(LOGNOTICE, "%s: @%08x", __FUNCTION__, m_input.pBuf);

  if (m_input.pBuf)
  {
    m_DllVMETA.vdec_os_api_dma_free(m_input.pBuf);
    m_input.pBuf = 0;
    m_input.nPhyAddr = 0;
  }

  m_input.nFlag = 0;
  m_input.nDataLen = 0;
  m_input.nBufSize = 0;
}


void CJpegHwDecVMeta::PrepareBuffer(unsigned int numBytes)
{
  if (numBytes > m_input.nBufSize)
  {
    CLog::Log(LOGWARNING, "%s: PrepareBuffer numBytes > nBufSize", __FUNCTION__);
    numBytes = m_input.nBufSize;
  }

  m_input.nDataLen = numBytes;
  m_input.nFlag = IPP_VMETA_STRM_BUF_END_OF_UNIT;
  
  // append padding bytes (128 at least)
  numBytes = (numBytes + 255) & ~127;
  if (numBytes > m_input.nBufSize)
    numBytes = m_input.nBufSize;
  memset(m_input.pBuf + m_input.nDataLen, 0x88, numBytes - m_input.nDataLen);

  //m_DllVMETA.vdec_os_api_flush_cache(m_input.pBuf, numBytes, DMA_TODEVICE);
}


int CJpegHwDecVMeta::DecodePopBuffers(IppVmetaBufferType type, ReturnMode mode, int maxCount)
{
  int count;

  for (count = 0; count < maxCount; count++)
  {
    union { void *p; IppVmetaBitstream *strm; IppVmetaPicture *pic; };

    p = 0;
    m_DllVMETA.DecoderPopBuffer_Vmeta(type, &p, m_pDecState);

    if (!p)
      break;

    if (mode & ReleaseStorage)
    {
      switch (type)
      {
      case IPP_VMETA_BUF_TYPE_STRM:
        m_DllVMETA.vdec_os_api_dma_free(strm->pBuf);
        strm->pBuf = 0;
        break;

      case  IPP_VMETA_BUF_TYPE_PIC:
        m_DllVMETA.vdec_os_api_dma_free(pic->pBuf);
        pic->pBuf = 0;
        break;

      default:
        break;
      }
    }

    if (mode & ReleaseBuffer)
      ::free(p);
  }

  return count;
}


bool CJpegHwDecVMeta::DecodePicture(unsigned int maxWidth, 
                                    unsigned int maxHeight, 
                                    unsigned int scaleDivider)
{
  unsigned                    len;
  bool                        bExit;
  IppVmetaJPEGDecParSet       extParms;
  IppVmetaDecInfo             m_VDecInfo;
  int                         numSubmitted = 0;

#if 0
  CLog::Log(LOGNOTICE, "%s: width=%d, height=%d, divider=%d", 
            __FUNCTION__, maxWidth, maxHeight, scaleDivider);
#endif

  do
  {
    bExit = false;
    IppCodecStatus retCodec = m_DllVMETA.DecodeFrame_Vmeta(&m_VDecInfo, m_pDecState);

    switch (retCodec)
    {
    case IPP_STATUS_NEED_INPUT:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_NEED_INPUT");

      if (m_input.nOffset < m_input.nBufSize)
      {
        IppVmetaBitstream *pStream = (IppVmetaBitstream *)malloc(sizeof(IppVmetaBitstream));
        memset(pStream, 0, sizeof(IppVmetaBitstream));

        pStream->pBuf     = m_input.pBuf + m_input.nOffset;
        pStream->nPhyAddr = m_input.nPhyAddr + m_input.nOffset;

        len = m_input.nDataLen - m_input.nOffset;
        pStream->nDataLen = (len < StreamBufLimit) ?  len : StreamBufLimit;

        len = m_input.nBufSize - m_input.nOffset;
        pStream->nBufSize = (len < StreamBufLimit) ?  len : StreamBufLimit;

        if (pStream->nDataLen != pStream->nBufSize)
          pStream->nFlag = IPP_VMETA_STRM_BUF_END_OF_UNIT;

        m_input.nOffset += pStream->nBufSize;

#if 0
        CLog::Log(LOGNOTICE, "%s: @%08x %d (%d, %08x) phys %08x", __FUNCTION__, pStream->pBuf,
                  pStream->nDataLen, pStream->nBufSize, pStream->nBufSize, pStream->nPhyAddr);
#endif

        retCodec = m_DllVMETA.DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, pStream, m_pDecState);
        if (retCodec != IPP_STATUS_NOERR)
        {
          CLog::Log(LOGERROR, "%s: failure IPP_STATUS_NEED_INPUT %d", __FUNCTION__, retCodec);
          free(pStream);
          bExit = true;
        }
        else
        {
          numSubmitted++;
        }
      }
      else
      {
        //CLog::Log(LOGNOTICE, "%s: sending END_OF_STREAM", __FUNCTION__);
        m_DllVMETA.DecodeSendCmd_Vmeta(IPPVC_END_OF_STREAM, NULL, NULL, m_pDecState);
      }
      break;

    case IPP_STATUS_RETURN_INPUT_BUF:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_RETURN_INPUT_BUF");

      numSubmitted -= DecodePopBuffers(IPP_VMETA_BUF_TYPE_STRM, ReleaseBuffer, numSubmitted);
      break;

    case IPP_STATUS_NEED_OUTPUT_BUF:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_NEED_OUTPUT_BUF");

      if (!m_picture.pBuf)
      {
        m_picture.pBuf = (Ipp8u *)m_DllVMETA.vdec_os_api_dma_alloc(
                            m_VDecInfo.seq_info.dis_buf_size, VMETA_DIS_BUF_ALIGN, &m_picture.nPhyAddr);
        m_picture.nBufSize = m_VDecInfo.seq_info.dis_buf_size;
        m_picture.nDataLen = 0;

        //CLog::Log(LOGNOTICE, "IPP_STATUS_NEED_OUTPUT_BUF size: %d", m_picture.nBufSize);

        retCodec = m_DllVMETA.DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, &m_picture, m_pDecState);
        if (retCodec != IPP_STATUS_NOERR)
        {
          CLog::Log(LOGERROR, "%s: failure IPP_STATUS_NEED_OUTPUT_BUF %d", __FUNCTION__, retCodec);

          m_DllVMETA.vdec_os_api_dma_free(m_picture.pBuf);
          m_picture.pBuf = 0;
          bExit = true;
        }
      }
      break;

    case IPP_STATUS_FRAME_COMPLETE:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_FRAME_COMPLETE");

      DecodePopBuffers(IPP_VMETA_BUF_TYPE_PIC, ReleaseNothing, 1);

#if 0
      if (m_picture.pBuf)
      {
        CLog::Log(LOGNOTICE, "IPP_STATUS_FRAME_COMPLETE: output available %d x %d (%d)",
                  m_picture.pic.picWidth, m_picture.pic.picHeight, m_picture.nDataLen);
      }
#endif

      numSubmitted -= DecodePopBuffers(IPP_VMETA_BUF_TYPE_STRM, ReleaseBuffer, numSubmitted);
      bExit = true;
      break;

    case IPP_STATUS_END_OF_STREAM:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_END_OF_STREAM");

      numSubmitted -= DecodePopBuffers(IPP_VMETA_BUF_TYPE_STRM, ReleaseBuffer, numSubmitted);
      bExit = true;
      break;

    case IPP_STATUS_WAIT_FOR_EVENT:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_WAIT_FOR_EVENT");
      break;

    case IPP_STATUS_NEW_VIDEO_SEQ:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_NEW_VIDEO_SEQ");

      if (m_picture.pBuf)
      {
        DecodePopBuffers(IPP_VMETA_BUF_TYPE_PIC, ReleaseStorage, 1);
        m_picture.pBuf = 0;
      }
      
      extParms.pp_hscale = scaleDivider;
      extParms.pp_vscale = scaleDivider;
      extParms.roi.x = extParms.roi.y = extParms.roi.width = extParms.roi.height = 0;

      retCodec = m_DllVMETA.DecodeSendCmd_Vmeta(IPPVC_RECONFIG, &extParms, NULL, m_pDecState);
      if (retCodec != IPP_STATUS_NOERR)
      {
        CLog::Log(LOGERROR, "%s: failure IPP_STATUS_NEW_VIDEO_SEQ %d", __FUNCTION__, retCodec);
        bExit = true;
      }
      break;

    default:
      CLog::Log(LOGERROR, "%s: DecodeFrame_Vmeta returned %d", __FUNCTION__, retCodec);
      bExit = true;
    }

  } while (!bExit);

  return m_picture.pBuf != 0;
}


void CJpegHwDecVMeta::ToRGB(unsigned char *dst, unsigned int pitch, 
                            unsigned int width, unsigned int height)
{
#if 0
  CLog::Log(LOGNOTICE, "%s: %dx%d, %d bytes, pitch %d width %d height %d", __FUNCTION__,
            m_picture.pic.picWidth, m_picture.pic.picHeight, m_picture.nDataLen, pitch, width, height);
#endif

  uint32_t *p = (uint32_t *)m_picture.pBuf;
  unsigned skip = (m_picture.pic.picWidth - width) / 2;

  for (unsigned j = 0; j < height; ++j)
  {
    uint8_t     *q = (uint8_t *)dst;

    for (unsigned i = 0; i < width / 2; ++i, q += 6)
    {
      int32_t   cr, cb, cg, y;
      uint32_t  u1, u2, v = *p++;

      cr = yuvTab[1][(v >> 16) & 0xff];
      cb = yuvTab[2][(v >> 0) & 0xff];
      cg = (int16_t)cr + (int16_t)cb;
      cr >>= 16;
      cb >>= 16;

      y = yuvTab[0][(v >> 8) & 0xff];
      u1 = clipByte0[y + cr] | clipByte1[y + cg] | clipByte2[y + cb];

      y = yuvTab[0][(v >> 24) & 0xff];
      u2 = clipByte0[y + cr] | clipByte1[y + cg] | clipByte2[y + cb];

      *(uint32_t *)&q[0] = u1 | (u2 << 24);
      *(uint16_t *)&q[4] = (uint16_t)(u2 >> 8);
    }

    dst += pitch;
    p += skip;
  }

#if 0
  writeBitmap("/root/output3.bmp", 24, dst, m_picture.pic.picWidth, m_picture.pic.picHeight, pitch);
#endif
}


void CJpegHwDecVMeta::ToBGRA(unsigned char *dst, unsigned int pitch, 
                             unsigned int width, unsigned int height)
{
#if 0
  CLog::Log(LOGNOTICE, "%s: %dx%d, %d bytes, pitch %d width %d height %d", __FUNCTION__,
            m_picture.pic.picWidth, m_picture.pic.picHeight, m_picture.nDataLen, pitch, width, height);
#endif

  uint32_t *p = (uint32_t *)m_picture.pBuf;
  unsigned skip = (m_picture.pic.picWidth - width) / 2;
  
  for (unsigned j = 0; j < height; ++j)
  {
    uint32_t    *q = (uint32_t *)dst;

    for (unsigned i = 0; i < width / 2; ++i, q += 2)
    {
      int32_t   cr, cb, cg, y;
      uint32_t  v = *p++;
      
      cr = yuvTab[1][(v >> 16) & 0xff];
      cb = yuvTab[2][(v >> 0) & 0xff];
      cg = (int16_t)cr + (int16_t)cb;
      cr >>= 16;
      cb >>= 16;

      y = yuvTab[0][(v >> 8) & 0xff];
      q[0] = clipByte0[y + cb] | clipByte1a[y + cg] | clipByte2[y + cr];

      y = yuvTab[0][(v >> 24) & 0xff];
      q[1] = clipByte0[y + cb] | clipByte1a[y + cg] | clipByte2[y + cr];
    }

    dst += pitch;
    p += skip;
  }

#if 0
  writeBitmap("/root/output4.bmp", 32, dst, m_picture.pic.picWidth, m_picture.pic.picHeight, pitch);
#endif
}


bool CJpegHwDecVMeta::CanDecode(unsigned int width, unsigned int height) const
{
  // don't use hardware for small pictures ...
  return m_pDecState != 0 && width * height >= 100 * 100;
}


bool CJpegHwDecVMeta::Decode(unsigned char *dst, 
                             unsigned int pitch, unsigned int format,
                             unsigned int maxWidth, unsigned int maxHeight,
                             unsigned int scaleNum, unsigned int scaleDenom)
{
  bool bOk = false;

  if( DecodePicture(maxWidth, maxHeight, scaleDenom / scaleNum) )
  {
    if ((int)maxHeight > m_picture.pic.picHeight)
      maxHeight = m_picture.pic.picHeight;
    
    if ((int)maxWidth > m_picture.pic.picWidth)
      maxWidth = m_picture.pic.picWidth;
   
    if (format == XB_FMT_RGB8)
    {
      ToRGB(dst, pitch, maxWidth, maxHeight);
      bOk = true;
    }
    else if (format == XB_FMT_A8R8G8B8)
    {
      ToBGRA(dst, pitch, maxWidth, maxHeight);
      bOk = true;
    }
    else
    {
      CLog::Log(LOGWARNING, "%s: Incorrect output format specified", __FUNCTION__);
    }

    m_DllVMETA.vdec_os_api_dma_free(m_picture.pBuf);
    m_picture.pBuf     = 0;
    m_picture.nBufSize = 0;
    m_picture.nDataLen = 0;
  }

  return bOk;
}


void CJpegHwDecVMeta::SetHardwareClock(int clkRate)
{
  int clkFreqHz = (clkRate == VMETA_CLK_667) ? 667000000 : 500000000;

  FILE *Fh = fopen("/sys/devices/platform/dove_clocks_sysfs.0/vmeta","w");

  if (Fh != 0)
  {
    fprintf (Fh, "%d", clkFreqHz);
    fclose(Fh);
  }
  else
    CLog::Log(LOGERROR, "Unable to open vmeta clock settings file on sysfs");
}


#endif
