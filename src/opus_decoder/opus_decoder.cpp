
/* The CELT decoder is a development of the Xiph.Org Foundation
 * adapted to the ESP32 microcontroller, max 2 channels, no multistream
 *
 *  opusdecoder.cpp
 *
 *  Created on: Sep 01.2022
 *
 *  Updated on: Feb 06.2023
 *      Author: Wolle (schreibfaul1)
 */
//----------------------------------------------------------------------------------------------------------------------
//                                     O G G / O P U S     I M P L.
//----------------------------------------------------------------------------------------------------------------------
#include "opus_decoder.h"


// global vars
bool      f_m_subsequentPage = false;
bool      f_m_parseOgg = false;
bool      f_m_newSt = false;  // streamTitle
bool      f_m_opusFramePacket = false;
uint8_t   m_channels = 0;
uint16_t  m_samplerate = 0;
uint32_t  m_segmentLength = 0;
char     *m_chbuf = NULL;
int32_t   s_validSamples = 0;

uint16_t *m_segmentTable;
uint8_t   m_segmentTableSize = 0;
int8_t    error = 0;

bool OPUSDecoder_AllocateBuffers(){
    const uint32_t CELT_SET_END_BAND_REQUEST = 10012;
    const uint32_t CELT_SET_SIGNALLING_REQUEST = 10016;
    m_chbuf = (char*)malloc(512);
    if(!CELTDecoder_AllocateBuffers()) return false;
    m_segmentTable = (uint16_t*)malloc(256 * sizeof(uint16_t));
    if(!m_segmentTable) return false;
    CELTDecoder_ClearBuffer();
    OPUSDecoder_ClearBuffers();
    error = celt_decoder_init(2); if(error < 0) return false;
    error = celt_decoder_ctl(CELT_SET_SIGNALLING_REQUEST,  0); if(error < 0) return false;
    error = celt_decoder_ctl(CELT_SET_END_BAND_REQUEST,   21); if(error < 0) return false;
    return true;
}
void OPUSDecoder_FreeBuffers(){
    if(m_chbuf)        {free(m_chbuf);        m_chbuf = NULL;}
    if(m_segmentTable) {free(m_segmentTable); m_segmentTable = NULL;}
}
void OPUSDecoder_ClearBuffers(){
    if(m_chbuf)        memset(m_chbuf, 0, 512);
    if(m_segmentTable) memset(m_segmentTable, 0, 256);
}
//----------------------------------------------------------------------------------------------------------------------

int OPUSDecode(uint8_t *inbuf, int *bytesLeft, short *outbuf){

    if(f_m_parseOgg){
        int ret = OPUSparseOGG(inbuf, bytesLeft);
        if(ret == ERR_OPUS_NONE) return OPUS_PARSE_OGG_DONE; // ok
        else return ret;  // error
    }

    static int16_t segmentTableRdPtr = -1;

    if(f_m_opusFramePacket){
        if(m_segmentTableSize > 0){
            segmentTableRdPtr++;
            m_segmentTableSize--;
            int len = m_segmentTable[segmentTableRdPtr];
            *bytesLeft -= len;
            int32_t ret = parseOpusTOC(inbuf[0]);
            if(ret < 0) return ret;
            int frame_size = opus_packet_get_samples_per_frame(inbuf, 48000);
            len--;
            inbuf++;

            ec_dec_init((uint8_t *)inbuf, len);
            ret = celt_decode_with_ec(inbuf, len, (int16_t*)outbuf, frame_size);
            if(ret < 0) return ret; // celt error
            s_validSamples = ret;

            if(m_segmentTableSize== 0){
                segmentTableRdPtr = -1; // back to the parking position
                f_m_opusFramePacket = false;
                f_m_parseOgg = true;
            }
        }
    }
    return ERR_OPUS_NONE;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t opus_packet_get_samples_per_frame(const uint8_t *data, int32_t Fs) {
    int32_t audiosize;
    if (data[0] & 0x80) {
        audiosize = ((data[0] >> 3) & 0x3);
        audiosize = (Fs << audiosize) / 400;
    } else if ((data[0] & 0x60) == 0x60) {
        audiosize = (data[0] & 0x08) ? Fs / 50 : Fs / 100;
    } else {
        audiosize = ((data[0] >> 3) & 0x3);
        if (audiosize == 3)
            audiosize = Fs * 60 / 1000;
        else
            audiosize = (Fs << audiosize) / 100;
    }
    return audiosize;
}
//----------------------------------------------------------------------------------------------------------------------

uint8_t OPUSGetChannels(){
    return m_channels;
}
uint32_t OPUSGetSampRate(){
    return m_samplerate;
}
uint8_t OPUSGetBitsPerSample(){
    return 16;
}
uint32_t OPUSGetBitRate(){
    return 1;
}
uint16_t OPUSGetOutputSamps(){
    return s_validSamples; // 1024
}
char* OPUSgetStreamTitle(){
    if(f_m_newSt){
        f_m_newSt = false;
        return m_chbuf;
    }
    return NULL;
}
//----------------------------------------------------------------------------------------------------------------------
int parseOpusTOC(uint8_t TOC_Byte){  // https://www.rfc-editor.org/rfc/rfc6716  page 16 ff

    uint8_t mode = 0;
    static uint8_t oldmode = 0;
    uint8_t configNr = 0;
    uint8_t s = 0;
    uint8_t c = 0; (void)c;

    configNr = (TOC_Byte & 0b11111000) >> 3;
    s        = (TOC_Byte & 0b00000100) >> 2;
    c        = (TOC_Byte & 0b00000011);
    if(TOC_Byte & 0x80) mode = 2; else mode = 1;

    if(oldmode != mode) {
        oldmode = mode;
        if(mode == 2) log_i("opus mode is MODE_CELT_ONLY");
    }

    /*  Configuration       Mode  Bandwidth            FrameSizes         Audio Bandwidth   Sample Rate (Effective)
        configNr 16 ... 19  CELT  NB (narrowband)      2.5, 5, 10, 20ms   4 kHz             8 kHz
        configNr 20 ... 23  CELT  WB (wideband)        2.5, 5, 10, 20ms   8 kHz             16 kHz
        configNr 24 ... 27  CELT  SWB(super wideband)  2.5, 5, 10, 20ms   12 kHz            24 kHz
        configNr 28 ... 31  CELT  FB (fullband)        2.5, 5, 10, 20ms   20 kHz (*)        48 kHz     <-------

        (*) Although the sampling theorem allows a bandwidth as large as half the sampling rate, Opus never codes
        audio above 20 kHz, as that is the generally accepted upper limit of human hearing.

        s = 0: mono 1: stereo

        c = 0: 1 frame in the packet
        c = 1: 2 frames in the packet, each with equal compressed size
        c = 2: 2 frames in the packet, with different compressed sizes
        c = 3: an arbitrary number of frames in the packet
    */

    // log_i("configNr %i, s %i, c %i", configNr, s, c);

    if(configNr < 12) return ERR_OPUS_SILK_MODE_UNSUPPORTED;
    if(configNr < 16) return ERR_OPUS_HYBRID_MODE_UNSUPPORTED;

    return s;
}
//----------------------------------------------------------------------------------------------------------------------
int parseOpusComment(uint8_t *inbuf, int nBytes){      // reference https://exiftool.org/TagNames/Vorbis.html#Comments
                                                       // reference https://www.rfc-editor.org/rfc/rfc7845#section-5
    int idx = OPUS_specialIndexOf(inbuf, "OpusTags", 10);
     if(idx != 0) return 0; // is not OpusTags
    char* artist = NULL;
    char* title  = NULL;

    uint16_t pos = 8;
    uint32_t vendorLength       = *(inbuf + 11) << 24; // lengt of vendor string, e.g. Lavf58.65.101
             vendorLength      += *(inbuf + 10) << 16;
             vendorLength      += *(inbuf +  9) << 8;
             vendorLength      += *(inbuf +  8);
    pos += vendorLength + 4;
    uint32_t commentListLength  = *(inbuf + 3 + pos) << 24; // nr. of comment entries
             commentListLength += *(inbuf + 2 + pos) << 16;
             commentListLength += *(inbuf + 1 + pos) << 8;
             commentListLength += *(inbuf + 0 + pos);
    pos += 4;
    for(int i = 0; i < commentListLength; i++){
        uint32_t commentStringLen   = *(inbuf + 3 + pos) << 24;
                 commentStringLen  += *(inbuf + 2 + pos) << 16;
                 commentStringLen  += *(inbuf + 1 + pos) << 8;
                 commentStringLen  += *(inbuf + 0 + pos);
        pos += 4;
        idx = OPUS_specialIndexOf(inbuf + pos, "artist=", 10);
        if(idx == 0){ artist = strndup((const char*)(inbuf + pos + 7), commentStringLen - 7);
        }
        idx = OPUS_specialIndexOf(inbuf + pos, "title=", 10);
        if(idx == 0){ title = strndup((const char*)(inbuf + pos + 6), commentStringLen - 6);
        }
        pos += commentStringLen;
    }
    if(artist && title){
        strcpy(m_chbuf, artist);
        strcat(m_chbuf, " - ");
        strcat(m_chbuf, title);
        f_m_newSt = true;
    }
    else if(artist){
        strcpy(m_chbuf, artist);
        f_m_newSt = true;
    }
    else if(title){
        strcpy(m_chbuf, title);
        f_m_newSt = true;
    }
    if(artist){free(artist); artist = NULL;}
    if(title) {free(title);  title = NULL;}

    return 1;
}
//----------------------------------------------------------------------------------------------------------------------
int parseOpusHead(uint8_t *inbuf, int nBytes){  // reference https://wiki.xiph.org/OggOpus

    int idx = OPUS_specialIndexOf(inbuf, "OpusHead", 10);
     if(idx != 0) return 0; //is not OpusHead
    uint8_t  version            = *(inbuf +  8); (void) version;
    uint8_t  channelCount       = *(inbuf +  9); // nr of channels
    uint16_t preSkip            = *(inbuf + 11) << 8;
             preSkip           += *(inbuf + 10);
    uint32_t sampleRate         = *(inbuf + 15) << 24;  // informational only
             sampleRate        += *(inbuf + 14) << 16;
             sampleRate        += *(inbuf + 13) << 8;
             sampleRate        += *(inbuf + 12);
    uint16_t outputGain         = *(inbuf + 17) << 8;  // Q7.8 in dB
             outputGain        += *(inbuf + 16);
    uint8_t  channelMap         = *(inbuf + 18);

    if(channelCount == 0 or channelCount >2) return ERR_OPUS_NR_OF_CHANNELS_UNSUPPORTED;
    m_channels = channelCount;
    if(sampleRate != 48000) return ERR_OPUS_INVALID_SAMPLERATE;
    m_samplerate = sampleRate;
    if(channelMap > 1) return ERR_OPUS_EXTRA_CHANNELS_UNSUPPORTED;

    (void)outputGain;

    return 1;
}

//----------------------------------------------------------------------------------------------------------------------
int OPUSparseOGG(uint8_t *inbuf, int *bytesLeft){  // reference https://www.xiph.org/ogg/doc/rfc3533.txt

    f_m_parseOgg = false;
    int ret = 0;
    int idx = OPUS_specialIndexOf(inbuf, "OggS", 6);
    if(idx != 0) return ERR_OPUS_DECODER_ASYNC;

    static int16_t segmentTableWrPtr = -1;

    uint8_t  version            = *(inbuf +  4); (void) version;
    uint8_t  headerType         = *(inbuf +  5); (void) headerType;
    uint64_t granulePosition    = (uint64_t)*(inbuf + 13) << 56;  // granule_position: an 8 Byte field containing -
             granulePosition   += (uint64_t)*(inbuf + 12) << 48;  // position information. For an audio stream, it MAY
             granulePosition   += (uint64_t)*(inbuf + 11) << 40;  // contain the total number of PCM samples encoded
             granulePosition   += (uint64_t)*(inbuf + 10) << 32;  // after including all frames finished on this page.
             granulePosition   += *(inbuf +  9) << 24;  // This is a hint for the decoder and gives it some timing
             granulePosition   += *(inbuf +  8) << 16;  // and position information. A special value of -1 (in two's
             granulePosition   += *(inbuf +  7) << 8;   // complement) indicates that no packets finish on this page.
             granulePosition   += *(inbuf +  6); (void) granulePosition;
    uint32_t bitstreamSerialNr  = *(inbuf + 17) << 24;  // bitstream_serial_number: a 4 Byte field containing the
             bitstreamSerialNr += *(inbuf + 16) << 16;  // unique serial number by which the logical bitstream
             bitstreamSerialNr += *(inbuf + 15) << 8;   // is identified.
             bitstreamSerialNr += *(inbuf + 14); (void) bitstreamSerialNr;
    uint32_t pageSequenceNr     = *(inbuf + 21) << 24;  // page_sequence_number: a 4 Byte field containing the sequence
             pageSequenceNr    += *(inbuf + 20) << 16;  // number of the page so the decoder can identify page loss
             pageSequenceNr    += *(inbuf + 19) << 8;   // This sequence number is increasing on each logical bitstream
             pageSequenceNr    += *(inbuf + 18); (void) pageSequenceNr;
    uint32_t CRCchecksum        = *(inbuf + 25) << 24;
             CRCchecksum       += *(inbuf + 24) << 16;
             CRCchecksum       += *(inbuf + 23) << 8;
             CRCchecksum       += *(inbuf + 22); (void) CRCchecksum;
    uint8_t  pageSegments       = *(inbuf + 26);        // giving the number of segment entries

    // read the segment table (contains pageSegments bytes),  1...251: Length of the frame in bytes,
    // 255: A second byte is needed.  The total length is first_byte + second byte
    m_segmentLength = 0;
    segmentTableWrPtr = -1;

    for(int i = 0; i < pageSegments; i++){
        int n = *(inbuf + 27 + i);
        while(*(inbuf + 27 + i) == 255){
            i++;
            n+= *(inbuf + 27 + i);
        }
        segmentTableWrPtr++;
        m_segmentTable[segmentTableWrPtr] = n;
        m_segmentLength += n;
    }
    m_segmentTableSize = segmentTableWrPtr + 1;

    bool     continuedPage = headerType & 0x01; // set: page contains data of a packet continued from the previous page
    bool     firstPage     = headerType & 0x02; // set: this is the first page of a logical bitstream (bos)
    bool     lastPage      = headerType & 0x04; // set: this is the last page of a logical bitstream (eos)

    uint16_t headerSize    = pageSegments + 27;
    (void)continuedPage; (void)lastPage;
    *bytesLeft -= headerSize;

    if(firstPage || f_m_subsequentPage){ // OpusHead or OggComment may follows
        ret = parseOpusHead(inbuf + headerSize, m_segmentTable[0]);
        if(ret == 1) *bytesLeft -= m_segmentTable[0];
        if(ret < 0){ *bytesLeft -= m_segmentTable[0]; return ret;}
        ret = parseOpusComment(inbuf + headerSize, m_segmentTable[0]);
        if(ret == 1) *bytesLeft -= m_segmentTable[0];
        if(ret < 0){ *bytesLeft -= m_segmentTable[0]; return ret;}
        f_m_parseOgg = true;// goto next page
    }

    f_m_opusFramePacket = true;
    if(firstPage) f_m_subsequentPage = true; else f_m_subsequentPage = false;

    return ERR_OPUS_NONE; // no error
}

//----------------------------------------------------------------------------------------------------------------------
int OPUSFindSyncWord(unsigned char *buf, int nBytes){
    // assume we have a ogg wrapper
    int idx = OPUS_specialIndexOf(buf, "OggS", nBytes);
    if(idx >= 0){ // Magic Word found
        log_i("OggS found at %i", idx);
        f_m_parseOgg = true;
        return idx;
    }
    log_i("find sync");
    f_m_parseOgg = false;
    return ERR_OPUS_OGG_SYNC_NOT_FOUND;
}
//----------------------------------------------------------------------------------------------------------------------
int OPUS_specialIndexOf(uint8_t* base, const char* str, int baselen, bool exact){
    int result;  // seek for str in buffer or in header up to baselen, not nullterninated
    if (strlen(str) > baselen) return -1; // if exact == true seekstr in buffer must have "\0" at the end
    for (int i = 0; i < baselen - strlen(str); i++){
        result = i;
        for (int j = 0; j < strlen(str) + exact; j++){
            if (*(base + i + j) != *(str + j)){
                result = -1;
                break;
            }
        }
        if (result >= 0) break;
    }
    return result;
}



// based on Xiph.Org Foundation celt decoder

bool     OPUSDecoder_AllocateBuffers();
void     OPUSDecoder_FreeBuffers();
void     OPUSDecoder_ClearBuffers();
int      OPUSDecode(uint8_t *inbuf, int *bytesLeft, short *outbuf);
uint8_t  OPUSGetChannels();
uint32_t OPUSGetSampRate();
uint8_t  OPUSGetBitsPerSample();
uint32_t OPUSGetBitRate();
uint16_t OPUSGetOutputSamps();
char    *OPUSgetStreamTitle();
int      OPUSFindSyncWord(unsigned char *buf, int nBytes);
int      OPUSparseOGG(uint8_t *inbuf, int *bytesLeft);
int      parseOpusHead(uint8_t *inbuf, int nBytes);
int      parseOpusComment(uint8_t *inbuf, int nBytes);
int      parseOpusTOC(uint8_t TOC_Byte);
int32_t  opus_packet_get_samples_per_frame(const uint8_t *data, int32_t Fs);


//----------------------------------------------------------------------------------------------------------------------
//                                    C E L T     D E C O D E R
/*----------------------------------------------------------------------------------------------------------------------
   Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2010 Xiph.Org Foundation
   Copyright (c) 2008 Gregory Maxwell
   Written by Jean-Marc Valin and Gregory Maxwell

   Redistribution  and use in source  and binary forms,  with or without modification,  are permitted  provided that the
   following conditions are met:
   - Redistributions of  source  code must retain the above copyright notice,  this list of conditions and the following
   disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING,  BUT NOT LIMITED TO,  THE IMPLIED WARRANTIES OF  MERCHANTABILITY AND  FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED.  IN NO EVENT  SHALL THE COPYRIGHT  OWNER OR CONTRIBUTORS  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL,  EXEMPLARY,  OR CONSEQUENTIAL DAMAGES  (INCLUDING,  BUT  NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES;  LOSS OF USE,  DATA,  OR PROFITS;  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
----------------------------------------------------------------------------------------------------------------------*/

#include <Arduino.h>
#include "opus_decoder.h"

CELTDecoder  *cdec;
band_ctx_t    s_band_ctx;
ec_ctx_t      s_ec;
int32_t*      s_freqBuff;           // mem in celt_synthesis
int32_t*      s_iyBuff;             // mem in alg_unquant
int16_t*      s_normBuff;           // mem in quant_all_bands
int16_t*      s_XBuff;              // mem in celt_decode_with_ec
int32_t*      s_bits1Buff;          // mem in clt_compute_allocation
int32_t*      s_bits2Buff;          // mem in clt_compute_allocation
int32_t*      s_threshBuff;         // mem in clt_compute_allocation
int32_t*      s_trim_offsetBuff;    // mem in clt_compute_allocation
uint8_t*      s_collapse_masksBuff; // mem n celt_decode_with_ec
int16_t*      s_tmpBuff;            // mem in deinterleave_hadamard and interleave_hadamard

const uint32_t CELT_GET_AND_CLEAR_ERROR_REQUEST = 10007;
const uint32_t CELT_SET_CHANNELS_REQUEST        = 10008;
const uint32_t CELT_SET_END_BAND_REQUEST        = 10012;
const uint32_t CELT_GET_MODE_REQUEST            = 10015;
const uint32_t CELT_SET_SIGNALLING_REQUEST      = 10016;
const uint32_t CELT_SET_TONALITY_REQUEST        = 10018;
const uint32_t CELT_SET_TONALITY_SLOPE_REQUEST  = 10020;
const uint32_t CELT_SET_ANALYSIS_REQUEST        = 10022;
const uint32_t OPUS_SET_LFE_REQUEST             = 10024;
const uint32_t OPUS_SET_ENERGY_MASK_REQUEST     = 10026;

const uint8_t  EPSILON           = 1;
const uint8_t  BITRES            = 3;
const uint32_t PLC_PITCH_LAG_MAX = 720;
const uint8_t  PLC_PITCH_LAG_MIN = 100;
const uint32_t EC_SYM_BITS       = 8;
const uint8_t  EC_UINT_BITS      = 8;
const uint8_t  EC_WINDOW_SIZE    = 32;
const uint8_t  EC_CODE_BITS      = 32;
const uint8_t  EC_SYM_MAX        = 255;        // (1U << EC_SYM_BITS) - 1;
const uint32_t EC_CODE_TOP       = 2147483648; // 1U << (EC_CODE_BITS - 1);
const uint32_t EC_CODE_BOT       = 8388608;    // EC_CODE_TOP >> EC_SYM_BITS;
const uint8_t  EC_CODE_EXTRA     = 7;          // (EC_CODE_BITS-2) % EC_SYM_BITS + 1;

/*For each V(N,K) supported, we will access element U(min(N,K+1),max(N,K+1)). Thus, the number of entries in row I is
  the larger of the maximum number of pulses we will ever allocate for a given N=I (K=128, or however many fit in
  32 bits, whichever is smaller), plus one, and the maximum N for which K=I-1 pulses fit in 32 bits.
  The largest band size in an Opus Custom mode is 208. Otherwise, we can limit things to the set of N which can be
  achieved by splitting a band from a
  standard Opus mode: 176, 144, 96, 88, 72, 64, 48,44, 36, 32, 24, 22, 18, 16, 8, 4, 2).*/

static const uint32_t CELT_PVQ_U_DATA[1272] = {

    /*N=0, K=0...176:*/
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

    /*N=1, K=1...176:*/
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    /*N=2, K=2...176:*/
    3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61,
    63, 65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 99, 101, 103, 105, 107, 109, 111, 113, 115,
    117, 119, 121, 123, 125, 127, 129, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149, 151, 153, 155, 157, 159, 161,
    163, 165, 167, 169, 171, 173, 175, 177, 179, 181, 183, 185, 187, 189, 191, 193, 195, 197, 199, 201, 203, 205, 207,
    209, 211, 213, 215, 217, 219, 221, 223, 225, 227, 229, 231, 233, 235, 237, 239, 241, 243, 245, 247, 249, 251, 253,
    255, 257, 259, 261, 263, 265, 267, 269, 271, 273, 275, 277, 279, 281, 283, 285, 287, 289, 291, 293, 295, 297, 299,
    301, 303, 305, 307, 309, 311, 313, 315, 317, 319, 321, 323, 325, 327, 329, 331, 333, 335, 337, 339, 341, 343, 345,
    347, 349, 351,

    /*N=3, K=3...176:*/
    13, 25, 41, 61, 85, 113, 145, 181, 221, 265, 313, 365, 421, 481, 545, 613, 685, 761, 841, 925, 1013, 1105, 1201,
    1301, 1405, 1513, 1625, 1741, 1861, 1985, 2113, 2245, 2381, 2521, 2665, 2813, 2965, 3121, 3281, 3445, 3613, 3785,
    3961, 4141, 4325, 4513, 4705, 4901, 5101, 5305, 5513, 5725, 5941, 6161, 6385, 6613, 6845, 7081, 7321, 7565, 7813,
    8065, 8321, 8581, 8845, 9113, 9385, 9661, 9941, 10225, 10513, 10805, 11101, 11401, 11705, 12013, 12325, 12641,
    12961, 13285, 13613, 13945, 14281, 14621, 14965, 15313, 15665, 16021, 16381, 16745, 17113, 17485, 17861, 18241,
    18625, 19013, 19405, 19801, 20201, 20605, 21013, 21425, 21841, 22261, 22685, 23113, 23545, 23981, 24421, 24865,
    25313, 25765, 26221, 26681, 27145, 27613, 28085, 28561, 29041, 29525, 30013, 30505, 31001, 31501, 32005, 32513,
    33025, 33541, 34061, 34585, 35113, 35645, 36181, 36721, 37265, 37813, 38365, 38921, 39481, 40045, 40613, 41185,
    41761, 42341, 42925, 43513, 44105, 44701, 45301, 45905, 46513, 47125, 47741, 48361, 48985, 49613, 50245, 50881,
    51521, 52165, 52813, 53465, 54121, 54781, 55445, 56113, 56785, 57461, 58141, 58825, 59513, 60205, 60901, 61601,

    /*N=4, K=4...176:*/
    63, 129, 231, 377, 575, 833, 1159, 1561, 2047, 2625, 3303, 4089, 4991, 6017, 7175, 8473, 9919, 11521, 13287, 15225,
    17343, 19649, 22151, 24857, 27775, 30913, 34279, 37881, 41727, 45825, 50183, 54809, 59711, 64897, 70375, 76153,
    82239, 88641, 95367, 102425, 109823, 117569, 125671, 134137, 142975, 152193, 161799, 171801, 182207, 193025, 204263,
    215929, 228031, 240577, 253575, 267033, 280959, 295361, 310247, 325625, 341503, 357889, 374791, 392217, 410175,
    428673, 447719, 467321, 487487, 508225, 529543, 551449, 573951, 597057, 620775, 645113, 670079, 695681, 721927,
    748825, 776383, 804609, 833511, 863097, 893375, 924353, 956039, 988441, 1021567, 1055425, 1090023, 1125369, 1161471,
    1198337, 1235975, 1274393, 1313599, 1353601, 1394407, 1436025, 1478463, 1521729, 1565831, 1610777, 1656575, 1703233,
    1750759, 1799161, 1848447, 1898625, 1949703, 2001689, 2054591, 2108417, 2163175, 2218873, 2275519, 2333121, 2391687,
    2451225, 2511743, 2573249, 2635751, 2699257, 2763775, 2829313, 2895879, 2963481, 3032127, 3101825, 3172583, 3244409,
    3317311, 3391297, 3466375, 3542553, 3619839, 3698241, 3777767, 3858425, 3940223, 4023169, 4107271, 4192537, 4278975,
    4366593, 4455399, 4545401, 4636607, 4729025, 4822663, 4917529, 5013631, 5110977, 5209575, 5309433, 5410559, 5512961,
    5616647, 5721625, 5827903, 5935489, 6044391, 6154617, 6266175, 6379073, 6493319, 6608921, 6725887, 6844225, 6963943,
    7085049, 7207551,

    /*N=5, K=5...176:*/
    321, 681, 1289, 2241, 3649, 5641, 8361, 11969, 16641, 22569, 29961, 39041, 50049, 63241, 78889, 97281, 118721,
    143529, 172041, 204609, 241601, 283401, 330409, 383041, 441729, 506921, 579081, 658689, 746241, 842249, 947241,
    1061761, 1186369, 1321641, 1468169, 1626561, 1797441, 1981449, 2179241, 2391489, 2618881, 2862121, 3121929, 3399041,
    3694209, 4008201, 4341801, 4695809, 5071041, 5468329, 5888521, 6332481, 6801089, 7295241, 7815849, 8363841, 8940161,
    9545769, 10181641, 10848769, 11548161, 12280841, 13047849, 13850241, 14689089, 15565481, 16480521, 17435329,
    18431041, 19468809, 20549801, 21675201, 22846209, 24064041, 25329929, 26645121, 28010881, 29428489, 30899241,
    32424449, 34005441, 35643561, 37340169, 39096641, 40914369, 42794761, 44739241, 46749249, 48826241, 50971689,
    53187081, 55473921, 57833729, 60268041, 62778409, 65366401, 68033601, 70781609, 73612041, 76526529, 79526721,
    82614281, 85790889, 89058241, 92418049, 95872041, 99421961, 103069569, 106816641, 110664969, 114616361, 118672641,
    122835649, 127107241, 131489289, 135983681, 140592321, 145317129, 150160041, 155123009, 160208001, 165417001,
    170752009, 176215041, 181808129, 187533321, 193392681, 199388289, 205522241, 211796649, 218213641, 224775361,
    231483969, 238341641, 245350569, 252512961, 259831041, 267307049, 274943241, 282741889, 290705281, 298835721,
    307135529, 315607041, 324252609, 333074601, 342075401, 351257409, 360623041, 370174729, 379914921, 389846081,
    399970689, 410291241, 420810249, 431530241, 442453761, 453583369, 464921641, 476471169, 488234561, 500214441,
    512413449, 524834241, 537479489, 550351881, 563454121, 576788929, 590359041, 604167209, 618216201, 632508801,

    /*N=6, K=6...96:*/
    1683, 3653, 7183, 13073, 22363, 36365, 56695, 85305, 124515, 177045, 246047, 335137, 448427, 590557, 766727, 982729,
    1244979, 1560549, 1937199, 2383409, 2908411, 3522221, 4235671, 5060441, 6009091, 7095093, 8332863, 9737793,
    11326283, 13115773, 15124775, 17372905, 19880915, 22670725, 25765455, 29189457, 32968347, 37129037, 41699767,
    46710137, 52191139, 58175189, 64696159, 71789409, 79491819, 87841821, 96879431, 106646281, 117185651, 128542501,
    140763503, 153897073, 167993403, 183104493, 199284183, 216588185, 235074115, 254801525, 275831935, 298228865,
    322057867, 347386557, 374284647, 402823977, 433078547, 465124549, 499040399, 534906769, 572806619, 612825229,
    655050231, 699571641, 746481891, 795875861, 847850911, 902506913, 959946283, 1020274013, 1083597703, 1150027593,
    1219676595, 1292660325, 1369097135, 1449108145, 1532817275, 1620351277, 1711839767, 1807415257, 1907213187,
    2011371957, 2120032959,

    /*N=7, K=7...54*/
    8989, 19825, 40081, 75517, 134245, 227305, 369305, 579125, 880685, 1303777, 1884961, 2668525, 3707509, 5064793,
    6814249, 9041957, 11847485, 15345233, 19665841, 24957661, 31388293, 39146185, 48442297, 59511829, 72616013,
    88043969, 106114625, 127178701, 151620757, 179861305, 212358985, 249612805, 292164445, 340600625, 395555537,
    457713341, 527810725, 606639529, 695049433, 793950709, 904317037, 1027188385, 1163673953, 1314955181, 1482288821,
    1667010073, 1870535785, 2094367717,

    /*N=8, K=8...37*/
    48639, 108545, 224143, 433905, 795455, 1392065, 2340495, 3800305, 5984767, 9173505, 13726991, 20103025, 28875327,
    40754369, 56610575, 77500017, 104692735, 139703809, 184327311, 240673265, 311207743, 398796225, 506750351,
    638878193, 799538175, 993696769, 1226990095, 1505789553, 1837271615, 2229491905U,

    /*N=9, K=9...28:*/
    265729, 598417, 1256465, 2485825, 4673345, 8405905, 14546705, 24331777, 39490049, 62390545, 96220561, 145198913,
    214828609, 312193553, 446304145, 628496897, 872893441, 1196924561, 1621925137, 2173806145U,

    /*N=10, K=10...24:*/
    1462563, 3317445, 7059735, 14218905, 27298155, 50250765, 89129247, 152951073, 254831667, 413442773, 654862247,
    1014889769, 1541911931, 2300409629U, 3375210671U,
    /*N=11, K=11...19:*/
    8097453, 18474633, 39753273, 81270333, 158819253, 298199265, 540279585, 948062325, 1616336765,

    /*N=12, K=12...18:*/
    45046719, 103274625, 224298231, 464387817, 921406335, 1759885185, 3248227095U,
    /*N=13, K=13...16:*/
    251595969, 579168825, 1267854873, 2653649025U,
    /*N=14, K=14:*/
    1409933619};

static const uint8_t band_allocation[] = {
    /*0  200 400 600 800  1k 1.2 1.4 1.6  2k 2.4 2.8 3.2  4k 4.8 5.6 6.8  8k 9.6 12k 15.6 */
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    90,  80,  75,  69,  63,  56,  49,  40,  34,  29,  20,  18,  10,  0,   0,   0,   0,   0,   0,   0,   0,
    110, 100, 90,  84,  78,  71,  65,  58,  51,  45,  39,  32,  26,  20,  12,  0,   0,   0,   0,   0,   0,
    118, 110, 103, 93,  86,  80,  75,  70,  65,  59,  53,  47,  40,  31,  23,  15,  4,   0,   0,   0,   0,
    126, 119, 112, 104, 95,  89,  83,  78,  72,  66,  60,  54,  47,  39,  32,  25,  17,  12,  1,   0,   0,
    134, 127, 120, 114, 103, 97,  91,  85,  78,  72,  66,  60,  54,  47,  41,  35,  29,  23,  16,  10,  1,
    144, 137, 130, 124, 113, 107, 101, 95,  88,  82,  76,  70,  64,  57,  51,  45,  39,  33,  26,  15,  1,
    152, 145, 138, 132, 123, 117, 111, 105, 98,  92,  86,  80,  74,  67,  61,  55,  49,  43,  36,  20,  1,
    162, 155, 148, 142, 133, 127, 121, 115, 108, 102, 96,  90,  84,  77,  71,  65,  59,  53,  46,  30,  1,
    172, 165, 158, 152, 143, 137, 131, 125, 118, 112, 106, 100, 94,  87,  81,  75,  69,  63,  56,  45,  20,
    200, 200, 200, 200, 200, 200, 200, 200, 198, 193, 188, 183, 178, 173, 168, 163, 158, 153, 148, 129, 104,
};

static const int16_t eband5ms[] = {
/*0  200 400 600 800  1k 1.2 1.4 1.6  2k 2.4 2.8 3.2  4k 4.8 5.6 6.8  8k 9.6 12k 15.6 */
  0,  1,  2,  3,  4,  5,  6,  7,  8, 10, 12, 14, 16, 20, 24, 28, 34, 40, 48, 60, 78, 100
};

static const int16_t mdct_twiddles960[1800] = {
    32767,  32767,  32767,  32766,  32765,  32763,  32761,  32759,  32756,  32753,  32750,  32746,  32742,  32738,
    32733,  32728,  32722,  32717,  32710,  32704,  32697,  32690,  32682,  32674,  32666,  32657,  32648,  32639,
    32629,  32619,  32609,  32598,  32587,  32576,  32564,  32552,  32539,  32526,  32513,  32500,  32486,  32472,
    32457,  32442,  32427,  32411,  32395,  32379,  32362,  32345,  32328,  32310,  32292,  32274,  32255,  32236,
    32217,  32197,  32177,  32157,  32136,  32115,  32093,  32071,  32049,  32027,  32004,  31981,  31957,  31933,
    31909,  31884,  31859,  31834,  31809,  31783,  31756,  31730,  31703,  31676,  31648,  31620,  31592,  31563,
    31534,  31505,  31475,  31445,  31415,  31384,  31353,  31322,  31290,  31258,  31226,  31193,  31160,  31127,
    31093,  31059,  31025,  30990,  30955,  30920,  30884,  30848,  30812,  30775,  30738,  30701,  30663,  30625,
    30587,  30548,  30509,  30470,  30430,  30390,  30350,  30309,  30269,  30227,  30186,  30144,  30102,  30059,
    30016,  29973,  29930,  29886,  29842,  29797,  29752,  29707,  29662,  29616,  29570,  29524,  29477,  29430,
    29383,  29335,  29287,  29239,  29190,  29142,  29092,  29043,  28993,  28943,  28892,  28842,  28791,  28739,
    28688,  28636,  28583,  28531,  28478,  28425,  28371,  28317,  28263,  28209,  28154,  28099,  28044,  27988,
    27932,  27876,  27820,  27763,  27706,  27648,  27591,  27533,  27474,  27416,  27357,  27298,  27238,  27178,
    27118,  27058,  26997,  26936,  26875,  26814,  26752,  26690,  26628,  26565,  26502,  26439,  26375,  26312,
    26247,  26183,  26119,  26054,  25988,  25923,  25857,  25791,  25725,  25658,  25592,  25524,  25457,  25389,
    25322,  25253,  25185,  25116,  25047,  24978,  24908,  24838,  24768,  24698,  24627,  24557,  24485,  24414,
    24342,  24270,  24198,  24126,  24053,  23980,  23907,  23834,  23760,  23686,  23612,  23537,  23462,  23387,
    23312,  23237,  23161,  23085,  23009,  22932,  22856,  22779,  22701,  22624,  22546,  22468,  22390,  22312,
    22233,  22154,  22075,  21996,  21916,  21836,  21756,  21676,  21595,  21515,  21434,  21352,  21271,  21189,
    21107,  21025,  20943,  20860,  20777,  20694,  20611,  20528,  20444,  20360,  20276,  20192,  20107,  20022,
    19937,  19852,  19767,  19681,  19595,  19509,  19423,  19336,  19250,  19163,  19076,  18988,  18901,  18813,
    18725,  18637,  18549,  18460,  18372,  18283,  18194,  18104,  18015,  17925,  17835,  17745,  17655,  17565,
    17474,  17383,  17292,  17201,  17110,  17018,  16927,  16835,  16743,  16650,  16558,  16465,  16372,  16279,
    16186,  16093,  15999,  15906,  15812,  15718,  15624,  15529,  15435,  15340,  15245,  15150,  15055,  14960,
    14864,  14769,  14673,  14577,  14481,  14385,  14288,  14192,  14095,  13998,  13901,  13804,  13706,  13609,
    13511,  13414,  13316,  13218,  13119,  13021,  12923,  12824,  12725,  12626,  12527,  12428,  12329,  12230,
    12130,  12030,  11930,  11831,  11730,  11630,  11530,  11430,  11329,  11228,  11128,  11027,  10926,  10824,
    10723,  10622,  10520,  10419,  10317,  10215,  10113,  10011,  9909,   9807,   9704,   9602,   9499,   9397,
    9294,   9191,   9088,   8985,   8882,   8778,   8675,   8572,   8468,   8364,   8261,   8157,   8053,   7949,
    7845,   7741,   7637,   7532,   7428,   7323,   7219,   7114,   7009,   6905,   6800,   6695,   6590,   6485,
    6380,   6274,   6169,   6064,   5958,   5853,   5747,   5642,   5536,   5430,   5325,   5219,   5113,   5007,
    4901,   4795,   4689,   4583,   4476,   4370,   4264,   4157,   4051,   3945,   3838,   3732,   3625,   3518,
    3412,   3305,   3198,   3092,   2985,   2878,   2771,   2664,   2558,   2451,   2344,   2237,   2130,   2023,
    1916,   1809,   1702,   1594,   1487,   1380,   1273,   1166,   1059,   952,    844,    737,    630,    523,
    416,    308,    201,    94,     -13,    -121,   -228,   -335,   -442,   -550,   -657,   -764,   -871,   -978,
    -1086,  -1193,  -1300,  -1407,  -1514,  -1621,  -1728,  -1835,  -1942,  -2049,  -2157,  -2263,  -2370,  -2477,
    -2584,  -2691,  -2798,  -2905,  -3012,  -3118,  -3225,  -3332,  -3439,  -3545,  -3652,  -3758,  -3865,  -3971,
    -4078,  -4184,  -4290,  -4397,  -4503,  -4609,  -4715,  -4821,  -4927,  -5033,  -5139,  -5245,  -5351,  -5457,
    -5562,  -5668,  -5774,  -5879,  -5985,  -6090,  -6195,  -6301,  -6406,  -6511,  -6616,  -6721,  -6826,  -6931,
    -7036,  -7140,  -7245,  -7349,  -7454,  -7558,  -7663,  -7767,  -7871,  -7975,  -8079,  -8183,  -8287,  -8390,
    -8494,  -8597,  -8701,  -8804,  -8907,  -9011,  -9114,  -9217,  -9319,  -9422,  -9525,  -9627,  -9730,  -9832,
    -9934,  -10037, -10139, -10241, -10342, -10444, -10546, -10647, -10748, -10850, -10951, -11052, -11153, -11253,
    -11354, -11455, -11555, -11655, -11756, -11856, -11955, -12055, -12155, -12254, -12354, -12453, -12552, -12651,
    -12750, -12849, -12947, -13046, -13144, -13242, -13340, -13438, -13536, -13633, -13731, -13828, -13925, -14022,
    -14119, -14216, -14312, -14409, -14505, -14601, -14697, -14793, -14888, -14984, -15079, -15174, -15269, -15364,
    -15459, -15553, -15647, -15741, -15835, -15929, -16023, -16116, -16210, -16303, -16396, -16488, -16581, -16673,
    -16766, -16858, -16949, -17041, -17133, -17224, -17315, -17406, -17497, -17587, -17678, -17768, -17858, -17948,
    -18037, -18127, -18216, -18305, -18394, -18483, -18571, -18659, -18747, -18835, -18923, -19010, -19098, -19185,
    -19271, -19358, -19444, -19531, -19617, -19702, -19788, -19873, -19959, -20043, -20128, -20213, -20297, -20381,
    -20465, -20549, -20632, -20715, -20798, -20881, -20963, -21046, -21128, -21210, -21291, -21373, -21454, -21535,
    -21616, -21696, -21776, -21856, -21936, -22016, -22095, -22174, -22253, -22331, -22410, -22488, -22566, -22643,
    -22721, -22798, -22875, -22951, -23028, -23104, -23180, -23256, -23331, -23406, -23481, -23556, -23630, -23704,
    -23778, -23852, -23925, -23998, -24071, -24144, -24216, -24288, -24360, -24432, -24503, -24574, -24645, -24716,
    -24786, -24856, -24926, -24995, -25064, -25133, -25202, -25270, -25339, -25406, -25474, -25541, -25608, -25675,
    -25742, -25808, -25874, -25939, -26005, -26070, -26135, -26199, -26264, -26327, -26391, -26455, -26518, -26581,
    -26643, -26705, -26767, -26829, -26891, -26952, -27013, -27073, -27133, -27193, -27253, -27312, -27372, -27430,
    -27489, -27547, -27605, -27663, -27720, -27777, -27834, -27890, -27946, -28002, -28058, -28113, -28168, -28223,
    -28277, -28331, -28385, -28438, -28491, -28544, -28596, -28649, -28701, -28752, -28803, -28854, -28905, -28955,
    -29006, -29055, -29105, -29154, -29203, -29251, -29299, -29347, -29395, -29442, -29489, -29535, -29582, -29628,
    -29673, -29719, -29764, -29808, -29853, -29897, -29941, -29984, -30027, -30070, -30112, -30154, -30196, -30238,
    -30279, -30320, -30360, -30400, -30440, -30480, -30519, -30558, -30596, -30635, -30672, -30710, -30747, -30784,
    -30821, -30857, -30893, -30929, -30964, -30999, -31033, -31068, -31102, -31135, -31168, -31201, -31234, -31266,
    -31298, -31330, -31361, -31392, -31422, -31453, -31483, -31512, -31541, -31570, -31599, -31627, -31655, -31682,
    -31710, -31737, -31763, -31789, -31815, -31841, -31866, -31891, -31915, -31939, -31963, -31986, -32010, -32032,
    -32055, -32077, -32099, -32120, -32141, -32162, -32182, -32202, -32222, -32241, -32260, -32279, -32297, -32315,
    -32333, -32350, -32367, -32383, -32399, -32415, -32431, -32446, -32461, -32475, -32489, -32503, -32517, -32530,
    -32542, -32555, -32567, -32579, -32590, -32601, -32612, -32622, -32632, -32641, -32651, -32659, -32668, -32676,
    -32684, -32692, -32699, -32706, -32712, -32718, -32724, -32729, -32734, -32739, -32743, -32747, -32751, -32754,
    -32757, -32760, -32762, -32764, -32765, -32767, -32767, -32767, 32767,  32767,  32765,  32761,  32756,  32750,
    32742,  32732,  32722,  32710,  32696,  32681,  32665,  32647,  32628,  32608,  32586,  32562,  32538,  32512,
    32484,  32455,  32425,  32393,  32360,  32326,  32290,  32253,  32214,  32174,  32133,  32090,  32046,  32001,
    31954,  31906,  31856,  31805,  31753,  31700,  31645,  31588,  31530,  31471,  31411,  31349,  31286,  31222,
    31156,  31089,  31020,  30951,  30880,  30807,  30733,  30658,  30582,  30504,  30425,  30345,  30263,  30181,
    30096,  30011,  29924,  29836,  29747,  29656,  29564,  29471,  29377,  29281,  29184,  29086,  28987,  28886,
    28784,  28681,  28577,  28471,  28365,  28257,  28147,  28037,  27925,  27812,  27698,  27583,  27467,  27349,
    27231,  27111,  26990,  26868,  26744,  26620,  26494,  26367,  26239,  26110,  25980,  25849,  25717,  25583,
    25449,  25313,  25176,  25038,  24900,  24760,  24619,  24477,  24333,  24189,  24044,  23898,  23751,  23602,
    23453,  23303,  23152,  22999,  22846,  22692,  22537,  22380,  22223,  22065,  21906,  21746,  21585,  21423,
    21261,  21097,  20933,  20767,  20601,  20434,  20265,  20096,  19927,  19756,  19584,  19412,  19239,  19065,
    18890,  18714,  18538,  18361,  18183,  18004,  17824,  17644,  17463,  17281,  17098,  16915,  16731,  16546,
    16361,  16175,  15988,  15800,  15612,  15423,  15234,  15043,  14852,  14661,  14469,  14276,  14083,  13889,
    13694,  13499,  13303,  13107,  12910,  12713,  12515,  12317,  12118,  11918,  11718,  11517,  11316,  11115,
    10913,  10710,  10508,  10304,  10100,  9896,   9691,   9486,   9281,   9075,   8869,   8662,   8455,   8248,
    8040,   7832,   7623,   7415,   7206,   6996,   6787,   6577,   6366,   6156,   5945,   5734,   5523,   5311,
    5100,   4888,   4675,   4463,   4251,   4038,   3825,   3612,   3399,   3185,   2972,   2758,   2544,   2330,
    2116,   1902,   1688,   1474,   1260,   1045,   831,    617,    402,    188,    -27,    -241,   -456,   -670,
    -885,   -1099,  -1313,  -1528,  -1742,  -1956,  -2170,  -2384,  -2598,  -2811,  -3025,  -3239,  -3452,  -3665,
    -3878,  -4091,  -4304,  -4516,  -4728,  -4941,  -5153,  -5364,  -5576,  -5787,  -5998,  -6209,  -6419,  -6629,
    -6839,  -7049,  -7258,  -7467,  -7676,  -7884,  -8092,  -8300,  -8507,  -8714,  -8920,  -9127,  -9332,  -9538,
    -9743,  -9947,  -10151, -10355, -10558, -10761, -10963, -11165, -11367, -11568, -11768, -11968, -12167, -12366,
    -12565, -12762, -12960, -13156, -13352, -13548, -13743, -13937, -14131, -14324, -14517, -14709, -14900, -15091,
    -15281, -15470, -15659, -15847, -16035, -16221, -16407, -16593, -16777, -16961, -17144, -17326, -17508, -17689,
    -17869, -18049, -18227, -18405, -18582, -18758, -18934, -19108, -19282, -19455, -19627, -19799, -19969, -20139,
    -20308, -20475, -20642, -20809, -20974, -21138, -21301, -21464, -21626, -21786, -21946, -22105, -22263, -22420,
    -22575, -22730, -22884, -23037, -23189, -23340, -23490, -23640, -23788, -23935, -24080, -24225, -24369, -24512,
    -24654, -24795, -24934, -25073, -25211, -25347, -25482, -25617, -25750, -25882, -26013, -26143, -26272, -26399,
    -26526, -26651, -26775, -26898, -27020, -27141, -27260, -27379, -27496, -27612, -27727, -27841, -27953, -28065,
    -28175, -28284, -28391, -28498, -28603, -28707, -28810, -28911, -29012, -29111, -29209, -29305, -29401, -29495,
    -29587, -29679, -29769, -29858, -29946, -30032, -30118, -30201, -30284, -30365, -30445, -30524, -30601, -30677,
    -30752, -30825, -30897, -30968, -31038, -31106, -31172, -31238, -31302, -31365, -31426, -31486, -31545, -31602,
    -31658, -31713, -31766, -31818, -31869, -31918, -31966, -32012, -32058, -32101, -32144, -32185, -32224, -32262,
    -32299, -32335, -32369, -32401, -32433, -32463, -32491, -32518, -32544, -32568, -32591, -32613, -32633, -32652,
    -32669, -32685, -32700, -32713, -32724, -32735, -32744, -32751, -32757, -32762, -32766, -32767, 32767,  32764,
    32755,  32741,  32720,  32694,  32663,  32626,  32583,  32535,  32481,  32421,  32356,  32286,  32209,  32128,
    32041,  31948,  31850,  31747,  31638,  31523,  31403,  31278,  31148,  31012,  30871,  30724,  30572,  30415,
    30253,  30086,  29913,  29736,  29553,  29365,  29172,  28974,  28771,  28564,  28351,  28134,  27911,  27684,
    27452,  27216,  26975,  26729,  26478,  26223,  25964,  25700,  25432,  25159,  24882,  24601,  24315,  24026,
    23732,  23434,  23133,  22827,  22517,  22204,  21886,  21565,  21240,  20912,  20580,  20244,  19905,  19563,
    19217,  18868,  18516,  18160,  17802,  17440,  17075,  16708,  16338,  15964,  15588,  15210,  14829,  14445,
    14059,  13670,  13279,  12886,  12490,  12093,  11693,  11291,  10888,  10482,  10075,  9666,   9255,   8843,
    8429,   8014,   7597,   7180,   6760,   6340,   5919,   5496,   5073,   4649,   4224,   3798,   3372,   2945,
    2517,   2090,   1661,   1233,   804,    375,    -54,    -483,   -911,   -1340,  -1768,  -2197,  -2624,  -3052,
    -3479,  -3905,  -4330,  -4755,  -5179,  -5602,  -6024,  -6445,  -6865,  -7284,  -7702,  -8118,  -8533,  -8946,
    -9358,  -9768,  -10177, -10584, -10989, -11392, -11793, -12192, -12589, -12984, -13377, -13767, -14155, -14541,
    -14924, -15305, -15683, -16058, -16430, -16800, -17167, -17531, -17892, -18249, -18604, -18956, -19304, -19649,
    -19990, -20329, -20663, -20994, -21322, -21646, -21966, -22282, -22595, -22904, -23208, -23509, -23806, -24099,
    -24387, -24672, -24952, -25228, -25499, -25766, -26029, -26288, -26541, -26791, -27035, -27275, -27511, -27741,
    -27967, -28188, -28405, -28616, -28823, -29024, -29221, -29412, -29599, -29780, -29957, -30128, -30294, -30455,
    -30611, -30761, -30906, -31046, -31181, -31310, -31434, -31552, -31665, -31773, -31875, -31972, -32063, -32149,
    -32229, -32304, -32373, -32437, -32495, -32547, -32594, -32635, -32671, -32701, -32726, -32745, -32758, -32766,
    32767,  32754,  32717,  32658,  32577,  32473,  32348,  32200,  32029,  31837,  31624,  31388,  31131,  30853,
    30553,  30232,  29891,  29530,  29148,  28746,  28324,  27883,  27423,  26944,  26447,  25931,  25398,  24847,
    24279,  23695,  23095,  22478,  21846,  21199,  20538,  19863,  19174,  18472,  17757,  17030,  16291,  15541,
    14781,  14010,  13230,  12441,  11643,  10837,  10024,  9204,   8377,   7545,   6708,   5866,   5020,   4171,
    3319,   2464,   1608,   751,    -107,   -965,   -1822,  -2678,  -3532,  -4383,  -5232,  -6077,  -6918,  -7754,
    -8585,  -9409,  -10228, -11039, -11843, -12639, -13426, -14204, -14972, -15730, -16477, -17213, -17937, -18648,
    -19347, -20033, -20705, -21363, -22006, -22634, -23246, -23843, -24423, -24986, -25533, -26062, -26573, -27066,
    -27540, -27995, -28431, -28848, -29245, -29622, -29979, -30315, -30630, -30924, -31197, -31449, -31679, -31887,
    -32074, -32239, -32381, -32501, -32600, -32675, -32729, -32759,
};

static const int16_t window120[120] = {
    2,     20,    55,    108,   178,   266,   372,   494,   635,   792,   966,   1157,  1365,  1590,  1831,
    2089,  2362,  2651,  2956,  3276,  3611,  3961,  4325,  4703,  5094,  5499,  5916,  6346,  6788,  7241,
    7705,  8179,  8663,  9156,  9657,  10167, 10684, 11207, 11736, 12271, 12810, 13353, 13899, 14447, 14997,
    15547, 16098, 16648, 17197, 17744, 18287, 18827, 19363, 19893, 20418, 20936, 21447, 21950, 22445, 22931,
    23407, 23874, 24330, 24774, 25208, 25629, 26039, 26435, 26819, 27190, 27548, 27893, 28224, 28541, 28845,
    29135, 29411, 29674, 29924, 30160, 30384, 30594, 30792, 30977, 31151, 31313, 31463, 31602, 31731, 31849,
    31958, 32057, 32148, 32229, 32303, 32370, 32429, 32481, 32528, 32568, 32604, 32634, 32661, 32683, 32701,
    32717, 32729, 32740, 32748, 32754, 32758, 32762, 32764, 32766, 32767, 32767, 32767, 32767, 32767, 32767,
};

static const int16_t logN400[21] = {
    0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 16, 16, 16, 21, 21, 24, 29, 34, 36,
};

const int16_t cache_index50[105] = {
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  0,   0,   0,   0,   41,  41,  41,  82,  82,  123, 164, 200, 222,
    0,   0,   0,   0,   0,   0,   0,   0,   41,  41,  41,  41,  123, 123, 123, 164, 164, 240, 266, 283, 295,
    41,  41,  41,  41,  41,  41,  41,  41,  123, 123, 123, 123, 240, 240, 240, 266, 266, 305, 318, 328, 336,
    123, 123, 123, 123, 123, 123, 123, 123, 240, 240, 240, 240, 305, 305, 305, 318, 318, 343, 351, 358, 364,
    240, 240, 240, 240, 240, 240, 240, 240, 305, 305, 305, 305, 343, 343, 343, 351, 351, 370, 376, 382, 387,
};
const uint8_t cache_bits50[392] = {
    40,  7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
    7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   40,  15,  23,
    28,  31,  34,  36,  38,  39,  41,  42,  43,  44,  45,  46,  47,  47,  49,  50,  51,  52,  53,  54,  55,  55,
    57,  58,  59,  60,  61,  62,  63,  63,  65,  66,  67,  68,  69,  70,  71,  71,  40,  20,  33,  41,  48,  53,
    57,  61,  64,  66,  69,  71,  73,  75,  76,  78,  80,  82,  85,  87,  89,  91,  92,  94,  96,  98,  101, 103,
    105, 107, 108, 110, 112, 114, 117, 119, 121, 123, 124, 126, 128, 40,  23,  39,  51,  60,  67,  73,  79,  83,
    87,  91,  94,  97,  100, 102, 105, 107, 111, 115, 118, 121, 124, 126, 129, 131, 135, 139, 142, 145, 148, 150,
    153, 155, 159, 163, 166, 169, 172, 174, 177, 179, 35,  28,  49,  65,  78,  89,  99,  107, 114, 120, 126, 132,
    136, 141, 145, 149, 153, 159, 165, 171, 176, 180, 185, 189, 192, 199, 205, 211, 216, 220, 225, 229, 232, 239,
    245, 251, 21,  33,  58,  79,  97,  112, 125, 137, 148, 157, 166, 174, 182, 189, 195, 201, 207, 217, 227, 235,
    243, 251, 17,  35,  63,  86,  106, 123, 139, 152, 165, 177, 187, 197, 206, 214, 222, 230, 237, 250, 25,  31,
    55,  75,  91,  105, 117, 128, 138, 146, 154, 161, 168, 174, 180, 185, 190, 200, 208, 215, 222, 229, 235, 240,
    245, 255, 16,  36,  65,  89,  110, 128, 144, 159, 173, 185, 196, 207, 217, 226, 234, 242, 250, 11,  41,  74,
    103, 128, 151, 172, 191, 209, 225, 241, 255, 9,   43,  79,  110, 138, 163, 186, 207, 227, 246, 12,  39,  71,
    99,  123, 144, 164, 182, 198, 214, 228, 241, 253, 9,   44,  81,  113, 142, 168, 192, 214, 235, 255, 7,   49,
    90,  127, 160, 191, 220, 247, 6,   51,  95,  134, 170, 203, 234, 7,   47,  87,  123, 155, 184, 212, 237, 6,
    52,  97,  137, 174, 208, 240, 5,   57,  106, 151, 192, 231, 5,   59,  111, 158, 202, 243, 5,   55,  103, 147,
    187, 224, 5,   60,  113, 161, 206, 248, 4,   65,  122, 175, 224, 4,   67,  127, 182, 234,
};
static const uint8_t cache_caps50[168] = {
    224, 224, 224, 224, 224, 224, 224, 224, 160, 160, 160, 160, 185, 185, 185, 178, 178, 168, 134, 61, 37,
    224, 224, 224, 224, 224, 224, 224, 224, 240, 240, 240, 240, 207, 207, 207, 198, 198, 183, 144, 66, 40,
    160, 160, 160, 160, 160, 160, 160, 160, 185, 185, 185, 185, 193, 193, 193, 183, 183, 172, 138, 64, 38,
    240, 240, 240, 240, 240, 240, 240, 240, 207, 207, 207, 207, 204, 204, 204, 193, 193, 180, 143, 66, 40,
    185, 185, 185, 185, 185, 185, 185, 185, 193, 193, 193, 193, 193, 193, 193, 183, 183, 172, 138, 65, 39,
    207, 207, 207, 207, 207, 207, 207, 207, 204, 204, 204, 204, 201, 201, 201, 188, 188, 176, 141, 66, 40,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 194, 194, 194, 184, 184, 173, 139, 65, 39,
    204, 204, 204, 204, 204, 204, 204, 204, 201, 201, 201, 201, 198, 198, 198, 187, 187, 175, 140, 66, 40,
};

static const kiss_twiddle_cpx fft_twiddles48000_960[480] = {
    {32767, 0},       {32766, -429},    {32757, -858},    {32743, -1287},   {32724, -1715},   {32698, -2143},
    {32667, -2570},   {32631, -2998},   {32588, -3425},   {32541, -3851},   {32488, -4277},   {32429, -4701},
    {32364, -5125},   {32295, -5548},   {32219, -5971},   {32138, -6393},   {32051, -6813},   {31960, -7231},
    {31863, -7650},   {31760, -8067},   {31652, -8481},   {31539, -8895},   {31419, -9306},   {31294, -9716},
    {31165, -10126},  {31030, -10532},  {30889, -10937},  {30743, -11340},  {30592, -11741},  {30436, -12141},
    {30274, -12540},  {30107, -12935},  {29936, -13328},  {29758, -13718},  {29577, -14107},  {29390, -14493},
    {29197, -14875},  {29000, -15257},  {28797, -15635},  {28590, -16010},  {28379, -16384},  {28162, -16753},
    {27940, -17119},  {27714, -17484},  {27482, -17845},  {27246, -18205},  {27006, -18560},  {26760, -18911},
    {26510, -19260},  {26257, -19606},  {25997, -19947},  {25734, -20286},  {25466, -20621},  {25194, -20952},
    {24918, -21281},  {24637, -21605},  {24353, -21926},  {24063, -22242},  {23770, -22555},  {23473, -22865},
    {23171, -23171},  {22866, -23472},  {22557, -23769},  {22244, -24063},  {21927, -24352},  {21606, -24636},
    {21282, -24917},  {20954, -25194},  {20622, -25465},  {20288, -25733},  {19949, -25997},  {19607, -26255},
    {19261, -26509},  {18914, -26760},  {18561, -27004},  {18205, -27246},  {17846, -27481},  {17485, -27713},
    {17122, -27940},  {16755, -28162},  {16385, -28378},  {16012, -28590},  {15636, -28797},  {15258, -28999},
    {14878, -29197},  {14494, -29389},  {14108, -29576},  {13720, -29757},  {13329, -29934},  {12937, -30107},
    {12540, -30274},  {12142, -30435},  {11744, -30592},  {11342, -30743},  {10939, -30889},  {10534, -31030},
    {10127, -31164},  {9718, -31294},   {9307, -31418},   {8895, -31537},   {8482, -31652},   {8067, -31759},
    {7650, -31862},   {7233, -31960},   {6815, -32051},   {6393, -32138},   {5973, -32219},   {5549, -32294},
    {5127, -32364},   {4703, -32429},   {4278, -32487},   {3852, -32541},   {3426, -32588},   {2999, -32630},
    {2572, -32667},   {2144, -32698},   {1716, -32724},   {1287, -32742},   {860, -32757},    {430, -32766},
    {0, -32767},      {-429, -32766},   {-858, -32757},   {-1287, -32743},  {-1715, -32724},  {-2143, -32698},
    {-2570, -32667},  {-2998, -32631},  {-3425, -32588},  {-3851, -32541},  {-4277, -32488},  {-4701, -32429},
    {-5125, -32364},  {-5548, -32295},  {-5971, -32219},  {-6393, -32138},  {-6813, -32051},  {-7231, -31960},
    {-7650, -31863},  {-8067, -31760},  {-8481, -31652},  {-8895, -31539},  {-9306, -31419},  {-9716, -31294},
    {-10126, -31165}, {-10532, -31030}, {-10937, -30889}, {-11340, -30743}, {-11741, -30592}, {-12141, -30436},
    {-12540, -30274}, {-12935, -30107}, {-13328, -29936}, {-13718, -29758}, {-14107, -29577}, {-14493, -29390},
    {-14875, -29197}, {-15257, -29000}, {-15635, -28797}, {-16010, -28590}, {-16384, -28379}, {-16753, -28162},
    {-17119, -27940}, {-17484, -27714}, {-17845, -27482}, {-18205, -27246}, {-18560, -27006}, {-18911, -26760},
    {-19260, -26510}, {-19606, -26257}, {-19947, -25997}, {-20286, -25734}, {-20621, -25466}, {-20952, -25194},
    {-21281, -24918}, {-21605, -24637}, {-21926, -24353}, {-22242, -24063}, {-22555, -23770}, {-22865, -23473},
    {-23171, -23171}, {-23472, -22866}, {-23769, -22557}, {-24063, -22244}, {-24352, -21927}, {-24636, -21606},
    {-24917, -21282}, {-25194, -20954}, {-25465, -20622}, {-25733, -20288}, {-25997, -19949}, {-26255, -19607},
    {-26509, -19261}, {-26760, -18914}, {-27004, -18561}, {-27246, -18205}, {-27481, -17846}, {-27713, -17485},
    {-27940, -17122}, {-28162, -16755}, {-28378, -16385}, {-28590, -16012}, {-28797, -15636}, {-28999, -15258},
    {-29197, -14878}, {-29389, -14494}, {-29576, -14108}, {-29757, -13720}, {-29934, -13329}, {-30107, -12937},
    {-30274, -12540}, {-30435, -12142}, {-30592, -11744}, {-30743, -11342}, {-30889, -10939}, {-31030, -10534},
    {-31164, -10127}, {-31294, -9718},  {-31418, -9307},  {-31537, -8895},  {-31652, -8482},  {-31759, -8067},
    {-31862, -7650},  {-31960, -7233},  {-32051, -6815},  {-32138, -6393},  {-32219, -5973},  {-32294, -5549},
    {-32364, -5127},  {-32429, -4703},  {-32487, -4278},  {-32541, -3852},  {-32588, -3426},  {-32630, -2999},
    {-32667, -2572},  {-32698, -2144},  {-32724, -1716},  {-32742, -1287},  {-32757, -860},   {-32766, -430},
    {-32767, 0},      {-32766, 429},    {-32757, 858},    {-32743, 1287},   {-32724, 1715},   {-32698, 2143},
    {-32667, 2570},   {-32631, 2998},   {-32588, 3425},   {-32541, 3851},   {-32488, 4277},   {-32429, 4701},
    {-32364, 5125},   {-32295, 5548},   {-32219, 5971},   {-32138, 6393},   {-32051, 6813},   {-31960, 7231},
    {-31863, 7650},   {-31760, 8067},   {-31652, 8481},   {-31539, 8895},   {-31419, 9306},   {-31294, 9716},
    {-31165, 10126},  {-31030, 10532},  {-30889, 10937},  {-30743, 11340},  {-30592, 11741},  {-30436, 12141},
    {-30274, 12540},  {-30107, 12935},  {-29936, 13328},  {-29758, 13718},  {-29577, 14107},  {-29390, 14493},
    {-29197, 14875},  {-29000, 15257},  {-28797, 15635},  {-28590, 16010},  {-28379, 16384},  {-28162, 16753},
    {-27940, 17119},  {-27714, 17484},  {-27482, 17845},  {-27246, 18205},  {-27006, 18560},  {-26760, 18911},
    {-26510, 19260},  {-26257, 19606},  {-25997, 19947},  {-25734, 20286},  {-25466, 20621},  {-25194, 20952},
    {-24918, 21281},  {-24637, 21605},  {-24353, 21926},  {-24063, 22242},  {-23770, 22555},  {-23473, 22865},
    {-23171, 23171},  {-22866, 23472},  {-22557, 23769},  {-22244, 24063},  {-21927, 24352},  {-21606, 24636},
    {-21282, 24917},  {-20954, 25194},  {-20622, 25465},  {-20288, 25733},  {-19949, 25997},  {-19607, 26255},
    {-19261, 26509},  {-18914, 26760},  {-18561, 27004},  {-18205, 27246},  {-17846, 27481},  {-17485, 27713},
    {-17122, 27940},  {-16755, 28162},  {-16385, 28378},  {-16012, 28590},  {-15636, 28797},  {-15258, 28999},
    {-14878, 29197},  {-14494, 29389},  {-14108, 29576},  {-13720, 29757},  {-13329, 29934},  {-12937, 30107},
    {-12540, 30274},  {-12142, 30435},  {-11744, 30592},  {-11342, 30743},  {-10939, 30889},  {-10534, 31030},
    {-10127, 31164},  {-9718, 31294},   {-9307, 31418},   {-8895, 31537},   {-8482, 31652},   {-8067, 31759},
    {-7650, 31862},   {-7233, 31960},   {-6815, 32051},   {-6393, 32138},   {-5973, 32219},   {-5549, 32294},
    {-5127, 32364},   {-4703, 32429},   {-4278, 32487},   {-3852, 32541},   {-3426, 32588},   {-2999, 32630},
    {-2572, 32667},   {-2144, 32698},   {-1716, 32724},   {-1287, 32742},   {-860, 32757},    {-430, 32766},
    {0, 32767},       {429, 32766},     {858, 32757},     {1287, 32743},    {1715, 32724},    {2143, 32698},
    {2570, 32667},    {2998, 32631},    {3425, 32588},    {3851, 32541},    {4277, 32488},    {4701, 32429},
    {5125, 32364},    {5548, 32295},    {5971, 32219},    {6393, 32138},    {6813, 32051},    {7231, 31960},
    {7650, 31863},    {8067, 31760},    {8481, 31652},    {8895, 31539},    {9306, 31419},    {9716, 31294},
    {10126, 31165},   {10532, 31030},   {10937, 30889},   {11340, 30743},   {11741, 30592},   {12141, 30436},
    {12540, 30274},   {12935, 30107},   {13328, 29936},   {13718, 29758},   {14107, 29577},   {14493, 29390},
    {14875, 29197},   {15257, 29000},   {15635, 28797},   {16010, 28590},   {16384, 28379},   {16753, 28162},
    {17119, 27940},   {17484, 27714},   {17845, 27482},   {18205, 27246},   {18560, 27006},   {18911, 26760},
    {19260, 26510},   {19606, 26257},   {19947, 25997},   {20286, 25734},   {20621, 25466},   {20952, 25194},
    {21281, 24918},   {21605, 24637},   {21926, 24353},   {22242, 24063},   {22555, 23770},   {22865, 23473},
    {23171, 23171},   {23472, 22866},   {23769, 22557},   {24063, 22244},   {24352, 21927},   {24636, 21606},
    {24917, 21282},   {25194, 20954},   {25465, 20622},   {25733, 20288},   {25997, 19949},   {26255, 19607},
    {26509, 19261},   {26760, 18914},   {27004, 18561},   {27246, 18205},   {27481, 17846},   {27713, 17485},
    {27940, 17122},   {28162, 16755},   {28378, 16385},   {28590, 16012},   {28797, 15636},   {28999, 15258},
    {29197, 14878},   {29389, 14494},   {29576, 14108},   {29757, 13720},   {29934, 13329},   {30107, 12937},
    {30274, 12540},   {30435, 12142},   {30592, 11744},   {30743, 11342},   {30889, 10939},   {31030, 10534},
    {31164, 10127},   {31294, 9718},    {31418, 9307},    {31537, 8895},    {31652, 8482},    {31759, 8067},
    {31862, 7650},    {31960, 7233},    {32051, 6815},    {32138, 6393},    {32219, 5973},    {32294, 5549},
    {32364, 5127},    {32429, 4703},    {32487, 4278},    {32541, 3852},    {32588, 3426},    {32630, 2999},
    {32667, 2572},    {32698, 2144},    {32724, 1716},    {32742, 1287},    {32757, 860},     {32766, 430},
};

static const int16_t fft_bitrev480[480] = {
    0,   96,  192, 288, 384, 32,  128, 224, 320, 416, 64,  160, 256, 352, 448, 8,   104, 200, 296, 392, 40,  136, 232,
    328, 424, 72,  168, 264, 360, 456, 16,  112, 208, 304, 400, 48,  144, 240, 336, 432, 80,  176, 272, 368, 464, 24,
    120, 216, 312, 408, 56,  152, 248, 344, 440, 88,  184, 280, 376, 472, 4,   100, 196, 292, 388, 36,  132, 228, 324,
    420, 68,  164, 260, 356, 452, 12,  108, 204, 300, 396, 44,  140, 236, 332, 428, 76,  172, 268, 364, 460, 20,  116,
    212, 308, 404, 52,  148, 244, 340, 436, 84,  180, 276, 372, 468, 28,  124, 220, 316, 412, 60,  156, 252, 348, 444,
    92,  188, 284, 380, 476, 1,   97,  193, 289, 385, 33,  129, 225, 321, 417, 65,  161, 257, 353, 449, 9,   105, 201,
    297, 393, 41,  137, 233, 329, 425, 73,  169, 265, 361, 457, 17,  113, 209, 305, 401, 49,  145, 241, 337, 433, 81,
    177, 273, 369, 465, 25,  121, 217, 313, 409, 57,  153, 249, 345, 441, 89,  185, 281, 377, 473, 5,   101, 197, 293,
    389, 37,  133, 229, 325, 421, 69,  165, 261, 357, 453, 13,  109, 205, 301, 397, 45,  141, 237, 333, 429, 77,  173,
    269, 365, 461, 21,  117, 213, 309, 405, 53,  149, 245, 341, 437, 85,  181, 277, 373, 469, 29,  125, 221, 317, 413,
    61,  157, 253, 349, 445, 93,  189, 285, 381, 477, 2,   98,  194, 290, 386, 34,  130, 226, 322, 418, 66,  162, 258,
    354, 450, 10,  106, 202, 298, 394, 42,  138, 234, 330, 426, 74,  170, 266, 362, 458, 18,  114, 210, 306, 402, 50,
    146, 242, 338, 434, 82,  178, 274, 370, 466, 26,  122, 218, 314, 410, 58,  154, 250, 346, 442, 90,  186, 282, 378,
    474, 6,   102, 198, 294, 390, 38,  134, 230, 326, 422, 70,  166, 262, 358, 454, 14,  110, 206, 302, 398, 46,  142,
    238, 334, 430, 78,  174, 270, 366, 462, 22,  118, 214, 310, 406, 54,  150, 246, 342, 438, 86,  182, 278, 374, 470,
    30,  126, 222, 318, 414, 62,  158, 254, 350, 446, 94,  190, 286, 382, 478, 3,   99,  195, 291, 387, 35,  131, 227,
    323, 419, 67,  163, 259, 355, 451, 11,  107, 203, 299, 395, 43,  139, 235, 331, 427, 75,  171, 267, 363, 459, 19,
    115, 211, 307, 403, 51,  147, 243, 339, 435, 83,  179, 275, 371, 467, 27,  123, 219, 315, 411, 59,  155, 251, 347,
    443, 91,  187, 283, 379, 475, 7,   103, 199, 295, 391, 39,  135, 231, 327, 423, 71,  167, 263, 359, 455, 15,  111,
    207, 303, 399, 47,  143, 239, 335, 431, 79,  175, 271, 367, 463, 23,  119, 215, 311, 407, 55,  151, 247, 343, 439,
    87,  183, 279, 375, 471, 31,  127, 223, 319, 415, 63,  159, 255, 351, 447, 95,  191, 287, 383, 479,
};

static const int16_t fft_bitrev240[240] = {
    0,  48, 96,  144, 192, 16, 64, 112, 160, 208, 32, 80, 128, 176, 224, 4,  52, 100, 148, 196, 20, 68, 116, 164, 212,
    36, 84, 132, 180, 228, 8,  56, 104, 152, 200, 24, 72, 120, 168, 216, 40, 88, 136, 184, 232, 12, 60, 108, 156, 204,
    28, 76, 124, 172, 220, 44, 92, 140, 188, 236, 1,  49, 97,  145, 193, 17, 65, 113, 161, 209, 33, 81, 129, 177, 225,
    5,  53, 101, 149, 197, 21, 69, 117, 165, 213, 37, 85, 133, 181, 229, 9,  57, 105, 153, 201, 25, 73, 121, 169, 217,
    41, 89, 137, 185, 233, 13, 61, 109, 157, 205, 29, 77, 125, 173, 221, 45, 93, 141, 189, 237, 2,  50, 98,  146, 194,
    18, 66, 114, 162, 210, 34, 82, 130, 178, 226, 6,  54, 102, 150, 198, 22, 70, 118, 166, 214, 38, 86, 134, 182, 230,
    10, 58, 106, 154, 202, 26, 74, 122, 170, 218, 42, 90, 138, 186, 234, 14, 62, 110, 158, 206, 30, 78, 126, 174, 222,
    46, 94, 142, 190, 238, 3,  51, 99,  147, 195, 19, 67, 115, 163, 211, 35, 83, 131, 179, 227, 7,  55, 103, 151, 199,
    23, 71, 119, 167, 215, 39, 87, 135, 183, 231, 11, 59, 107, 155, 203, 27, 75, 123, 171, 219, 43, 91, 139, 187, 235,
    15, 63, 111, 159, 207, 31, 79, 127, 175, 223, 47, 95, 143, 191, 239,
};

static const int16_t fft_bitrev120[120] = {
    0,   24,  48,  72,  96, 8,   32,  56,  80,  104, 16, 40,  64,  88,  112, 4,   28, 52,  76,  100, 12,  36,  60, 84,
    108, 20,  44,  68,  92, 116, 1,   25,  49,  73,  97, 9,   33,  57,  81,  105, 17, 41,  65,  89,  113, 5,   29, 53,
    77,  101, 13,  37,  61, 85,  109, 21,  45,  69,  93, 117, 2,   26,  50,  74,  98, 10,  34,  58,  82,  106, 18, 42,
    66,  90,  114, 6,   30, 54,  78,  102, 14,  38,  62, 86,  110, 22,  46,  70,  94, 118, 3,   27,  51,  75,  99, 11,
    35,  59,  83,  107, 19, 43,  67,  91,  115, 7,   31, 55,  79,  103, 15,  39,  63, 87,  111, 23,  47,  71,  95, 119,
};

static const int16_t fft_bitrev60[60] = {
    0, 12, 24, 36, 48, 4, 16, 28, 40, 52, 8,  20, 32, 44, 56, 1, 13, 25, 37, 49, 5, 17, 29, 41, 53, 9,  21, 33, 45, 57,
    2, 14, 26, 38, 50, 6, 18, 30, 42, 54, 10, 22, 34, 46, 58, 3, 15, 27, 39, 51, 7, 19, 31, 43, 55, 11, 23, 35, 47, 59,
};

const uint8_t LOG2_FRAC_TABLE[24] = {0,  8,  13, 16, 19, 21, 23, 24, 26, 27, 28, 29,
                                     30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 37};

/* Mean energy in each band quantized in Q4 */
const signed char eMeans[25] = {103, 100, 92, 85, 81, 77, 72, 70, 78, 75, 73, 71, 78,
                                74,  69,  72, 70, 74, 76, 71, 60, 60, 60, 60, 60};

/* prediction coefficients: 0.9, 0.8, 0.65, 0.5 */
static const int16_t pred_coef[4] = {29440, 26112, 21248, 16384};
static const int16_t beta_coef[4] = {30147, 22282, 12124, 6554};
static const int16_t beta_intra = 4915;


/*Parameters of the Laplace-like probability models used for the coarse energy. There is one pair of parameters for
  each frame size, prediction type (inter/intra), and band number. The first number of each pair is the probability
  of 0, and the second is the decay rate, both in Q8 precision.*/
static const uint8_t e_prob_model[4][2][42] = {
    /*120 sample frames.*/
    {/*Inter*/
     {72, 127, 65, 129, 66, 128, 65, 128, 64, 128, 62, 128, 64, 128, 64, 128, 92, 78,  92, 79,  92,
      78, 90,  79, 116, 41, 115, 40, 114, 40, 132, 26, 132, 26, 145, 17, 161, 12, 176, 10, 177, 11},
     /*Intra*/
     {24, 179, 48, 138, 54, 135, 54, 132, 53, 134, 56, 133, 55, 132, 55, 132, 61, 114, 70, 96, 74,
      88, 75,  88, 87,  74, 89,  66, 91,  67, 100, 59, 108, 50, 120, 40, 122, 37, 97,  43, 78, 50}},
    /*240 sample frames.*/
    {/*Inter*/
     {83, 78,  84, 81,  88, 75,  86, 74,  87, 71,  90, 73,  93, 74,  93, 74,  109, 40,  114, 36,  117,
      34, 117, 34, 143, 17, 145, 18, 146, 19, 162, 12, 165, 10, 178, 7,  189, 6,   190, 8,   177, 9},
     /*Intra*/
     {23, 178, 54, 115, 63, 102, 66, 98,  69, 99,  74, 89,  71, 91,  73, 91,  78, 89, 86, 80, 92,
      66, 93,  64, 102, 59, 103, 60, 104, 60, 117, 52, 123, 44, 138, 35, 133, 31, 97, 38, 77, 45}},
    /*480 sample frames.*/
    {/*Inter*/
     {61, 90,  93, 60,  105, 42,  107, 41,  110, 45,  116, 38,  113, 38,  112, 38,  124, 26,  132, 27,  136,
      19, 140, 20, 155, 14,  159, 16,  158, 18,  170, 13,  177, 10,  187, 8,   192, 6,   175, 9,   159, 10},
     /*Intra*/
     {21, 178, 59, 110, 71, 86,  75, 85,  84, 83,  91, 66,  88, 73,  87, 72,  92, 75, 98, 72, 105,
      58, 107, 54, 115, 52, 114, 55, 112, 56, 129, 51, 132, 40, 150, 33, 140, 29, 98, 35, 77, 42}},
    /*960 sample frames.*/
    {/*Inter*/
     {42, 121, 96, 66,  108, 43,  111, 40,  117, 44,  123, 32,  120, 36,  119, 33,  127, 33,  134, 34,  139,
      21, 147, 23, 152, 20,  158, 25,  154, 26,  166, 21,  173, 16,  184, 13,  184, 10,  150, 13,  139, 15},
     /*Intra*/
     {22, 178, 63, 114, 74, 82,  84, 83,  92, 82,  103, 62,  96, 72,  96, 67,  101, 73, 107, 72, 113,
      55, 118, 52, 125, 52, 118, 52, 117, 55, 135, 49,  137, 39, 157, 32, 145, 29,  97, 33,  77, 40}}};

static const uint8_t small_energy_icdf[3]={2,1,0};

static const uint8_t trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
/* Probs: NONE: 21.875%, LIGHT: 6.25%, NORMAL: 65.625%, AGGRESSIVE: 6.25% */
static const uint8_t spread_icdf[4] = {25, 23, 2, 0};

static const uint8_t tapset_icdf[3]={2,1,0};

static const kiss_fft_state fft_state48000_960_0 = {
    480,   /* nfft */
    17476, /* scale */
    8,     /* scale_shift */
    -1,    /* shift */
    {5, 96, 3, 32, 4, 8, 2, 4, 4, 1, 0,  0, 0, 0, 0, 0,}, /* factors */
    fft_bitrev480,         /* bitrev */
    fft_twiddles48000_960, /* bitrev */
};

static const kiss_fft_state fft_state48000_960_1 = {
    240,   /* nfft */
    17476, /* scale */
    7,     /* scale_shift */
    1,     /* shift */
    {5, 48, 3, 16, 4, 4, 4, 1, 0, 0, 0, 0, 0, 0,  0, 0,}, /* factors */
    fft_bitrev240,         /* bitrev */
    fft_twiddles48000_960, /* bitrev */
};

static const kiss_fft_state fft_state48000_960_2 = {
    120,   /* nfft */
    17476, /* scale */
    6,     /* scale_shift */
    2,     /* shift */
    {5, 24, 3, 8, 2, 4, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0,}, /* factors */
    fft_bitrev120,         /* bitrev */
    fft_twiddles48000_960, /* bitrev */
};

static const kiss_fft_state fft_state48000_960_3 = {
    60,    /* nfft */
    17476, /* scale */
    5,     /* scale_shift */
    3,     /* shift */
    {5, 12, 3, 4, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,}, /* factors */
    fft_bitrev60,          /* bitrev */
    fft_twiddles48000_960, /* bitrev */
};

const CELTMode m_CELTMode = {
    48000,                  /* Fs */
    120,                    /* overlap */
    21,                     /* nbEBands */
    21,                     /* effEBands */
    {27853, 0, 4096, 8192,},/* preemph */
    3,                      /* maxLM */
    8,                      /* nbShortMdcts */
    120,                    /* shortMdctSize */
    11,                     /* nbAllocVectors */
};

const mdct_lookup_t m_mdct_lookup = {
    1920,3, {
         &fft_state48000_960_0,
         &fft_state48000_960_1,
         &fft_state48000_960_2,
         &fft_state48000_960_3,
     },
     mdct_twiddles960,                               /* mdct */
};

const uint32_t row_idx[15] = {0, 176, 351, 525, 698, 870, 1041, 1131, 1178, 1207, 1226, 1240, 1248, 1254, 1257};

uint32_t celt_pvq_u_row(uint32_t row, uint32_t data){
    uint32_t  ret = CELT_PVQ_U_DATA[row_idx[row] + data];
    return ret;
}

#define DECODE_BUFFER_SIZE 2048
#define CELT_PVQ_U(_n, _k) (celt_pvq_u_row(min(_n, _k), max(_n, _k)))
#define CELT_PVQ_V(_n, _k) (CELT_PVQ_U(_n, _k) + CELT_PVQ_U(_n, (_k) + 1))

//----------------------------------------------------------------------------------------------------------------------

void exp_rotation1(int16_t *X, int32_t len, int32_t stride, int16_t c, int16_t s) {
    int32_t i;
    int16_t ms;
    int16_t *Xptr;
    Xptr = X;
    ms = s * (-1);
    for (i = 0; i < len - stride; i++) {
        int16_t x1, x2;
        x1 = Xptr[0];
        x2 = Xptr[stride];
        Xptr[stride] = (int16_t)(PSHR(MAC16_16(MULT16_16(c, x2), s, x1), 15));
        *Xptr++ = (int16_t)(PSHR(MAC16_16(MULT16_16(c, x1), ms, x2), 15));
    }
    Xptr = &X[len - 2 * stride - 1];
    for (i = len - 2 * stride - 1; i >= 0; i--) {
        int16_t x1, x2;
        x1 = Xptr[0];
        x2 = Xptr[stride];
        Xptr[stride] = (int16_t)(PSHR(MAC16_16(MULT16_16(c, x2), s, x1), 15));
        *Xptr-- = (int16_t)(PSHR(MAC16_16(MULT16_16(c, x1), ms, x2), 15));
    }
}
//----------------------------------------------------------------------------------------------------------------------

void exp_rotation(int16_t *X, int32_t len, int32_t dir, int32_t stride, int32_t K, int32_t spread) {
    static const int32_t SPREAD_FACTOR[3] = {15, 10, 5};
    int32_t i;
    int16_t c, s;
    int16_t gain, theta;
    int32_t stride2 = 0;
    int32_t factor;

    if (2 * K >= len || spread == 0) return; // SPREAD_NONE
    factor = SPREAD_FACTOR[spread - 1];

    gain = celt_div((int32_t)MULT16_16(Q15_ONE, len), (int32_t)(len + factor * K));
    theta = HALF16(MULT16_16_Q15(gain, gain));

    c = celt_cos_norm(EXTEND32(theta));
    s = celt_cos_norm(EXTEND32(SUB16(32767, theta))); /*  sin(theta) */

    if (len >= 8 * stride) {
        stride2 = 1;
        /* This is just a simple (equivalent) way of computing sqrt(len/stride) with rounding.
           It's basically incrementing long as (stride2+0.5)^2 < len/stride. */
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) stride2++;
    }
    /*NOTE: As a minor optimization, we could be passing around log2(B), not B, for both this and for
       extract_collapse_mask().*/
    assert(stride > 0);
    len = len / stride;
    for (i = 0; i < stride; i++) {
        if (dir < 0) {
            if (stride2) exp_rotation1(X + i * len, len, stride2, s, c);
            exp_rotation1(X + i * len, len, 1, c, s);
        } else {
            exp_rotation1(X + i * len, len, 1, c, -s);
            if (stride2) exp_rotation1(X + i * len, len, stride2, s, -c);
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------

/** Takes the pitch vector and the decoded residual vector, computes the gain
    that will give ||p+g*y||=1 and mixes the residual with the pitch. */
void normalise_residual(int32_t * iy, int16_t * X, int32_t N, int32_t Ryy, int16_t gain) {
    int32_t i;
    int32_t k;
    int32_t t;
    int16_t g;
    if(Ryy < 1) log_e("celt_ilog2 %i", Ryy);
    k = celt_ilog2(Ryy) >> 1;
    t = VSHR32(Ryy, 2 * (k - 7));
    g = MULT16_16_P15(celt_rsqrt_norm(t), gain);

    i = 0;
    do X[i] = (int16_t)(PSHR(MULT16_16(g, iy[i]), k + 1));
    while (++i < N);
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t extract_collapse_mask(int32_t *iy, int32_t N, int32_t B) {
    uint32_t collapse_mask;
    int32_t N0;
    int32_t i;
    if (B <= 1) return 1;
    /*NOTE: As a minor optimization, we could be passing around log2(B), not B, for both this and for exp_rotation().*/
       assert(B > 0);
    N0 = N / B;
    collapse_mask = 0;
    i = 0;
    do {
        int32_t j;
        uint32_t tmp = 0;
        j = 0;
        do {
            tmp |= iy[i * N0 + j];
        } while (++j < N0);
        collapse_mask |= (tmp != 0) << i;
    } while (++i < B);
    return collapse_mask;
}
//----------------------------------------------------------------------------------------------------------------------

/** Decode pulse vector and combine the result with the pitch vector to produce
    the final normalised signal in the current band. */
uint32_t alg_unquant(int16_t *X, int32_t N, int32_t K, int32_t spread, int32_t B, int16_t gain) {
    int32_t Ryy;
    uint32_t collapse_mask;
    if(K <= 0) log_e("alg_unquant() needs at least one pulse");
    if(N <= 1) log_e("alg_unquant() needs at least two dimensions");

    int32_t* iy = s_iyBuff; assert(N <= 96);
    Ryy = decode_pulses(iy, N, K);
    normalise_residual(iy, X, N, Ryy, gain);
    exp_rotation(X, N, -1, B, K, spread);
    collapse_mask = extract_collapse_mask(iy, N, B);
    return collapse_mask;
}
//----------------------------------------------------------------------------------------------------------------------

void renormalise_vector(int16_t *X, int32_t N, int16_t gain) {
    int32_t i;
    int32_t k;
    uint32_t E;
    int16_t g;
    int32_t t;
    int16_t *xptr;
    E = EPSILON + celt_inner_prod(X, X, N);
    if(E < 1) log_e("celt_ilog2 %i, X=%i, N=%i", E, X[N], N );  // assert E > 0
    k = celt_ilog2(E) >> 1;
    t = VSHR32(E, 2 * (k - 7));
    g = MULT16_16_P15(celt_rsqrt_norm(t), gain);

    xptr = X;
    for (i = 0; i < N; i++) {
        *xptr = (int16_t)(PSHR(MULT16_16(g, *xptr), k + 1));
        xptr++;
    }
    /*return celt_sqrt(E);*/
}
//----------------------------------------------------------------------------------------------------------------------

void comb_filter_const(int32_t *y, int32_t *x, int32_t T, int32_t N, int16_t g10, int16_t g11, int16_t g12) {
    int32_t x0, x1, x2, x3, x4;
    int32_t i;
    x4 = x[-T - 2];
    x3 = x[-T - 1];
    x2 = x[-T];
    x1 = x[-T + 1];
    for (i = 0; i < N; i++) {
        x0 = x[i - T + 2];
        y[i]  = x[i];
        y[i] += MULT16_32_Q15(g10, x2);
        y[i] += MULT16_32_Q15(g11, ADD32(x1, x3));
        y[i] += MULT16_32_Q15(g12, ADD32(x0, x4));
        y[i] = SATURATE(y[i], (300000000));
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
}
//----------------------------------------------------------------------------------------------------------------------

void comb_filter(int32_t *y, int32_t *x, int32_t T0, int32_t T1, int32_t N, int16_t g0, int16_t g1, int32_t tapset0,
                 int32_t tapset1) {
    int32_t i;
    const uint8_t COMBFILTER_MINPERIOD = 15;
    uint8_t  overlap = m_CELTMode.overlap; // =120
    /* printf ("%d %d %f %f\n", T0, T1, g0, g1); */
    int16_t              g00, g01, g02, g10, g11, g12;
    int32_t              x0, x1, x2, x3, x4;
    static const int16_t gains[3][3] = {
        {QCONST16(0.3066406250f, 15), QCONST16(0.2170410156f, 15), QCONST16(0.1296386719f, 15)},
        {QCONST16(0.4638671875f, 15), QCONST16(0.2680664062f, 15), QCONST16(0.f, 15)},
        {QCONST16(0.7998046875f, 15), QCONST16(0.1000976562f, 15), QCONST16(0.f, 15)}};

    if(g0 == 0 && g1 == 0) {
        if(x != y) OPUS_MOVE(y, x, N);
        return;
    }
    /* When the gain is zero, T0 and/or T1 is set to zero. We need
       to have then be at least 2 to avoid processing garbage data. */
    T0 = max(T0, COMBFILTER_MINPERIOD);
    T1 = max(T1, COMBFILTER_MINPERIOD);
    g00 = MULT16_16_P15(g0, gains[tapset0][0]);
    g01 = MULT16_16_P15(g0, gains[tapset0][1]);
    g02 = MULT16_16_P15(g0, gains[tapset0][2]);
    g10 = MULT16_16_P15(g1, gains[tapset1][0]);
    g11 = MULT16_16_P15(g1, gains[tapset1][1]);
    g12 = MULT16_16_P15(g1, gains[tapset1][2]);
    x1 = x[-T1 + 1];
    x2 = x[-T1];
    x3 = x[-T1 - 1];
    x4 = x[-T1 - 2];
    /* If the filter didn't change, we don't need the overlap */
    if(g0 == g1 && T0 == T1 && tapset0 == tapset1) overlap = 0;
    for(i = 0; i < overlap; i++) {
        int16_t f;
        x0 = x[i - T1 + 2];
        f = MULT16_16_Q15(window120[i], window120[i]);
        y[i] = x[i];
        y[i] += MULT16_32_Q15(MULT16_16_Q15((32767 - f), g00), x[i - T0]);
        y[i] += MULT16_32_Q15(MULT16_16_Q15((32767 - f), g01), ADD32(x[i - T0 + 1], x[i - T0 - 1]));
        y[i] += MULT16_32_Q15(MULT16_16_Q15((32767 - f), g02), ADD32(x[i - T0 + 2], x[i - T0 - 2]));
        y[i] += MULT16_32_Q15(MULT16_16_Q15(f, g10), x2);
        y[i] += MULT16_32_Q15(MULT16_16_Q15(f, g11), ADD32(x1, x3));
        y[i] += MULT16_32_Q15(MULT16_16_Q15(f, g12), ADD32(x0, x4));
        y[i] = SATURATE(y[i], (300000000));
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
    if(g1 == 0) {
        if(x != y) OPUS_MOVE(y + overlap, x + overlap, N - overlap);
        return;
    }

    /* Compute the part with the constant filter. */
    comb_filter_const(y + i, x + i, T1, N - i, g10, g11, g12);
}
//----------------------------------------------------------------------------------------------------------------------

/* TF change table. Positive values mean better frequency resolution (longer effective window), whereas negative values
   mean better time resolution (shorter effective window). The second index is computed as:
   4*isTransient + 2*tf_select + per_band_flag */
const signed char tf_select_table[4][8] = {
    /*isTransient=0     isTransient=1 */
    {0, -1, 0, -1, 0, -1, 0, -1}, /* 2.5 ms */
    {0, -1, 0, -2, 1, 0, 1, -1},  /* 5 ms */
    {0, -2, 0, -3, 2, 0, 1, -1},  /* 10 ms */
    {0, -2, 0, -3, 3, 0, 1, -1},  /* 20 ms */
};
//----------------------------------------------------------------------------------------------------------------------

void init_caps(int32_t *cap, int32_t LM, int32_t C) {
    int32_t i;
    for (i = 0; i < m_CELTMode.nbEBands; i++)
    {
        int32_t N;
        N = (eband5ms[i + 1] - eband5ms[i]) << LM;
        cap[i] = (cache_caps50[m_CELTMode.nbEBands * (2 * LM + C - 1) + i] + 64) * C * N >> 2;
    }
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t celt_lcg_rand(uint32_t seed) {
    return 1664525 * seed + 1013904223;
}
//----------------------------------------------------------------------------------------------------------------------

/* This is a cos() approximation designed to be bit-exact on any platform. Bit exactness
   with this approximation is important because it has an impact on the bit allocation */
int16_t bitexact_cos(int16_t x) {
    int32_t tmp;
    int16_t x2;
    tmp = (4096 + ((int32_t)(x) * (x))) >> 13;
    assert(tmp <= 32767);
    x2 = tmp;
    x2 = (32767 - x2) + FRAC_MUL16(x2, (-7651 + FRAC_MUL16(x2, (8277 + FRAC_MUL16(-626, x2)))));
    assert(x2 <= 32766);
    return 1 + x2;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t bitexact_log2tan(int32_t isin, int32_t icos) {
    int32_t lc;
    int32_t ls;
    lc = EC_ILOG(icos);
    ls = EC_ILOG(isin);
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + FRAC_MUL16(isin, FRAC_MUL16(isin, -2597) + 7932) - FRAC_MUL16(icos, FRAC_MUL16(icos, -2597) + 7932);
}
//----------------------------------------------------------------------------------------------------------------------

/* De-normalise the energy to produce the synthesis from the unit-energy bands */
void denormalise_bands(const int16_t * X, int32_t * freq,
                       const int16_t *bandLogE, int32_t end, int32_t M, int32_t silence) {
    int32_t start = 0;
    int32_t i, N;
    int32_t bound;
    int32_t * f;
    const int16_t * x;
    const int16_t *eBands = eband5ms;
    N = M * m_CELTMode.shortMdctSize;
    bound = M * eBands[end];
    if (silence) {
        bound = 0;
        start = end = 0;
    }
    f = freq;
    x = X + M * eBands[start];
    for (i = 0; i < M * eBands[start]; i++)
        *f++ = 0;
    for (i = start; i < end; i++) {
        int32_t j, band_end;
        int16_t g;
        int16_t lg;

        int32_t shift;

        j = M * eBands[i];
        band_end = M * eBands[i + 1];
        lg = SATURATE16(ADD32(bandLogE[i], SHL32((int32_t)eMeans[i], 6)));

        /* Handle the integer part of the log energy */
        shift = 16 - (lg >> 10);
        if (shift > 31) {
            shift = 0;
            g = 0;
        }
        else {
            /* Handle the fractional part. */
            g = celt_exp2_frac(lg & ((1 << 10) - 1));
        }
        /* Handle extreme gains with negative shift. */
        if (shift < 0) {
            /* For shift <= -2 and g > 16384 we'd be likely to overflow, so we're
               capping the gain here, which is equivalent to a cap of 18 on lg.
               This shouldn't trigger unless the bitstream is already corrupted. */
            if (shift <= -2) {
                g = 16384;
                shift = -2;
            }
            do {
                *f++ = SHL32(MULT16_16(*x++, g), -shift);
            } while (++j < band_end);
        }
        else

            /* Be careful of the fixed-point "else" just above when changing this code */
            do {
                *f++ = MULT16_16(*x++, g) >> shift;
            } while (++j < band_end);
    }
    assert(start <= end);
    memset(&freq[bound], 0, (N - bound) * sizeof(*freq));
}
//----------------------------------------------------------------------------------------------------------------------

/* This prevents energy collapse for transients with multiple short MDCTs */
void anti_collapse(int16_t *X_, uint8_t *collapse_masks, int32_t LM, int32_t C, int32_t size,
                   const int16_t *logE, const int16_t *prev1logE, const int16_t *prev2logE, const int32_t *pulses,
                   uint32_t seed){
    int32_t c, i, j, k;
    const uint8_t  end = cdec->end;  // 21
    for (i = 0; i < end; i++) {
        int32_t N0;
        int16_t thresh, sqrt_1;
        int32_t depth;

        int32_t shift;
        int32_t thresh32;

        N0 = eband5ms[i + 1] - eband5ms[i];
        /* depth in 1/8 bits */
        assert(pulses[i] >= 0);
        assert(eband5ms[i + 1] - eband5ms[i] > 0);
        depth = ((1 + pulses[i]) / (eband5ms[i + 1] - eband5ms[i])) >> LM;

        thresh32 = celt_exp2(-SHL16(depth, 10 - BITRES)) >> 1;
        thresh = MULT16_32_Q15(QCONST16(0.5f, 15), min(32767, thresh32)); {
            int32_t t;
            t = N0 << LM;
            if(t < 1) log_e("celt_ilog2 %i", t);
            shift = celt_ilog2(t) >> 1;
            t = SHL32(t, (7 - shift) << 1);
            sqrt_1 = celt_rsqrt_norm(t);
        }

        c = 0;
        do {
            int16_t *X;
            int16_t prev1;
            int16_t prev2;
            int32_t Ediff;
            int16_t r;
            int32_t renormalize = 0;
            prev1 = prev1logE[c * m_CELTMode.nbEBands + i];
            prev2 = prev2logE[c * m_CELTMode.nbEBands + i];
            if (C == 1) {
                prev1 = max(prev1, prev1logE[m_CELTMode.nbEBands + i]);
                prev2 = max(prev2, prev2logE[m_CELTMode.nbEBands + i]);
            }
            Ediff = EXTEND32(logE[c * m_CELTMode.nbEBands + i]) - EXTEND32(min(prev1, prev2));
            Ediff = max(0, Ediff);

            if (Ediff < 16384) {
                int32_t r32 = celt_exp2(-(int16_t)(Ediff >> 1));
                r = 2 * min(16383, r32);
            }
            else {
                r = 0;
            }
            if (LM == 3)
                r = MULT16_16_Q14(23170, min(23169, r));
            r = SHR16(min(thresh, r), 1);
            r = MULT16_16_Q15(sqrt_1, r) >> shift;

            X = X_ + c * size + (eband5ms[i] << LM);
            for (k = 0; k < 1 << LM; k++) {
                /* Detect collapse */
                if (!(collapse_masks[i * C + c] & 1 << k)) {
                    /* Fill with noise */
                    for (j = 0; j < N0; j++) {
                        seed = celt_lcg_rand(seed);
                        X[(j << LM) + k] = (seed & 0x8000 ? r : -r);
                    }
                    renormalize = 1;
                }
            }
            /* We just added some energy, so we need to renormalise */
            if (renormalize){
                // if(*X > 0xFFFF) log_e("X=%i", *X);
                renormalise_vector(X, N0 << LM, 32767);
            }
        } while (++c < C);
    }
}
//----------------------------------------------------------------------------------------------------------------------

/* Compute the weights to use for optimizing normalized distortion across channels. We use the amplitude to weight
   square distortion, which means that we use the square root of the value we would have been using if we wanted to
   minimize the MSE in the non-normalized domain. This roughly corresponds to some quick-and-dirty perceptual
   experiments I ran to measure inter-aural masking (there doesn't seem to be any published data on the topic). */
void compute_channel_weights(int32_t Ex, int32_t Ey, int16_t w[2]) {
    int32_t minE;

    int32_t shift;

    minE = min(Ex, Ey);
    /* Adjustment to make the weights a bit more conservative. */
    Ex = ADD32(Ex, minE / 3);
    Ey = ADD32(Ey, minE / 3);

    if(EPSILON + max(Ex, Ey) < 1) log_e("celt_ilog2 %i", EPSILON + max(Ex, Ey));
    shift = celt_ilog2(EPSILON + max(Ex, Ey)) - 14;

    w[0] = VSHR32(Ex, shift);
    w[1] = VSHR32(Ey, shift);
}
//----------------------------------------------------------------------------------------------------------------------

void stereo_split(int16_t * X, int16_t * Y, int32_t N) {
    int32_t j;
    for (j = 0; j < N; j++) {
        int32_t r, l;
        l = MULT16_16(QCONST16(.70710678f, 15), X[j]);
        r = MULT16_16(QCONST16(.70710678f, 15), Y[j]);
        X[j] = (int16_t)(ADD32(l, r) >> 15);
        Y[j] = (int16_t)(SUB32(r, l) >> 15);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void stereo_merge(int16_t * X, int16_t * Y, int16_t mid, int32_t N){
    int32_t j;
    int32_t xp = 0, side = 0;
    int32_t El, Er;
    int16_t mid2;

    int32_t kl = 0, kr = 0;

    int32_t t, lgain, rgain;

    /* Compute the norm of X+Y and X-Y as |X|^2 + |Y|^2 +/- sum(xy) */
    dual_inner_prod(Y, X, Y, N, &xp, &side);
    /* Compensating for the mid normalization */
    xp = MULT16_32_Q15(mid, xp);
    /* mid and side are in Q15, not Q14 like X and Y */
    mid2 = SHR16(mid, 1);
    El = MULT16_16(mid2, mid2) + side - 2 * xp;
    Er = MULT16_16(mid2, mid2) + side + 2 * xp;
    if (Er < QCONST32(6e-4f, 28) || El < QCONST32(6e-4f, 28)) {
        memcpy(Y, X, N * sizeof(*Y));
        return;
    }

    if(El < 1) log_e("celt_ilog2 %i", El);
    kl = celt_ilog2(El) >> 1;
    if(Er < 1) log_e("celt_ilog2 %i", Er);
    kr = celt_ilog2(Er) >> 1;

    t = VSHR32(El, (kl - 7) << 1);
    lgain = celt_rsqrt_norm(t);
    t = VSHR32(Er, (kr - 7) << 1);
    rgain = celt_rsqrt_norm(t);

    if (kl < 7)  kl = 7;
    if (kr < 7)  kr = 7;

    for (j = 0; j < N; j++) {
        int16_t r, l;
        /* Apply mid scaling (side is already scaled) */
        l = MULT16_16_P15(mid, X[j]);
        r = Y[j];
        X[j] = (int16_t)(PSHR(MULT16_16(lgain, SUB16(l, r)), kl + 1));
        Y[j] = (int16_t)(PSHR(MULT16_16(rgain, ADD16(l, r)), kr + 1));
    }
}
//----------------------------------------------------------------------------------------------------------------------

/* Indexing table for converting from natural Hadamard to ordery Hadamard. This is essentially a bit-reversed Gray,
   on top of which we've added an inversion of the order because we want the DC at the end rather than the beginning.
   The lines are for N=2, 4, 8, 16 */
static const int32_t ordery_table[] = {
    1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2, 15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
};
//----------------------------------------------------------------------------------------------------------------------

void deinterleave_hadamard(int16_t *X, int32_t N0, int32_t stride, int32_t hadamard){
    int32_t i, j;
    int32_t N;
    N = N0 * stride;

    assert(N <= 176);
    int16_t* tmp = s_tmpBuff;

    assert(stride > 0);
    if (hadamard) {
        const int32_t *ordery = ordery_table + stride - 2;
        for (i = 0; i < stride; i++) {
            for (j = 0; j < N0; j++)
                tmp[ordery[i] * N0 + j] = X[j * stride + i];
        }
    }
    else {
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[i * N0 + j] = X[j * stride + i];
    }
    memcpy(X, tmp, N * sizeof(*X));
}
//----------------------------------------------------------------------------------------------------------------------

void interleave_hadamard(int16_t *X, int32_t N0, int32_t stride, int32_t hadamard){
    int32_t i, j;
    int32_t N;
    N = N0 * stride;

    assert(N <= 176);
    int16_t* tmp = s_tmpBuff;

    if (hadamard) {
        const int32_t *ordery = ordery_table + stride - 2;
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[j * stride + i] = X[ordery[i] * N0 + j];
    }
    else {
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[j * stride + i] = X[i * N0 + j];
    }
    memcpy(X, tmp, N * sizeof(*X));
}
//----------------------------------------------------------------------------------------------------------------------

void haar1(int16_t *X, int32_t N0, int32_t stride) {
    int32_t i, j;
    N0 >>= 1;
    for (i = 0; i < stride; i++)
        for (j = 0; j < N0; j++) {
            int32_t tmp1, tmp2;
            tmp1 = MULT16_16(QCONST16(.70710678f, 15), X[stride * 2 * j + i]);
            tmp2 = MULT16_16(QCONST16(.70710678f, 15), X[stride * (2 * j + 1) + i]);
            X[stride * 2 * j + i] = (int16_t)(PSHR(ADD32(tmp1, tmp2), 15));
            X[stride * (2 * j + 1) + i] = (int16_t)(PSHR(SUB32(tmp1, tmp2), 15));
        }
}
//----------------------------------------------------------------------------------------------------------------------

int32_t compute_qn(int32_t N, int32_t b, int32_t offset, int32_t pulse_cap, int32_t stereo) {
    static const int16_t exp2_table8[8] =
        {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    int32_t qn, qb;
    int32_t N2 = 2 * N - 1;
    if (stereo && N == 2)
        N2--;
    /* The upper limit ensures that in a stereo split with itheta==16384, we'll
        always have enough bits left over to code at least one pulse in the
        side; otherwise it would collapse, since it doesn't get folded. */
    qb = celt_sudiv(b + N2 * offset, N2);
    qb = min(b - pulse_cap - (4 << BITRES), qb);

    qb = min(8 << BITRES, qb);

    if (qb < (1 << BITRES >> 1)) {
        qn = 1;
    }
    else {
        qn = exp2_table8[qb & 0x7] >> (14 - (qb >> BITRES));
        qn = (qn + 1) >> 1 << 1;
    }
    assert(qn <= 256);
    return qn;
}
//----------------------------------------------------------------------------------------------------------------------

void compute_theta(struct split_ctx *sctx, int16_t *X, int16_t *Y, int32_t N, int32_t *b, int32_t B,
                          int32_t __B0, int32_t LM, int32_t stereo, int32_t *fill) {
    int32_t qn;
    int32_t itheta = 0;
    int32_t delta;
    int32_t imid, iside;
    int32_t qalloc;
    int32_t pulse_cap;
    int32_t offset;
    int32_t tell;
    int32_t inv = 0;
    int32_t i;
    int32_t intensity;
    i = s_band_ctx.i;
    intensity = s_band_ctx.intensity;

    /* Decide on the resolution to give to the split parameter theta */
    pulse_cap = logN400[i] + LM * (1 << BITRES);
    offset = (pulse_cap >> 1) - (stereo && N == 2 ? QTHETA_OFFSET_TWOPHASE : QTHETA_OFFSET);
    qn = compute_qn(N, *b, offset, pulse_cap, stereo);
    if (stereo && i >= intensity)
        qn = 1;
    tell = ec_tell_frac();
    if (qn != 1) {
        /* Entropy coding of the angle. We use a uniform pdf for the time split, a step for stereo,
           and a triangular one for the rest. */
        if (stereo && N > 2) {
            int32_t p0 = 3;
            int32_t x = itheta;
            int32_t x0 = qn / 2;
            int32_t ft = p0 * (x0 + 1) + x0;
            /* Use a probability of p0 up to itheta=8192 and then use 1 after */

            int32_t fs;
            fs = ec_decode(ft);
            if (fs < (x0 + 1) * p0)
                x = fs / p0;
            else
                x = x0 + 1 + (fs - (x0 + 1) * p0);
            ec_dec_update(x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0, x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0, ft);
            itheta = x;

        }
        else if (__B0 > 1 || stereo) {
            /* Uniform pdf */
            itheta = ec_dec_uint(qn + 1);
        }
        else {
            int32_t fs = 1, ft;
            ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
            /* Triangular pdf */
            int32_t fl = 0;
            int32_t fm;
            fm = ec_decode(ft);
            if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1))
            {
                itheta = (isqrt32(8 * (uint32_t)fm + 1) - 1) >> 1;
                fs = itheta + 1;
                fl = itheta * (itheta + 1) >> 1;
            }
            else
            {
                itheta = (2 * (qn + 1) - isqrt32(8 * (uint32_t)(ft - fm - 1) + 1)) >> 1;
                fs = qn + 1 - itheta;
                fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
            }
            ec_dec_update(fl, fl + fs, ft);

        }
        assert(itheta >= 0);
        assert(qn > 0);
        itheta = (int32_t)itheta * 16384 / qn;
            stereo_split(X, Y, N);
        /* NOTE: Renormalising X and Y *may* help fixed-point a bit at very high rate.
                 Let's do that at higher complexity */
    }
    else if (stereo) {
        if (*b > 2 << BITRES && s_band_ctx.remaining_bits > 2 << BITRES) {
            inv = ec_dec_bit_logp(2);
        }
        else
            inv = 0;
        /* inv flag override to avoid problems with downmixing. */
        if (s_band_ctx.disable_inv)
            inv = 0;
        itheta = 0;
    }
    qalloc = ec_tell_frac() - tell;
    *b -= qalloc;

    if (itheta == 0) {
        imid = 32767;
        iside = 0;
        *fill &= (1 << B) - 1;
        delta = -16384;
    }
    else if (itheta == 16384){
        imid = 0;
        iside = 32767;
        *fill &= ((1 << B) - 1) << B;
        delta = 16384;
    }
    else {
        imid = bitexact_cos((int16_t)itheta);
        iside = bitexact_cos((int16_t)(16384 - itheta));
        /* This is the mid vs side allocation that minimizes squared error
           in that band. */
        delta = FRAC_MUL16((N - 1) << 7, bitexact_log2tan(iside, imid));
    }

    sctx->inv = inv;
    sctx->imid = imid;
    sctx->iside = iside;
    sctx->delta = delta;
    sctx->itheta = itheta;
    sctx->qalloc = qalloc;
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t quant_band_n1(int16_t *X, int16_t *Y, int32_t b,  int16_t *lowband_out) {

    int32_t c;
    int32_t stereo;
    int16_t *x = X;

    stereo = Y != NULL;
    c = 0;
    do {
        if (s_band_ctx.remaining_bits >= 1 << BITRES) {
            s_band_ctx.remaining_bits -= 1 << BITRES;
            b -= 1 << BITRES;
        }
        if (s_band_ctx.resynth)
            x[0] = 16384;  // NORM_SCALING
        x = Y;
    } while (++c < 1 + stereo);
    if (lowband_out)
        lowband_out[0] = SHR16(X[0], 4);
    return 1;
}
//----------------------------------------------------------------------------------------------------------------------

/* This function is responsible for encoding and decoding a mono partition. It can split the band in two and transmit
   the energy difference with the two half-bands. It can be called recursively so bands can end up being
   split in 8 parts. */
uint32_t quant_partition(int16_t *X, int32_t N, int32_t b, int32_t B, int16_t *lowband, int32_t LM,
                                int16_t gain, int32_t fill){
    const uint8_t *cache;
    int32_t q;
    int32_t curr_bits;
    int32_t imid = 0, iside = 0;
    int32_t _B0 = B;
    int16_t mid = 0, side = 0;
    uint32_t cm = 0;
    int16_t *Y = NULL;
    int32_t i;
    int32_t spread;
    i = s_band_ctx.i;
    spread = s_band_ctx.spread;

    /* If we need 1.5 more bit than we can produce, split the band in two. */
    cache = cache_bits50 + cache_index50[(LM + 1) * m_CELTMode.nbEBands + i];
    if (LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
        int32_t mbits, sbits, delta;
        int32_t itheta;
        int32_t qalloc;
        struct split_ctx sctx;
        int16_t *next_lowband2 = NULL;
        int32_t rebalance;

        N >>= 1;
        Y = X + N;
        LM -= 1;
        if (B == 1)
            fill = (fill & 1) | (fill << 1);
        B = (B + 1) >> 1;

        compute_theta(&sctx, X, Y, N, &b, B, _B0, LM, 0, &fill);
        imid = sctx.imid;
        iside = sctx.iside;
        delta = sctx.delta;
        itheta = sctx.itheta;
        qalloc = sctx.qalloc;

        mid = imid;
        side = iside;

        /* Give more bits to low-energy MDCTs than they would otherwise deserve */
        if (_B0 > 1 && (itheta & 0x3fff)) {
            if (itheta > 8192)
                /* Rough approximation for pre-echo masking */
                delta -= delta >> (4 - LM);
            else
                /* Corresponds to a forward-masking slope of 1.5 dB per 10 ms */
                delta = min(0, delta + (N << BITRES >> (5 - LM)));
        }
        mbits = max(0, min(b, (b - delta) / 2));
        sbits = b - mbits;
        s_band_ctx.remaining_bits -= qalloc;

        if (lowband)
            next_lowband2 = lowband + N; /* >32-bit split case */

        rebalance = s_band_ctx.remaining_bits;
        if (mbits >= sbits)  {
            cm = quant_partition(X, N, mbits, B, lowband, LM,
                                 MULT16_16_P15(gain, mid), fill);
            rebalance = mbits - (rebalance - s_band_ctx.remaining_bits);
            if (rebalance > 3 << BITRES && itheta != 0)
                sbits += rebalance - (3 << BITRES);
            cm |= quant_partition(Y, N, sbits, B, next_lowband2, LM,
                                  MULT16_16_P15(gain, side), fill >> B)
                  << (_B0 >> 1);
        }
        else {
            cm = quant_partition(Y, N, sbits, B, next_lowband2, LM,
                                 MULT16_16_P15(gain, side), fill >> B)
                 << (_B0 >> 1);
            rebalance = sbits - (rebalance - s_band_ctx.remaining_bits);
            if (rebalance > 3 << BITRES && itheta != 16384)
                mbits += rebalance - (3 << BITRES);
            cm |= quant_partition(X, N, mbits, B, lowband, LM,
                                  MULT16_16_P15(gain, mid), fill);
        }
    }
    else {
        /* This is the basic no-split case */
        q = bits2pulses(i, LM, b);
        curr_bits = pulses2bits(i, LM, q);
        s_band_ctx.remaining_bits -= curr_bits;

        /* Ensures we can never bust the budget */
        while (s_band_ctx.remaining_bits < 0 && q > 0) {
            s_band_ctx.remaining_bits += curr_bits;
            q--;
            curr_bits = pulses2bits(i, LM, q);
            s_band_ctx.remaining_bits -= curr_bits;
        }

        if (q != 0) {
            int32_t K = get_pulses(q);

            /* Finally do the actual quantization */
            cm = alg_unquant(X, N, K, spread, B, gain);

        }
        else {
            /* If there's no pulse, fill the band anyway */
            int32_t j;
            if (s_band_ctx.resynth)
            {
                uint32_t cm_mask;
                /* B can be as large as 16, so this shift might overflow an int32_t on a
                   16-bit platform; use a long to get defined behavior.*/
                cm_mask = (uint32_t)(1UL << B) - 1;
                fill &= cm_mask;
                if (!fill) {
                    memset(&X, 0, N * sizeof(X));
                }
                else  {
                    if (lowband == NULL) {
                        /* Noise */
                        for (j = 0; j < N; j++) {
                            s_band_ctx.seed = celt_lcg_rand(s_band_ctx.seed);
                            X[j] = (int16_t)((int32_t)s_band_ctx.seed >> 20);
                        }
                        cm = cm_mask;
                    }
                    else {
                        /* Folded spectrum */
                        for (j = 0; j < N; j++) {
                            int16_t tmp;
                            s_band_ctx.seed = celt_lcg_rand(s_band_ctx.seed);
                            /* About 48 dB below the "normal" folding level */
                            tmp = QCONST16(1.0f / 256, 10);
                            tmp = (s_band_ctx.seed) & 0x8000 ? tmp : -tmp;
                            X[j] = lowband[j] + tmp;
                        }
                        cm = fill;
                    }
                    // if(*X > 0xFFFF) log_e("X=%i", *X);
                    renormalise_vector(X, N, gain);
                }
            }
        }
    }
    return cm;
}
//----------------------------------------------------------------------------------------------------------------------

/* This function is responsible for encoding and decoding a band for the mono case. */
uint32_t quant_band(int16_t *X, int32_t N, int32_t b, int32_t B, int16_t *lowband, int32_t LM,
                           int16_t *lowband_out, int16_t gain, int16_t *lowband_scratch, int32_t fill) {
    int32_t N0 = N;
    int32_t N_B = N;
    int32_t N__B0;
    int32_t _B0 = B;
    int32_t time_divide = 0;
    int32_t recombine = 0;
    int32_t longBlocks;
    uint32_t cm = 0;
    int32_t k;
    int32_t tf_change;
    tf_change = s_band_ctx.tf_change;

    longBlocks = _B0 == 1;

    assert(B > 0);
    N_B = N_B / B;

    /* Special case for one sample */
    if (N == 1) {
        return quant_band_n1(X, NULL, b, lowband_out);
    }

    if (tf_change > 0)
        recombine = tf_change;
    /* Band recombining to increase frequency resolution */

    if (lowband_scratch && lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || _B0 > 1)) {
        memcpy(lowband_scratch, lowband, N * sizeof(*lowband_scratch));
        lowband = lowband_scratch;
    }

    for (k = 0; k < recombine; k++) {
        static const uint8_t bit_interleave_table[16] = {
            0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
        if (lowband)
            haar1(lowband, N >> k, 1 << k);
        fill = bit_interleave_table[fill & 0xF] | bit_interleave_table[fill >> 4] << 2;
    }
    B >>= recombine;
    N_B <<= recombine;

    /* Increasing the time resolution */
    while ((N_B & 1) == 0 && tf_change < 0) {
        if (lowband)
            haar1(lowband, N_B, B);
        fill |= fill << B;
        B <<= 1;
        N_B >>= 1;
        time_divide++;
        tf_change++;
    }
    _B0 = B;
    N__B0 = N_B;

    /* Reorganize the samples in time order instead of frequency order */
    if (_B0 > 1) {
        if (lowband)
            deinterleave_hadamard(lowband, N_B >> recombine, _B0 << recombine, longBlocks);
    }

    cm = quant_partition(X, N, b, B, lowband, LM, gain, fill);

    if (s_band_ctx.resynth) {
        /* Undo the sample reorganization going from time order to frequency order */
        if (_B0 > 1)
            interleave_hadamard(X, N_B >> recombine, _B0 << recombine, longBlocks);

        /* Undo time-freq changes that we did earlier */
        N_B = N__B0;
        B = _B0;
        for (k = 0; k < time_divide; k++) {
            B >>= 1;
            N_B <<= 1;
            cm |= cm >> B;
            haar1(X, N_B, B);
        }

        for (k = 0; k < recombine; k++) {
            static const uint8_t bit_deinterleave_table[16] = {
                0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
            cm = bit_deinterleave_table[cm];
            haar1(X, N0 >> k, 1 << k);
        }
        B <<= recombine;

        /* Scale output for later folding */
        if (lowband_out) {
            int32_t j;
            int16_t n;
            n = celt_sqrt(SHL32(EXTEND32(N0), 22));
            for (j = 0; j < N0; j++)
                lowband_out[j] = MULT16_16_Q15(n, X[j]);
        }
        cm &= (1 << B) - 1;
    }
    return cm;
}
//----------------------------------------------------------------------------------------------------------------------

/* This function is responsible for encoding and decoding a band for the stereo case. */
uint32_t quant_band_stereo(int16_t *X, int16_t *Y, int32_t N, int32_t b, int32_t B, int16_t *lowband,
                                  int32_t LM, int16_t *lowband_out, int16_t *lowband_scratch, int32_t fill) {
    int32_t imid = 0, iside = 0;
    int32_t inv = 0;
    int16_t mid = 0, side = 0;
    uint32_t cm = 0;
    int32_t mbits, sbits, delta;
    int32_t itheta;
    int32_t qalloc;
    struct split_ctx sctx;
    int32_t orig_fill;

    /* Special case for one sample */
    if (N == 1){
        return quant_band_n1(X, Y, b, lowband_out);
    }

    orig_fill = fill;

    compute_theta(&sctx, X, Y, N, &b, B, B, LM, 1, &fill);
    inv = sctx.inv;
    imid = sctx.imid;
    iside = sctx.iside;
    delta = sctx.delta;
    itheta = sctx.itheta;
    qalloc = sctx.qalloc;

    mid = imid;
    side = iside;

    /* This is a special case for N=2 that only works for stereo and takes
       advantage of the fact that mid and side are orthogonal to encode
       the side with just one bit. */
    if (N == 2) {
        int32_t c;
        int32_t sign = 0;
        int16_t *x2, *y2;
        mbits = b;
        sbits = 0;
        /* Only need one bit for the side. */
        if (itheta != 0 && itheta != 16384)
            sbits = 1 << BITRES;
        mbits -= sbits;
        c = itheta > 8192;
        s_band_ctx.remaining_bits -= qalloc + sbits;

        x2 = c ? Y : X;
        y2 = c ? X : Y;
        if (sbits) {
            sign = ec_dec_bits(1);
        }
        sign = 1 - 2 * sign;
        /* We use orig_fill here because we want to fold the side, but if
           itheta==16384, we'll have cleared the low bits of fill. */
        cm = quant_band(x2, N, mbits, B, lowband, LM, lowband_out, 32767,
                        lowband_scratch, orig_fill);
        /* We don't split N=2 bands, so cm is either 1 or 0 (for a fold-collapse),
           and there's no need to worry about mixing with the other channel. */
        y2[0] = -sign * x2[1];
        y2[1] = sign * x2[0];
        if (s_band_ctx.resynth) {
            int16_t tmp;
            X[0] = MULT16_16_Q15(mid, X[0]);
            X[1] = MULT16_16_Q15(mid, X[1]);
            Y[0] = MULT16_16_Q15(side, Y[0]);
            Y[1] = MULT16_16_Q15(side, Y[1]);
            tmp = X[0];
            X[0] = SUB16(tmp, Y[0]);
            Y[0] = ADD16(tmp, Y[0]);
            tmp = X[1];
            X[1] = SUB16(tmp, Y[1]);
            Y[1] = ADD16(tmp, Y[1]);
        }
    }
    else {
        /* "Normal" split code */
        int32_t rebalance;

        mbits = max(0, min(b, (b - delta) / 2));
        sbits = b - mbits;
        s_band_ctx.remaining_bits -= qalloc;

        rebalance = s_band_ctx.remaining_bits;
        if (mbits >= sbits) {
            /* In stereo mode, we do not apply a scaling to the mid because we need the normalized
               mid for folding later. */
            cm = quant_band(X, N, mbits, B, lowband, LM, lowband_out, 32767,
                            lowband_scratch, fill);
            rebalance = mbits - (rebalance - s_band_ctx.remaining_bits);
            if (rebalance > 3 << BITRES && itheta != 0)
                sbits += rebalance - (3 << BITRES);

            /* For a stereo split, the high bits of fill are always zero, so no
               folding will be done to the side. */
            cm |= quant_band(Y, N, sbits, B, NULL, LM, NULL, side, NULL, fill >> B);
        }
        else {
            /* For a stereo split, the high bits of fill are always zero, so no
               folding will be done to the side. */
            cm = quant_band(Y, N, sbits, B, NULL, LM, NULL, side, NULL, fill >> B);
            rebalance = sbits - (rebalance - s_band_ctx.remaining_bits);
            if (rebalance > 3 << BITRES && itheta != 16384)
                mbits += rebalance - (3 << BITRES);
            /* In stereo mode, we do not apply a scaling to the mid because we need the normalized
               mid for folding later. */
            cm |= quant_band(X, N, mbits, B, lowband, LM, lowband_out, 32767,
                             lowband_scratch, fill);
        }
    }
    if (s_band_ctx.resynth) {
        if (N != 2)
            stereo_merge(X, Y, mid, N);
        if (inv)
        {
            int32_t j;
            for (j = 0; j < N; j++)
                Y[j] = -Y[j];
        }
    }
    return cm;
}
//----------------------------------------------------------------------------------------------------------------------

void special_hybrid_folding(int16_t *norm, int16_t *norm2, int32_t M, int32_t dual_stereo){
    int32_t n1, n2;
    const int16_t * eBands = eband5ms;
    n1 = M * (eBands[1] - eBands[0]);
    n2 = M * (eBands[2] - eBands[1]);
    /* Duplicate enough of the first band folding data to be able to fold the second band.
       Copies no data for CELT-only mode. */
    memcpy(&norm[n1], &norm[2 * n1 - n2], (n2 - n1) * sizeof(*norm));
    if (dual_stereo)
        memcpy(&norm2[n1], &norm2[2 * n1 - n2], (n2 - n1) * sizeof(*norm2));
}
//----------------------------------------------------------------------------------------------------------------------

void quant_all_bands(int16_t *X_, int16_t *Y_, uint8_t *collapse_masks, int32_t *pulses,
                     int32_t shortBlocks, int32_t spread,
                     int32_t dual_stereo, int32_t intensity, int32_t *tf_res, int32_t total_bits, int32_t balance,
                     int32_t LM, int32_t codedBands){

    int32_t i;
    int32_t remaining_bits;
    const int16_t * eBands = eband5ms;
    int16_t * norm, * norm2;
    int16_t *lowband_scratch;
    int32_t B;
    int32_t M;
    int32_t lowband_offset;
    int32_t update_lowband = 1;
    int32_t C = Y_ != NULL ? 2 : 1;
    int32_t norm_offset;
    int32_t resynth = 1;
    const uint8_t end = cdec->end;  // 21
    uint8_t disable_inv = cdec->disable_inv; // 1- mono, 0- stereo

    M = 1 << LM;
    B = shortBlocks ? M : 1;
    norm_offset = M * eBands[0];
    /* No need to allocate norm for the last band because we don't need an
       output in that band. */

    assert(C * (M * eBands[m_CELTMode.nbEBands - 1] - norm_offset) >= 1248);
    norm = s_normBuff;

    norm2 = norm + M * eBands[m_CELTMode.nbEBands - 1] - norm_offset;

    /* For decoding, we can use the last band as scratch space because we don't need that
       scratch space for the last band and we don't care about the data there until we're
       decoding the last band. */

    lowband_scratch = X_ + M * eBands[m_CELTMode.nbEBands - 1];

    lowband_offset = 0;
    s_band_ctx.encode = 0;
    s_band_ctx.intensity = intensity;
    s_band_ctx.seed = 0;
    s_band_ctx.spread = spread;
    s_band_ctx.disable_inv = disable_inv; // 0 - stereo, 1 - mono
    s_band_ctx.resynth = resynth;
    s_band_ctx.theta_round = 0;
    /* Avoid injecting noise in the first band on transients. */
    s_band_ctx.avoid_split_noise = B > 1;
    for (i = 0; i < end; i++){
        int32_t tell;
        int32_t b;
        int32_t N;
        int32_t curr_balance;
        int32_t effective_lowband = -1;
        int16_t * X, * Y;
        int32_t tf_change = 0;
        uint32_t x_cm;
        uint32_t y_cm;
        int32_t last;

        s_band_ctx.i = i;
        last = (i == end - 1);

        X = X_ + M * eBands[i];
        if (Y_ != NULL)
            Y = Y_ + M * eBands[i];
        else
            Y = NULL;
        N = M * eBands[i + 1] - M * eBands[i];
        assert(N > 0);
        tell = ec_tell_frac();

        /* Compute how many bits we want to allocate to this band */
        if (i != 0)
            balance -= tell;
        remaining_bits = total_bits - tell - 1;
        s_band_ctx.remaining_bits = remaining_bits;
        if (i <= codedBands - 1){
            curr_balance = celt_sudiv(balance, min(3, codedBands - i));
            b = max(0, min(16383, min(remaining_bits + 1, pulses[i] + curr_balance)));
        }
        else {
            b = 0;
        }

        if (resynth && (M * eBands[i] - N >= M * eBands[0] || i == 1) && (update_lowband || lowband_offset == 0))
            lowband_offset = i;
        if (i == 1)
            special_hybrid_folding(norm, norm2, M, dual_stereo);

        tf_change = tf_res[i];
        s_band_ctx.tf_change = tf_change;
        if (i >= m_CELTMode.effEBands) {
            X = norm;
            if (Y_ != NULL)
                Y = norm;
            lowband_scratch = NULL;
        }
        if (last)
            lowband_scratch = NULL;

        /* Get a conservative estimate of the collapse_mask's for the bands we're
           going to be folding from. */
        if (lowband_offset != 0 && (spread != 3 || B > 1 || tf_change < 0)) { // SPREAD_AGGRESSIVE
            int32_t fold_start;
            int32_t fold_end;
            int32_t fold_i;
            /* This ensures we never repeat spectral content within one band */
            effective_lowband = max(0, M * eBands[lowband_offset] - norm_offset - N);
            fold_start = lowband_offset;
            while (M * eBands[--fold_start] > effective_lowband + norm_offset)
                ;
            fold_end = lowband_offset - 1;

            while (++fold_end < i && M * eBands[fold_end] < effective_lowband + norm_offset + N)
                ;

            x_cm = y_cm = 0;
            fold_i = fold_start;
            do {
                x_cm |= collapse_masks[fold_i * C + 0];
                y_cm |= collapse_masks[fold_i * C + C - 1];
            } while (++fold_i < fold_end);
        }
        /* Otherwise, we'll be using the LCG to fold, so all blocks will (almost
           always) be non-zero. */
        else
            x_cm = y_cm = (1 << B) - 1;

        if (dual_stereo && i == intensity) {
            int32_t j;

            /* Switch off dual stereo to do intensity. */
            dual_stereo = 0;
            if (resynth)
                for (j = 0; j < M * eBands[i] - norm_offset; j++)
                    norm[j] = HALF32(norm[j] + norm2[j]);
        }
        if (dual_stereo) {
            x_cm = quant_band(X, N, b / 2, B,
                              effective_lowband != -1 ? norm + effective_lowband : NULL, LM,
                              last ? NULL : norm + M * eBands[i] - norm_offset, 32767, lowband_scratch, x_cm);
            y_cm = quant_band(Y, N, b / 2, B,
                              effective_lowband != -1 ? norm2 + effective_lowband : NULL, LM,
                              last ? NULL : norm2 + M * eBands[i] - norm_offset, 32767, lowband_scratch, y_cm);
        }
        else {
            if (Y != NULL) {
                s_band_ctx.theta_round = 0;
                x_cm = quant_band_stereo(X, Y, N, b, B,
                                    effective_lowband != -1 ? norm + effective_lowband : NULL, LM,
                                    last ? NULL : norm + M * eBands[i] - norm_offset, lowband_scratch, x_cm | y_cm);

            }
            else {
                x_cm = quant_band(X, N, b, B,
                                  effective_lowband != -1 ? norm + effective_lowband : NULL, LM,
                                  last ? NULL : norm + M * eBands[i] - norm_offset, 32767, lowband_scratch, x_cm | y_cm);
            }
            y_cm = x_cm;
        }
        collapse_masks[i * C + 0] = (uint8_t)x_cm;
        collapse_masks[i * C + C - 1] = (uint8_t)y_cm;
        balance += pulses[i] + tell;

        /* Update the folding position only as long as we have 1 bit/sample depth. */
        update_lowband = b > (N << BITRES);
        /* We only need to avoid noise on a split for the first band. After that, we
           have folding. */
        s_band_ctx.avoid_split_noise = 0;
    }

}
//----------------------------------------------------------------------------------------------------------------------

int32_t celt_decoder_get_size(int32_t channels){
    static int32_t size;
    size = sizeof(struct CELTDecoder) + (channels * (DECODE_BUFFER_SIZE + m_CELTMode.overlap) - 1) * sizeof(int32_t)
           + channels * 24 * sizeof(int16_t) + 4 * 2 * m_CELTMode.nbEBands * sizeof(int16_t);
    return size;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t celt_decoder_init(int32_t channels){

    // allocate buffers first
    if (channels < 0 || channels > 2)
        return OPUS_BAD_ARG;

    if (cdec == NULL)
        return OPUS_ALLOC_FAIL;
    int n = celt_decoder_get_size(channels);
    memset(cdec, 0, n * sizeof(char));

    cdec->mode = &m_CELTMode;
    cdec->overlap = m_CELTMode.overlap;
    cdec->stream_channels = channels;
    cdec->channels = channels;

    cdec->start = 0;
    cdec->end = cdec->mode->effEBands; // 21
    cdec->signalling = 1;

    if(channels == 1) cdec->disable_inv = 1; else cdec->disable_inv = 0; // 1 mono ,  0 stereo

    celt_decoder_ctl(OPUS_RESET_STATE);

    return OPUS_OK;
}
//----------------------------------------------------------------------------------------------------------------------

// save stack arrays in heap, prefer PSRAM
#ifdef BOARD_HAS_PSRAM
    #define __heap_caps_malloc(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#else
    #define __heap_caps_malloc(size) heap_caps_malloc(size, MALLOC_CAP_DEFAULT)
#endif

bool CELTDecoder_AllocateBuffers(void) {
    size_t omd = celt_decoder_get_size(2);
    if(!cdec)                   {cdec = (CELTDecoder*)            __heap_caps_malloc(omd);}
    if(!s_freqBuff)             {s_freqBuff = (int32_t*)          __heap_caps_malloc(960  * sizeof(int32_t));}
    if(!s_iyBuff)               {s_iyBuff = (int32_t*)            __heap_caps_malloc(96   * sizeof(int32_t));}
    if(!s_normBuff)             {s_normBuff = (int16_t*)          __heap_caps_malloc(1248 * sizeof(int16_t));}
    if(!s_XBuff)                {s_XBuff = (int16_t*)             __heap_caps_malloc(1920 * sizeof(int16_t));}
    if(!s_bits1Buff)            {s_bits1Buff = (int32_t*)         __heap_caps_malloc(21   * sizeof(int32_t));}
    if(!s_bits2Buff)            {s_bits2Buff = (int32_t*)         __heap_caps_malloc(21   * sizeof(int32_t));}
    if(!s_threshBuff)           {s_threshBuff = (int32_t*)        __heap_caps_malloc(21   * sizeof(int32_t));}
    if(!s_trim_offsetBuff)      {s_trim_offsetBuff = (int32_t*)   __heap_caps_malloc(21   * sizeof(int32_t));}
    if(!s_collapse_masksBuff)   {s_collapse_masksBuff = (uint8_t*)__heap_caps_malloc(42   * sizeof(uint8_t));}
    if(!s_tmpBuff)              {s_tmpBuff = (int16_t*)           __heap_caps_malloc(176  * sizeof(int16_t));}

    if(!cdec) {
        CELTDecoder_FreeBuffers();
        log_e("not enough memory to allocate celtdecoder buffers");
        return false;
    }
    return true;
}
//----------------------------------------------------------------------------------------------------------------------
void CELTDecoder_FreeBuffers(){
    if(cdec){free(cdec); cdec = NULL;}
    if(!s_freqBuff) { free(s_freqBuff), s_freqBuff = NULL; }
    if(!s_iyBuff) { free(s_iyBuff), s_iyBuff = NULL; }
    if(!s_normBuff) { free(s_normBuff), s_normBuff = NULL; }
    if(!s_XBuff) { free(s_XBuff), s_XBuff = NULL; }
    if(!s_bits1Buff) { free(s_bits1Buff), s_bits1Buff = NULL; }
    if(!s_bits2Buff) { free(s_bits2Buff), s_bits2Buff = NULL; }
    if(!s_threshBuff) { free(s_threshBuff), s_threshBuff = NULL; }
    if(!s_trim_offsetBuff) { free(s_trim_offsetBuff), s_trim_offsetBuff = NULL; }
    if(!s_collapse_masksBuff) { free(s_collapse_masksBuff), s_collapse_masksBuff = NULL; }
    if(!s_tmpBuff) { free(s_tmpBuff), s_tmpBuff = NULL; }
}
//----------------------------------------------------------------------------------------------------------------------
void CELTDecoder_ClearBuffer(void){
    size_t omd = celt_decoder_get_size(2);
    memset(cdec, 0, omd * sizeof(char));

}
//----------------------------------------------------------------------------------------------------------------------

/* Special case for stereo with no downsampling and no accumulation. This is quite common and we can make it faster by
   processing both channels in the same loop, reducing overhead due to the dependency loop in the IIR filter. */
void deemphasis_stereo_simple(int32_t *in[], int16_t *pcm, int32_t N, const int16_t coef0, int32_t *mem) {
    int32_t * x0;
    int32_t * x1;
    int32_t m0, m1;
    int32_t j;
    x0 = in[0];
    x1 = in[1];
    m0 = mem[0];
    m1 = mem[1];
    for (j = 0; j < N; j++) {
        int32_t tmp0, tmp1;
        tmp0 = x0[j] + m0;
        tmp1 = x1[j] + m1;
        m0 = MULT16_32_Q15(coef0, tmp0);
        m1 = MULT16_32_Q15(coef0, tmp1);
        pcm[2 * j] = sig2word16(tmp0);
        pcm[2 * j + 1] = sig2word16(tmp1);
    }
    mem[0] = m0;
    mem[1] = m1;
}
//----------------------------------------------------------------------------------------------------------------------

void deemphasis(int32_t *in[], int16_t *pcm, int32_t N) {
    int32_t        c;
    int32_t        Nd;
    int32_t        apply_downsampling = 0;
    int16_t        coef0;
    const int32_t  CC = cdec->channels;
    const int16_t *coef = m_CELTMode.preemph;
    int32_t       *mem = cdec->preemph_memD;

    /* Short version for common case. */
    if(CC == 2) {
        deemphasis_stereo_simple(in, pcm, N, coef[0], mem);
        return;
    }

    int32_t scratch[N];
    coef0 = coef[0];
    Nd = N;
    c = 0;
    do {
        int32_t  j;
        int32_t *x;
        int16_t *y;
        int32_t  m = mem[c];
        x = in[c];
        y = pcm + c;

        for(j = 0; j < N; j++) {
            int32_t tmp = x[j] +  m;
            m = MULT16_32_Q15(coef0, tmp);
            y[j * CC] = sig2word16(tmp);
        }

        mem[c] = m;

        if(apply_downsampling) {
            /* Perform down-sampling */

            for(j = 0; j < Nd; j++) y[j * CC] = sig2word16(scratch[j]);
        }
    } while(++c < CC);
}
//----------------------------------------------------------------------------------------------------------------------

void celt_synthesis(int16_t *X, int32_t *out_syn[], int16_t *oldBandE, int32_t C,
                    int32_t isTransient, int32_t LM, int32_t silence) {
    int32_t c, i;
    int32_t M;
    int32_t b;
    int32_t B;
    int32_t N, NB;
    int32_t shift;
    int32_t nbEBands;
    int32_t overlap;
    const int32_t  CC = cdec->channels;
    const uint8_t effEnd = cdec->end;  // 21

    overlap = m_CELTMode.overlap;
    nbEBands = m_CELTMode.nbEBands;
    N = m_CELTMode.shortMdctSize << LM;
    int32_t* freq = s_freqBuff; assert(N <= 960); /**< Interleaved signal MDCTs */
    M = 1 << LM;

    if(isTransient) {
        B = M;
        NB = m_CELTMode.shortMdctSize;
        shift = m_CELTMode.maxLM;
    } else {
        B = 1;
        NB = m_CELTMode.shortMdctSize << LM;
        shift = m_CELTMode.maxLM - LM;
    }

    if(CC == 2 && C == 1) {
        /* Copying a mono streams to two channels */
        int32_t *freq2;
        denormalise_bands(X, freq, oldBandE,effEnd, M, silence);
        /* Store a temporary copy in the output buffer because the IMDCT destroys its input. */
        freq2 = out_syn[1] + overlap / 2;
        memcpy(freq2, freq, N * sizeof(*freq2));
        for(b = 0; b < B; b++) clt_mdct_backward(&freq2[b], out_syn[0] + NB * b, overlap, shift, B);
        for(b = 0; b < B; b++) clt_mdct_backward(&freq[b], out_syn[1] + NB * b, overlap, shift, B);
    } else if(CC == 1 && C == 2) {
        /* Downmixing a stereo stream to mono */
        int32_t *freq2;
        freq2 = out_syn[0] + overlap / 2;
        denormalise_bands(X, freq, oldBandE, effEnd, M, silence);
        /* Use the output buffer as temp array before downmixing. */
        denormalise_bands(X + N, freq2, oldBandE + nbEBands, effEnd, M, silence);
        for(i = 0; i < N; i++) freq[i] = (int32_t)HALF32(freq[i]) + (int32_t)HALF32(freq2[i]);
        for(b = 0; b < B; b++) clt_mdct_backward(&freq[b], out_syn[0] + NB * b, overlap, shift, B);
    } else {
        /* Normal case (mono or stereo) */
        c = 0;
        do {
            denormalise_bands(X + c * N, freq, oldBandE + c * nbEBands, effEnd, M, silence);
            for(b = 0; b < B; b++) clt_mdct_backward(&freq[b], out_syn[c] + NB * b, overlap, shift, B);
        } while(++c < CC);
    }
    /* Saturate IMDCT output so that we can't overflow in the pitch postfilter
       or in the */
    c = 0;
    do {
        for(i = 0; i < N; i++) out_syn[c][i] = SATURATE(out_syn[c][i], (300000000));
    } while(++c < CC);

    return;
}
//----------------------------------------------------------------------------------------------------------------------

void tf_decode(int32_t isTransient, int32_t *tf_res, int32_t LM){
    int32_t i, curr, tf_select;
    int32_t tf_select_rsv;
    int32_t tf_changed;
    int32_t logp;
    uint32_t budget;
    uint32_t tell;
    const uint8_t end = cdec->end;

    budget = s_ec.storage * 8;
    tell = ec_tell();
    logp = isTransient ? 2 : 4;
    tf_select_rsv = LM > 0 && tell + logp + 1 <= budget;
    budget -= tf_select_rsv;
    tf_changed = curr = 0;
    for (i = 0; i < end; i++) {
        if (tell + logp <= budget) {
            curr ^= ec_dec_bit_logp(logp);
            tell = ec_tell();
            tf_changed |= curr;
        }
        tf_res[i] = curr;
        logp = isTransient ? 4 : 5;
    }
    tf_select = 0;
    if (tf_select_rsv &&
        tf_select_table[LM][4 * isTransient + 0 + tf_changed] !=
            tf_select_table[LM][4 * isTransient + 2 + tf_changed]) {
        tf_select = ec_dec_bit_logp(1);
    }
    for (i = 0; i < end; i++) {
        tf_res[i] = tf_select_table[LM][4 * isTransient + 2 * tf_select + tf_res[i]];
    }
}
//----------------------------------------------------------------------------------------------------------------------

int32_t celt_decode_with_ec(const uint8_t *inbuf, int32_t len, int16_t *outbuf, int32_t frame_size) {

    int32_t  c, i, N;
    int32_t  spread_decision;
    int32_t  bits;
    int32_t *decode_mem[2];
    int32_t *out_syn[2];
    int16_t *lpc;
    int16_t *oldBandE, *oldLogE, *oldLogE2, *backgroundLogE;

    int32_t        shortBlocks;
    int32_t        isTransient;
    int32_t        intra_ener;
    const uint8_t  CC = cdec->channels;
    int32_t        LM, M;
    const uint8_t  end = cdec->end;  // 21
    int32_t        codedBands;
    int32_t        alloc_trim;
    int32_t        postfilter_pitch;
    int16_t        postfilter_gain;
    int32_t        intensity = 0;
    int32_t        dual_stereo = 0;
    int32_t        total_bits;
    int32_t        balance;
    int32_t        tell;
    int32_t        dynalloc_logp;
    int32_t        postfilter_tapset;
    int32_t        anti_collapse_rsv;
    int32_t        anti_collapse_on = 0;
    int32_t        silence;
    const uint8_t  C = cdec->stream_channels; // =channels=2
    const uint8_t  nbEBands = m_CELTMode.nbEBands; // =21
    const uint8_t  overlap = m_CELTMode.overlap; // =120
    const int16_t *eBands = eband5ms;

    lpc = (int16_t *)(cdec->_decode_mem + (DECODE_BUFFER_SIZE + overlap) * CC);
    oldBandE = lpc + CC * 24;
    oldLogE = oldBandE + 2 * nbEBands;
    oldLogE2 = oldLogE + 2 * nbEBands;
    backgroundLogE = oldLogE2 + 2 * nbEBands;

    {
        for(LM = 0; LM <= m_CELTMode.maxLM; LM++)                     // m_CELTMode.maxLM == 3
            if(m_CELTMode.shortMdctSize << LM == frame_size) break;   // frame_size == 960
        if(LM > m_CELTMode.maxLM) return OPUS_BAD_ARG;
    }

    M = 1 << LM; // LM=3 -> M = 8

    if(len < 0 || len > 1275 || outbuf == NULL) return OPUS_BAD_ARG;

    N = M * m_CELTMode.shortMdctSize; // const m_CELTMode.shortMdctSize == 120, M == 8 -> N = 960

    c = 0;
    do {
        decode_mem[c] = cdec->_decode_mem + c * (DECODE_BUFFER_SIZE + overlap);
        out_syn[c] = decode_mem[c] + DECODE_BUFFER_SIZE - N;
    } while(++c < CC);

    if(len <= 1) { return OPUS_BAD_ARG; }

    if(C == 1) {
        for(i = 0; i < nbEBands; i++) oldBandE[i] = max(oldBandE[i], oldBandE[nbEBands + i]);
    }

    total_bits = len * 8;
    tell = ec_tell();

    if(tell >= total_bits) silence = 1;
    else if(tell == 1)
        silence = ec_dec_bit_logp(15);
    else
        silence = 0;
    if(silence) {
        /* Pretend we've read all the remaining bits */
        tell = len * 8;
        s_ec.nbits_total += tell - ec_tell();
    }

    postfilter_gain = 0;
    postfilter_pitch = 0;
    postfilter_tapset = 0;
    if(tell + 16 <= total_bits) {
        if(ec_dec_bit_logp(1)) {
            int32_t qg, octave;
            octave = ec_dec_uint(6);
            postfilter_pitch = (16 << octave) + ec_dec_bits(4 + octave) - 1;
            qg = ec_dec_bits(3);
            if(ec_tell() + 2 <= total_bits) postfilter_tapset = ec_dec_icdf(tapset_icdf, 2);
            postfilter_gain = QCONST16(.09375f, 15) * (qg + 1);
        }
        tell = ec_tell();
    }

    if(LM > 0 && tell + 3 <= total_bits) {
        isTransient = ec_dec_bit_logp(3);
        tell = ec_tell();
    } else
        isTransient = 0;

    if(isTransient) shortBlocks = M;
    else
        shortBlocks = 0;

    /* Decode the global flags (first symbols in the stream) */
    intra_ener = tell + 3 <= total_bits ? ec_dec_bit_logp(3) : 0;
    /* Get band energies */
    unquant_coarse_energy(oldBandE, intra_ener, C, LM);

    int32_t tf_res[nbEBands];
    tf_decode(isTransient, tf_res, LM);

    tell = ec_tell();
    spread_decision = 2;  // SPREAD_NORMAL
    if(tell + 4 <= total_bits) spread_decision = ec_dec_icdf(spread_icdf, 5);

    int32_t cap[nbEBands];
    init_caps(cap, LM, C);

    int32_t offsets[nbEBands];
    dynalloc_logp = 6;
    total_bits <<= BITRES;
    tell = ec_tell_frac();
    for(i = 0; i < end; i++) {
        int32_t width, quanta;
        int32_t dynalloc_loop_logp;
        int32_t boost;
        width = C * (eBands[i + 1] - eBands[i]) << LM;
        /* quanta is 6 bits, but no more than 1 bit/sample
           and no less than 1/8 bit/sample */
        quanta = min(width << BITRES, max(6 << BITRES, width));
        dynalloc_loop_logp = dynalloc_logp;
        boost = 0;
        while(tell + (dynalloc_loop_logp << BITRES) < total_bits && boost < cap[i]) {
            int32_t flag;
            flag = ec_dec_bit_logp(dynalloc_loop_logp);
            tell = ec_tell_frac();
            if(!flag) break;
            boost += quanta;
            total_bits -= quanta;
            dynalloc_loop_logp = 1;
        }
        offsets[i] = boost;
        /* Making dynalloc more likely */
        if(boost > 0) dynalloc_logp = max(2, dynalloc_logp - 1);
    }

    int32_t fine_quant[nbEBands];
    alloc_trim = tell + (6 << BITRES) <= total_bits ? ec_dec_icdf(trim_icdf, 7) : 5;

    bits = (((int32_t)len * 8) << BITRES) - ec_tell_frac() - 1;
    anti_collapse_rsv = isTransient && LM >= 2 && bits >= ((LM + 2) << BITRES) ? (1 << BITRES) : 0;
    bits -= anti_collapse_rsv;

    int32_t pulses[nbEBands];
    int32_t fine_priority[nbEBands];

    codedBands = clt_compute_allocation(offsets, cap, alloc_trim, &intensity, &dual_stereo, bits, &balance,
                                        pulses, fine_quant, fine_priority, C, LM);

    unquant_fine_energy(oldBandE, fine_quant, C);

    c = 0;
    do { OPUS_MOVE(decode_mem[c], decode_mem[c] + N, DECODE_BUFFER_SIZE - N + overlap / 2); } while(++c < CC);

    /* Decode fixed codebook */
    assert(C * nbEBands <= 42);
    uint8_t* collapse_masks = s_collapse_masksBuff;

    assert(C * N <= 1920);
    int16_t* X = s_XBuff;

    quant_all_bands(X, C == 2 ? X + N : NULL, collapse_masks, pulses, shortBlocks, spread_decision,
                    dual_stereo, intensity, tf_res, len * (8 << BITRES) - anti_collapse_rsv, balance, LM, codedBands);

    if(anti_collapse_rsv > 0) { anti_collapse_on = ec_dec_bits(1); }

    unquant_energy_finalise(oldBandE, fine_quant, fine_priority, len * 8 - ec_tell(), C);

    if(anti_collapse_on) anti_collapse(X, collapse_masks, LM, C, N, oldBandE, oldLogE, oldLogE2, pulses, cdec->rng);

    if(silence) {
        for(i = 0; i < C * nbEBands; i++) oldBandE[i] = -QCONST16(28.f, 10);
    }

    celt_synthesis(X, out_syn, oldBandE, C, isTransient, LM, silence);

    c = 0;
    const uint8_t COMBFILTER_MINPERIOD = 15;
    do {
        cdec->postfilter_period = max(cdec->postfilter_period, COMBFILTER_MINPERIOD);
        cdec->postfilter_period_old = max(cdec->postfilter_period_old, COMBFILTER_MINPERIOD);
        comb_filter(out_syn[c], out_syn[c], cdec->postfilter_period_old, cdec->postfilter_period,
                    m_CELTMode.shortMdctSize, cdec->postfilter_gain_old, cdec->postfilter_gain,
                    cdec->postfilter_tapset_old, cdec->postfilter_tapset);
        if(LM != 0)
            comb_filter(out_syn[c] + m_CELTMode.shortMdctSize, out_syn[c] + m_CELTMode.shortMdctSize,
                        cdec->postfilter_period, postfilter_pitch, N - m_CELTMode.shortMdctSize, cdec->postfilter_gain,
                        postfilter_gain, cdec->postfilter_tapset, postfilter_tapset);

    } while(++c < CC);
    cdec->postfilter_period_old = cdec->postfilter_period;
    cdec->postfilter_gain_old = cdec->postfilter_gain;
    cdec->postfilter_tapset_old = cdec->postfilter_tapset;
    cdec->postfilter_period = postfilter_pitch;
    cdec->postfilter_gain = postfilter_gain;
    cdec->postfilter_tapset = postfilter_tapset;
    if(LM != 0) {
        cdec->postfilter_period_old = cdec->postfilter_period;
        cdec->postfilter_gain_old = cdec->postfilter_gain;
        cdec->postfilter_tapset_old = cdec->postfilter_tapset;
    }

    if(C == 1) memcpy(&oldBandE[nbEBands], oldBandE, nbEBands * sizeof(*oldBandE));

    /* In case start or end were to change */
    if(!isTransient) {
        int16_t max_background_increase;
        memcpy(oldLogE2, oldLogE, 2 * nbEBands * sizeof(*oldLogE2));
        memcpy(oldLogE, oldBandE, 2 * nbEBands * sizeof(*oldLogE));
        /* In normal circumstances, we only allow the noise floor to increase by
           up to 2.4 dB/second, but when we're in DTX, we allow up to 6 dB
           increase for each update.*/
        max_background_increase = M * QCONST16(0.001f, 10);

        for(i = 0; i < 2 * nbEBands; i++)
            backgroundLogE[i] = min(backgroundLogE[i] + max_background_increase, oldBandE[i]);
    } else {
        for(i = 0; i < 2 * nbEBands; i++) oldLogE[i] = min(oldLogE[i], oldBandE[i]);
    }
    c = 0;
    do {
        for(i = end; i < nbEBands; i++) {
            oldBandE[c * nbEBands + i] = 0;
            oldLogE[c * nbEBands + i] = oldLogE2[c * nbEBands + i] = -QCONST16(28.f, 10);
        }
    } while(++c < 2);
    cdec->rng = 0; //dec->rng;

    deemphasis(out_syn, outbuf, N);

    if(ec_tell() > 8 * len) return OPUS_INTERNAL_ERROR;
    if(s_ec.error) cdec->error = 1;

    return frame_size;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t celt_decoder_ctl(int32_t request, ...) {
    va_list ap;

    va_start(ap, request);
    switch (request) {
        case CELT_SET_END_BAND_REQUEST: {
            int32_t value = va_arg(ap, int32_t);
            if (value < 1 || value > cdec->mode->nbEBands) goto bad_arg;
            cdec->end = value;
        } break;
        case CELT_SET_CHANNELS_REQUEST: {
            int32_t value = va_arg(ap, int32_t);
            if (value < 1 || value > 2) goto bad_arg;
            cdec->stream_channels = value;
        } break;
        case CELT_GET_AND_CLEAR_ERROR_REQUEST: {
            int32_t *value = va_arg(ap, int32_t *);
            if (value == NULL) goto bad_arg;
            *value = cdec->error;
            cdec->error = 0;
        } break;
        case OPUS_RESET_STATE: {
            int32_t i;
            int16_t *lpc, *oldBandE, *oldLogE, *oldLogE2;
            lpc = (int16_t *)(cdec->_decode_mem + (DECODE_BUFFER_SIZE + cdec->overlap) * cdec->channels);
            oldBandE = lpc + cdec->channels * 24;
            oldLogE = oldBandE + 2 * cdec->mode->nbEBands;
            oldLogE2 = oldLogE + 2 * cdec->mode->nbEBands;

            int n = celt_decoder_get_size(cdec->channels);
            char* dest   = (char*)&cdec->rng;
            char* offset = (char*)cdec;
            memset(dest, 0,  n - (dest - offset) * sizeof(cdec));

            for (i = 0; i < 2 * cdec->mode->nbEBands; i++) oldLogE[i] = oldLogE2[i] = -QCONST16(28.f, 10);
        } break;
        case CELT_GET_MODE_REQUEST: {
            const CELTMode **value = va_arg(ap, const CELTMode **);
            if (value == 0) goto bad_arg;
            *value = cdec->mode;
        } break;
        case CELT_SET_SIGNALLING_REQUEST: {
            int32_t value = va_arg(ap, int32_t);
            cdec->signalling = value;
        } break;
        default:
            goto bad_request;
    }
    va_end(ap);
    return OPUS_OK;
bad_arg:
    va_end(ap);
    return OPUS_BAD_ARG;
bad_request:
    va_end(ap);
    return OPUS_UNIMPLEMENTED;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t cwrsi(int32_t _n, int32_t _k, uint32_t _i, int32_t *_y) {
    uint32_t p;
    int32_t s;
    int32_t k0;
    int16_t val;
    int32_t yy = 0;
    assert(_k > 0);
    assert(_n > 1);
    while (_n > 2) {
        uint32_t q;
        /*Lots of pulses case:*/
        if (_k >= _n) {
            const uint32_t *row;
            //row = celt_pvq_u_row[_n];
            row = &CELT_PVQ_U_DATA[row_idx[_n]];

            /*Are the pulses in this dimension negative?*/
            p = row[_k + 1];
            s = -(_i >= p);
            _i -= p & s;
            /*Count how many pulses were placed in this dimension.*/
            k0 = _k;
            q = row[_n];
            if (q > _i) {
                assert(p > q);
                _k = _n;
                do p = celt_pvq_u_row(--_k, _n);
                while (p > _i);
            } else
                for (p = row[_k]; p > _i; p = row[_k]) _k--;
            _i -= p;
            val = (k0 - _k + s) ^ s;
            *_y++ = val;
            yy = MAC16_16(yy, val, val);
        }
        /*Lots of dimensions case:*/
        else {
            /*Are there any pulses in this dimension at all?*/
            p = celt_pvq_u_row(_k, _n);
            q = celt_pvq_u_row(_k + 1, _n);
            if (p <= _i && _i < q) {
                _i -= p;
                *_y++ = 0;
            } else {
                /*Are the pulses in this dimension negative?*/
                s = -(_i >= q);
                _i -= q & s;
                /*Count how many pulses were placed in this dimension.*/
                k0 = _k;
                do p = celt_pvq_u_row(--_k, _n);
                while (p > _i);
                _i -= p;
                val = (k0 - _k + s) ^ s;
                *_y++ = val;
                yy = MAC16_16(yy, val, val);
            }
        }
        _n--;
    }
    /*_n==2*/
    p = 2 * _k + 1;
    s = -(_i >= p);
    _i -= p & s;
    k0 = _k;
    _k = (_i + 1) >> 1;
    if (_k) _i -= 2 * _k - 1;
    val = (k0 - _k + s) ^ s;
    *_y++ = val;
    yy = MAC16_16(yy, val, val);
    /*_n==1*/
    s = -(int32_t)_i;
    val = (_k + s) ^ s;
    *_y = val;
    yy = MAC16_16(yy, val, val);
    return yy;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t decode_pulses(int32_t *_y, int32_t _n, int32_t _k) {
    return cwrsi(_n, _k, ec_dec_uint(CELT_PVQ_V(_n, _k)), _y);
}
//----------------------------------------------------------------------------------------------------------------------

/* This is a faster version of ec_tell_frac() that takes advantage of the low (1/8 bit) resolution to use just a linear
   function followed by a lookup to determine the exact transition thresholds. */
uint32_t ec_tell_frac() {
    static const uint32_t correction[8] = {35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
    uint32_t nbits;
    uint32_t r;
    int32_t l;
    uint32_t b;
    nbits = s_ec.nbits_total << BITRES;
    l = EC_ILOG(s_ec.rng);
    r = s_ec.rng >> (l - 16);
    b = (r >> 12) - 8;
    b += r > correction[b];
    l = (l << 3) + b;
    return nbits - l;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t ec_read_byte() { return s_ec.offs < s_ec.storage ? s_ec.buf[s_ec.offs++] : 0; }

//----------------------------------------------------------------------------------------------------------------------

int32_t ec_read_byte_from_end() {
    return s_ec.end_offs < s_ec.storage ? s_ec.buf[s_ec.storage - ++(s_ec.end_offs)] : 0;
}
//----------------------------------------------------------------------------------------------------------------------

/*Normalizes the contents of val and rng so that rng lies entirely in the high-order symbol.*/
void ec_dec_normalize() {
    /*If the range is too small, rescale it and input some bits.*/
    while (s_ec.rng <= EC_CODE_BOT) {
        int32_t sym;
        s_ec.nbits_total += EC_SYM_BITS;
        s_ec.rng <<= EC_SYM_BITS;
        /*Use up the remaining bits from our last symbol.*/
        sym = s_ec.rem;
        /*Read the next value from the input.*/
        s_ec.rem = ec_read_byte();
        /*Take the rest of the bits we need from this new symbol.*/
        sym = (sym << EC_SYM_BITS | s_ec.rem) >> (EC_SYM_BITS - EC_CODE_EXTRA);
        /*And subtract them from val, capped to be less than EC_CODE_TOP.*/
        s_ec.val = ((s_ec.val << EC_SYM_BITS) + (EC_SYM_MAX & ~sym)) & (EC_CODE_TOP - 1);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void ec_dec_init(uint8_t *_buf, uint32_t _storage) {
    s_ec.buf = _buf;
    s_ec.storage = _storage;
    s_ec.end_offs = 0;
    s_ec.end_window = 0;
    s_ec.nend_bits = 0;
    s_ec.nbits_total = EC_CODE_BITS + 1 - ((EC_CODE_BITS - EC_CODE_EXTRA) / EC_SYM_BITS) * EC_SYM_BITS;
    s_ec.offs = 0;
    s_ec.rng = 1U << EC_CODE_EXTRA;
    s_ec.rem = ec_read_byte();
    s_ec.val = s_ec.rng - 1 - (s_ec.rem >> (EC_SYM_BITS - EC_CODE_EXTRA));
    s_ec.error = 0;
    /*Normalize the interval.*/
    ec_dec_normalize();
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t ec_decode(uint32_t _ft) {
    uint32_t s;
    assert(_ft > 0);
    s_ec.ext = s_ec.rng / _ft;
    s = (uint32_t)(s_ec.val / s_ec.ext);
    return _ft - EC_MINI(s + 1, _ft);
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t ec_decode_bin(uint32_t _bits) {
    uint32_t s;
    s_ec.ext = s_ec.rng >> _bits;
    s = (uint32_t)(s_ec.val / s_ec.ext);
    return (1U << _bits) - EC_MINI(s + 1U, 1U << _bits);
}
//----------------------------------------------------------------------------------------------------------------------

void ec_dec_update(uint32_t _fl, uint32_t _fh, uint32_t _ft) {
    uint32_t s;
    s = s_ec.ext *  (_ft - _fh);
    s_ec.val -= s;

    if(_fl > 0){
        s_ec.rng = s_ec.ext * (_fh - _fl);
    }
    else{
        s_ec.rng = s_ec.rng - s;
    }
    ec_dec_normalize();
}
//----------------------------------------------------------------------------------------------------------------------

/*The probability of having a "one" is 1/(1<<_logp).*/
int32_t ec_dec_bit_logp(uint32_t _logp) {
    uint32_t r;
    uint32_t d;
    uint32_t s;
    int32_t ret;
    r = s_ec.rng;
    d = s_ec.val;
    s = r >> _logp;
    ret = d < s;
    if (!ret) s_ec.val = d - s;
    s_ec.rng = ret ? s : r - s;
    ec_dec_normalize();
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t ec_dec_icdf(const uint8_t *_icdf, uint32_t _ftb) {
    uint32_t r;
    uint32_t d;
    uint32_t s;
    uint32_t t;
    int32_t ret;
    s = s_ec.rng;
    d = s_ec.val;
    r = s >> _ftb;
    ret = -1;
    do {
        t = s;
        s = r * _icdf[++ret];
    } while (d < s);
    s_ec.val = d - s;
    s_ec.rng = t - s;
    ec_dec_normalize();
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t ec_dec_uint(uint32_t _ft) {
    uint32_t ft;
    uint32_t s;
    int32_t ftb;
    /*In order to optimize EC_ILOG(), it is undefined for the value 0.*/
    assert(_ft > 1);
    _ft--;
    ftb = EC_ILOG(_ft);
    if (ftb > EC_UINT_BITS) {
        uint32_t t;
        ftb -= EC_UINT_BITS;
        ft = (uint32_t)(_ft >> ftb) + 1;
        s = ec_decode(ft);
        ec_dec_update(s, s + 1, ft);
        t = (uint32_t)s << ftb | ec_dec_bits(ftb);
        if (t <= _ft) return t;
        s_ec.error = 1;
        return _ft;
    } else {
        _ft++;
        s = ec_decode((uint32_t)_ft);
        ec_dec_update(s, s + 1, (uint32_t)_ft);
        return s;
    }
}
//----------------------------------------------------------------------------------------------------------------------

uint32_t ec_dec_bits(uint32_t _bits) {
    uint32_t window;
    int32_t available;
    uint32_t ret;
    window = s_ec.end_window;
    available = s_ec.nend_bits;
    if ((uint32_t)available < _bits) {
        do {
            window |= (uint32_t)ec_read_byte_from_end() << available;
            available += EC_SYM_BITS;
        } while (available <= EC_WINDOW_SIZE - EC_SYM_BITS);
    }
    ret = (uint32_t)window & (((uint32_t)1 << _bits) - 1U);
    window >>= _bits;
    available -= _bits;
    s_ec.end_window = window;
    s_ec.nend_bits = available;
    s_ec.nbits_total += _bits;
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------

void kf_bfly2(kiss_fft_cpx *Fout, int32_t m, int32_t N) {
    kiss_fft_cpx *Fout2;
    int32_t i;
    (void)m;

    {
        int16_t tw;
        tw = QCONST16(0.7071067812f, 15);
        /* We know that m==4 here because the radix-2 is just after a radix-4 */
        assert(m == 4);
        for (i = 0; i < N; i++) {
            kiss_fft_cpx t;
            Fout2 = Fout + 4;
            t = Fout2[0];
            C_SUB(Fout2[0], Fout[0], t);
            C_ADDTO(Fout[0], t);

            t.r = S_MUL(ADD32_ovflw(Fout2[1].r, Fout2[1].i), tw);
            t.i = S_MUL(SUB32_ovflw(Fout2[1].i, Fout2[1].r), tw);
            C_SUB(Fout2[1], Fout[1], t);
            C_ADDTO(Fout[1], t);

            t.r = Fout2[2].i;
            t.i = -Fout2[2].r;
            C_SUB(Fout2[2], Fout[2], t);
            C_ADDTO(Fout[2], t);

            t.r = S_MUL(SUB32_ovflw(Fout2[3].i, Fout2[3].r), tw);
            t.i = S_MUL(NEG32_ovflw(ADD32_ovflw(Fout2[3].i, Fout2[3].r)), tw);
            C_SUB(Fout2[3], Fout[3], t);
            C_ADDTO(Fout[3], t);
            Fout += 8;
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------

void kf_bfly4(kiss_fft_cpx *Fout, const size_t fstride, const kiss_fft_state *st, int32_t m, int32_t N, int32_t mm) {
    int32_t i;

    if (m == 1) {
        /* Degenerate case where all the twiddles are 1. */
        for (i = 0; i < N; i++) {
            kiss_fft_cpx scratch0, scratch1;

            C_SUB(scratch0, *Fout, Fout[2]);
            C_ADDTO(*Fout, Fout[2]);
            C_ADD(scratch1, Fout[1], Fout[3]);
            C_SUB(Fout[2], *Fout, scratch1);
            C_ADDTO(*Fout, scratch1);
            C_SUB(scratch1, Fout[1], Fout[3]);

            Fout[1].r = ADD32_ovflw(scratch0.r, scratch1.i);
            Fout[1].i = SUB32_ovflw(scratch0.i, scratch1.r);
            Fout[3].r = SUB32_ovflw(scratch0.r, scratch1.i);
            Fout[3].i = ADD32_ovflw(scratch0.i, scratch1.r);
            Fout += 4;
        }
    } else {
        int32_t j;
        kiss_fft_cpx scratch[6];
        const kiss_twiddle_cpx *tw1, *tw2, *tw3;
        const int32_t m2 = 2 * m;
        const int32_t m3 = 3 * m;
        kiss_fft_cpx *Fout_beg = Fout;
        for (i = 0; i < N; i++) {
            Fout = Fout_beg + i * mm;
            tw3 = tw2 = tw1 = st->twiddles;
            /* m is guaranteed to be a multiple of 4. */
            for (j = 0; j < m; j++) {
                C_MUL(scratch[0], Fout[m], *tw1);
                C_MUL(scratch[1], Fout[m2], *tw2);
                C_MUL(scratch[2], Fout[m3], *tw3);

                C_SUB(scratch[5], *Fout, scratch[1]);
                C_ADDTO(*Fout, scratch[1]);
                C_ADD(scratch[3], scratch[0], scratch[2]);
                C_SUB(scratch[4], scratch[0], scratch[2]);
                C_SUB(Fout[m2], *Fout, scratch[3]);
                tw1 += fstride;
                tw2 += fstride * 2;
                tw3 += fstride * 3;
                C_ADDTO(*Fout, scratch[3]);

                Fout[m].r = ADD32_ovflw(scratch[5].r, scratch[4].i);
                Fout[m].i = SUB32_ovflw(scratch[5].i, scratch[4].r);
                Fout[m3].r = SUB32_ovflw(scratch[5].r, scratch[4].i);
                Fout[m3].i = ADD32_ovflw(scratch[5].i, scratch[4].r);
                ++Fout;
            }
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------

void kf_bfly3(kiss_fft_cpx *Fout, const size_t fstride, const kiss_fft_state *st, int32_t m, int32_t N, int32_t mm) {
    int32_t i;
    size_t k;
    const size_t m2 = 2 * m;
    const kiss_twiddle_cpx *tw1, *tw2;
    kiss_fft_cpx scratch[5];
    kiss_twiddle_cpx epi3;

    kiss_fft_cpx *Fout_beg = Fout;
    /*epi3.r = -16384;*/ /* Unused */
    epi3.i = -28378;
    for (i = 0; i < N; i++) {
        Fout = Fout_beg + i * mm;
        tw1 = tw2 = st->twiddles;
        /* For non-custom modes, m is guaranteed to be a multiple of 4. */
        k = m;
        do {
            C_MUL(scratch[1], Fout[m], *tw1);
            C_MUL(scratch[2], Fout[m2], *tw2);

            C_ADD(scratch[3], scratch[1], scratch[2]);
            C_SUB(scratch[0], scratch[1], scratch[2]);
            tw1 += fstride;
            tw2 += fstride * 2;

            Fout[m].r = SUB32_ovflw(Fout->r, scratch[3].r >> 1);
            Fout[m].i = SUB32_ovflw(Fout->i, scratch[3].i >> 1);

            C_MULBYSCALAR(scratch[0], epi3.i);

            C_ADDTO(*Fout, scratch[3]);

            Fout[m2].r = ADD32_ovflw(Fout[m].r, scratch[0].i);
            Fout[m2].i = SUB32_ovflw(Fout[m].i, scratch[0].r);

            Fout[m].r = SUB32_ovflw(Fout[m].r, scratch[0].i);
            Fout[m].i = ADD32_ovflw(Fout[m].i, scratch[0].r);

            ++Fout;
        } while (--k);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void kf_bfly5(kiss_fft_cpx *Fout, const size_t fstride, const kiss_fft_state *st, int32_t m, int32_t N, int32_t mm) {
    kiss_fft_cpx *Fout0, *Fout1, *Fout2, *Fout3, *Fout4;
    int32_t i, u;
    kiss_fft_cpx scratch[13];
    const kiss_twiddle_cpx *tw;
    kiss_twiddle_cpx ya, yb;
    kiss_fft_cpx *Fout_beg = Fout;

    ya.r = 10126;
    ya.i = -31164;
    yb.r = -26510;
    yb.i = -19261;
    tw = st->twiddles;

    for (i = 0; i < N; i++) {
        Fout = Fout_beg + i * mm;
        Fout0 = Fout;
        Fout1 = Fout0 + m;
        Fout2 = Fout0 + 2 * m;
        Fout3 = Fout0 + 3 * m;
        Fout4 = Fout0 + 4 * m;

        /* For non-custom modes, m is guaranteed to be a multiple of 4. */
        for (u = 0; u < m; ++u) {
            scratch[0] = *Fout0;

            C_MUL(scratch[1], *Fout1, tw[u * fstride]);
            C_MUL(scratch[2], *Fout2, tw[2 * u * fstride]);
            C_MUL(scratch[3], *Fout3, tw[3 * u * fstride]);
            C_MUL(scratch[4], *Fout4, tw[4 * u * fstride]);

            C_ADD(scratch[7], scratch[1], scratch[4]);
            C_SUB(scratch[10], scratch[1], scratch[4]);
            C_ADD(scratch[8], scratch[2], scratch[3]);
            C_SUB(scratch[9], scratch[2], scratch[3]);

            Fout0->r = ADD32_ovflw(Fout0->r, ADD32_ovflw(scratch[7].r, scratch[8].r));
            Fout0->i = ADD32_ovflw(Fout0->i, ADD32_ovflw(scratch[7].i, scratch[8].i));

            scratch[5].r = ADD32_ovflw(scratch[0].r, ADD32_ovflw(S_MUL(scratch[7].r, ya.r), S_MUL(scratch[8].r, yb.r)));
            scratch[5].i = ADD32_ovflw(scratch[0].i, ADD32_ovflw(S_MUL(scratch[7].i, ya.r), S_MUL(scratch[8].i, yb.r)));

            scratch[6].r = ADD32_ovflw(S_MUL(scratch[10].i, ya.i), S_MUL(scratch[9].i, yb.i));
            scratch[6].i = NEG32_ovflw(ADD32_ovflw(S_MUL(scratch[10].r, ya.i), S_MUL(scratch[9].r, yb.i)));

            C_SUB(*Fout1, scratch[5], scratch[6]);
            C_ADD(*Fout4, scratch[5], scratch[6]);

            scratch[11].r =
                ADD32_ovflw(scratch[0].r, ADD32_ovflw(S_MUL(scratch[7].r, yb.r), S_MUL(scratch[8].r, ya.r)));
            scratch[11].i =
                ADD32_ovflw(scratch[0].i, ADD32_ovflw(S_MUL(scratch[7].i, yb.r), S_MUL(scratch[8].i, ya.r)));
            scratch[12].r = SUB32_ovflw(S_MUL(scratch[9].i, ya.i), S_MUL(scratch[10].i, yb.i));
            scratch[12].i = SUB32_ovflw(S_MUL(scratch[10].r, yb.i), S_MUL(scratch[9].r, ya.i));

            C_ADD(*Fout2, scratch[11], scratch[12]);
            C_SUB(*Fout3, scratch[11], scratch[12]);

            ++Fout0;
            ++Fout1;
            ++Fout2;
            ++Fout3;
            ++Fout4;
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------

void opus_fft_impl(const kiss_fft_state *st, kiss_fft_cpx *fout) {
    int32_t m2, m;
    int32_t p;
    int32_t L;
    int32_t fstride[MAXFACTORS];
    int32_t i;
    int32_t shift;

    /* st->shift can be -1 */
    shift = st->shift > 0 ? st->shift : 0;

    fstride[0] = 1;
    L = 0;
    do {
        p = st->factors[2 * L];
        m = st->factors[2 * L + 1];
        fstride[L + 1] = fstride[L] * p;
        L++;
    } while (m != 1);
    m = st->factors[2 * L - 1];
    for (i = L - 1; i >= 0; i--) {
        if (i != 0)
            m2 = st->factors[2 * i - 1];
        else
            m2 = 1;
        switch (st->factors[2 * i]) {
            case 2:
                kf_bfly2(fout, m, fstride[i]);
                break;
            case 4:
                kf_bfly4(fout, fstride[i] << shift, st, m, fstride[i], m2);
                break;
            case 3:
                kf_bfly3(fout, fstride[i] << shift, st, m, fstride[i], m2);
                break;
            case 5:
                kf_bfly5(fout, fstride[i] << shift, st, m, fstride[i], m2);
                break;
        }
        m = m2;
    }
}
//----------------------------------------------------------------------------------------------------------------------

/* When called, decay is positive and at most 11456. */
uint32_t ec_laplace_get_freq1(uint32_t fs0, int32_t decay) {
    uint32_t ft;
    ft = 32768 - (2 * 16) - fs0;
    return ft * (int32_t)(16384 - decay) >> 15;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t ec_laplace_decode(uint32_t fs, int32_t decay) {
    int32_t val = 0;
    uint32_t fl;
    uint32_t fm;
    fm = ec_decode_bin(15);
    fl = 0;
    if (fm >= fs) {
        val++;
        fl = fs;
        fs = ec_laplace_get_freq1(fs, decay) + 1;
        /* Search the decaying part of the PDF.*/
        while (fs > 1 && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = ((fs - 2) * (int32_t)decay) >> 15;
            fs += 1;
            val++;
        }
        /* Everything beyond that has probability 1. */
        if (fs <= 1) {
            int32_t di;
            di = (fm - fl) >> (1);
            val += di;
            fl += 2 * di;
        }
        if (fm < fl + fs)
            val = -val;
        else
            fl += fs;
    }
    assert(fl < 32768);
    assert(fs > 0);
    assert(fl <= fm);
    assert(fm < min(fl + fs, 32768));
    ec_dec_update(fl, min(fl + fs, 32768), 32768);
    return val;
}
//----------------------------------------------------------------------------------------------------------------------

/*Compute floor(sqrt(_val)) with exact arithmetic. _val must be greater than 0. This has been tested on all
 possible 32-bit inputs greater than 0.*/
uint32_t isqrt32(uint32_t _val) {
    uint32_t b;
    uint32_t g;
    int32_t bshift;
    /*Uses the second method from  http://www.azillionmonkeys.com/qed/sqroot.html The main idea is to search for the
     largest binary digit b such that (g+b)*(g+b) <= _val, and add it to the solution g.*/
    g = 0;
    bshift = (EC_ILOG(_val) - 1) >> 1;
    b = 1U << bshift;
    do {
        uint32_t t;
        t = (((uint32_t)g << 1) + b) << bshift;
        if (t <= _val) {
            g += b;
            _val -= t;
        }
        b >>= 1;
        bshift--;
    } while (bshift >= 0);
    return g;
}
//----------------------------------------------------------------------------------------------------------------------

/** Reciprocal sqrt approximation in the range [0.25,1) (Q16 in, Q14 out) */
int16_t celt_rsqrt_norm(int32_t x) {
    int16_t n;
    int16_t r;
    int16_t r2;
    int16_t y;
    /* Range of n is [-16384,32767] ([-0.5,1) in Q15). */
    n = x - 32768;
    /* Get a rough initial guess for the root. The optimal minmax quadratic approximation (using relative error) is
       r = 1.437799046117536+n*(-0.823394375837328+n*0.4096419668459485).  Coefficients here, and the final result r,
       are Q14.*/
    r = ADD16(23557, MULT16_16_Q15(n, ADD16(-13490, MULT16_16_Q15(n, 6713))));
    /* We want y = x*r*r-1 in Q15, but x is 32-bit Q16 and r is Q14. We can compute the result from n and r using Q15
       multiplies with some adjustment, carefully done to avoid overflow. Range of y is [-1564,1594]. */
    r2 = MULT16_16_Q15(r, r);
    y = SHL16(SUB16(ADD16(MULT16_16_Q15(r2, n), r2), 16384), 1);
    /* Apply a 2nd-order Householder iteration: r += r*y*(y*0.375-0.5). This yields the Q14 reciprocal square root of
       the Q16 x, with a maximum relative error of 1.04956E-4, a (relative) RMSE of 2.80979E-5, and a peak absolute
       error of 2.26591/16384. */
    return ADD16(r, MULT16_16_Q15(r, MULT16_16_Q15(y, SUB16(MULT16_16_Q15(y, 12288), 16384))));
}
//----------------------------------------------------------------------------------------------------------------------

/** Sqrt approximation (QX input, QX/2 output) */
int32_t celt_sqrt(int32_t x) {
    int32_t k;
    int16_t n;
    int32_t rt;
    static const int16_t C[5] = {23175, 11561, -3011, 1699, -664};
    if (x == 0)
        return 0;
    else if (x >= 1073741824)
        return 32767;
    if(x < 1) log_e("celt_ilog2 %i", x);
    k = (celt_ilog2(x) >> 1) - 7;
    x = VSHR32(x, 2 * k);
    n = x - 32768;
    rt = ADD16(
        C[0],
        MULT16_16_Q15(
            n, ADD16(C[1], MULT16_16_Q15(n, ADD16(C[2], MULT16_16_Q15(n, ADD16(C[3], MULT16_16_Q15(n, (C[4])))))))));
    rt = VSHR32(rt, 7 - k);
    return rt;
}
//----------------------------------------------------------------------------------------------------------------------

static inline int16_t _celt_cos_pi_2(int16_t x) {
    int16_t x2;

    x2 = MULT16_16_P15(x, x);
    return ADD16(
        1,
        min(32766, ADD32(SUB16(32767, x2),
                           MULT16_16_P15(x2, ADD32(-7651, MULT16_16_P15(x2, ADD32(8277, MULT16_16_P15(-626, x2))))))));
}
//----------------------------------------------------------------------------------------------------------------------

int16_t celt_cos_norm(int32_t x) {
    x = x & 0x0001ffff;
    if (x > SHL32(EXTEND32(1), 16)) x = SUB32(SHL32(EXTEND32(1), 17), x);
    if (x & 0x00007fff) {
        if (x < SHL32(EXTEND32(1), 15)) {
            return _celt_cos_pi_2((int16_t)(x));
        } else {
            return (_celt_cos_pi_2((int16_t)(65536 - x))) * (-1);
        }
    } else {
        if (x & 0x0000ffff)
            return 0;
        else if (x & 0x0001ffff)
            return -32767;
        else
            return 32767;
    }
}
//----------------------------------------------------------------------------------------------------------------------

/** Reciprocal approximation (Q15 input, Q16 output) */
int32_t celt_rcp(int32_t x) {
    int32_t i;
    int16_t n;
    int16_t r;
    assert(x > 0);
    i = celt_ilog2(x);
    /* n is Q15 with range [0,1). */
    n = VSHR32(x, i - 15) - 32768;
    /* Start with a linear approximation:
       r = 1.8823529411764706-0.9411764705882353*n.
       The coefficients and the result are Q14 in the range [15420,30840].*/
    r = ADD16(30840, MULT16_16_Q15(-15420, n));
    /* Perform two Newton iterations:
       r -= r*((r*n)-1.Q15) = r*((r*n)+(r-1.Q15)). */
    r = SUB16(r, MULT16_16_Q15(r, ADD16(MULT16_16_Q15(r, n), ADD16(r, -32768))));
    /* We subtract an extra 1 in the second iteration to avoid overflow; it also
        neatly compensates for truncation error in the rest of the process. */
    r = SUB16(r, ADD16(1, MULT16_16_Q15(r, ADD16(MULT16_16_Q15(r, n), ADD16(r, -32768)))));
    /* r is now the Q15 solution to 2/(n+1), with a maximum relative error
        of 7.05346E-5, a (relative) RMSE of 2.14418E-5, and a peak absolute error of 1.24665/32768. */
    return VSHR32(EXTEND32(r), i - 16);
}
//----------------------------------------------------------------------------------------------------------------------

void clt_mdct_backward(int32_t *in, int32_t * out, int32_t overlap, int32_t shift, int32_t stride) {
    int32_t i;
    int32_t N, N2, N4;
    const int16_t *trig;

    N = m_mdct_lookup.n;
    trig = m_mdct_lookup.trig;
    for (i = 0; i < shift; i++) {
        N >>= 1;
        trig += N;
    }
    N2 = N >> 1;
    N4 = N >> 2;

    /* Pre-rotate */
    {
        /* Temp pointers to make it really clear to the compiler what we're doing */
        const int32_t * xp1 = in;
        const int32_t * xp2 = in + stride * (N2 - 1);
        int32_t * yp = out + (overlap >> 1);
        const int16_t * t = &trig[0];
        const int16_t * bitrev = m_mdct_lookup.kfft[shift]->bitrev;
        for (i = 0; i < N4; i++) {
            int32_t rev;
            int32_t yr, yi;
            rev = *bitrev++;
            yr = ADD32_ovflw(S_MUL(*xp2, t[i]), S_MUL(*xp1, t[N4 + i]));
            yi = SUB32_ovflw(S_MUL(*xp1, t[i]), S_MUL(*xp2, t[N4 + i]));
            /* We swap real and imag because we use an FFT instead of an IFFT. */
            yp[2 * rev + 1] = yr;
            yp[2 * rev] = yi;
            /* Storing the pre-rotation directly in the bitrev order. */
            xp1 += 2 * stride;
            xp2 -= 2 * stride;
        }
    }

    opus_fft_impl(m_mdct_lookup.kfft[shift], (kiss_fft_cpx *)(out + (overlap >> 1)));

    /* Post-rotate and de-shuffle from both ends of the buffer at once to make
       it in-place. */
    {
        int32_t *yp0 = out + (overlap >> 1);
        int32_t *yp1 = out + (overlap >> 1) + N2 - 2;
        const int16_t *t = &trig[0];
        /* Loop to (N4+1)>>1 to handle odd N4. When N4 is odd, the
           middle pair will be computed twice. */
        for (i = 0; i < (N4 + 1) >> 1; i++) {
            int32_t re, im, yr, yi;
            int16_t t0, t1;
            /* We swap real and imag because we're using an FFT instead of an IFFT. */
            re = yp0[1];
            im = yp0[0];
            t0 = t[i];
            t1 = t[N4 + i];
            /* We'd scale up by 2 here, but instead it's done when mixing the windows */
            yr = ADD32_ovflw(S_MUL(re, t0), S_MUL(im, t1));
            yi = SUB32_ovflw(S_MUL(re, t1), S_MUL(im, t0));
            /* We swap real and imag because we're using an FFT instead of an IFFT. */
            re = yp1[1];
            im = yp1[0];
            yp0[0] = yr;
            yp1[1] = yi;

            t0 = t[(N4 - i - 1)];
            t1 = t[(N2 - i - 1)];
            /* We'd scale up by 2 here, but instead it's done when mixing the windows */
            yr = ADD32_ovflw(S_MUL(re, t0), S_MUL(im, t1));
            yi = SUB32_ovflw(S_MUL(re, t1), S_MUL(im, t0));
            yp1[0] = yr;
            yp0[1] = yi;
            yp0 += 2;
            yp1 -= 2;
        }
    }

    /* Mirror on both sides for TDAC */
    {
        int32_t * xp1 = out + overlap - 1;
        int32_t * yp1 = out;
        const int16_t * wp1 = window120;
        const int16_t * wp2 = window120 + overlap - 1;

        for (i = 0; i < overlap / 2; i++) {
            int32_t x1, x2;
            x1 = *xp1;
            x2 = *yp1;
            *yp1++ = SUB32_ovflw(MULT16_32_Q15(*wp2, x2), MULT16_32_Q15(*wp1, x1));
            *xp1-- = ADD32_ovflw(MULT16_32_Q15(*wp1, x2), MULT16_32_Q15(*wp2, x1));
            wp1++;
            wp2--;
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------

int32_t interp_bits2pulses(int32_t end, int32_t skip_start, const int32_t *bits1, const int32_t *bits2,
                           const int32_t *thresh, const int32_t *cap, int32_t total, int32_t *_balance,
                           int32_t skip_rsv, int32_t *intensity, int32_t intensity_rsv, int32_t *dual_stereo,
                           int32_t dual_stereo_rsv, int32_t *bits, int32_t *ebits, int32_t *fine_priority, int32_t C,
                           int32_t LM) {
    int32_t psum;
    int32_t lo, hi;
    int32_t i, j;
    int32_t logM;
    int32_t stereo;
    int32_t codedBands = -1;
    int32_t alloc_floor;
    int32_t left, percoeff;
    int32_t done;
    int32_t balance;

    alloc_floor = C << BITRES;
    stereo = C > 1;

    logM = LM << BITRES;
    lo = 0;
    hi = 1 << ALLOC_STEPS;
    for(i = 0; i < ALLOC_STEPS; i++) {
        int32_t mid = (lo + hi) >> 1;
        psum = 0;
        done = 0;
        for(j = end; j-- > 0;) {
            int32_t tmp = bits1[j] + (mid * (int32_t)bits2[j] >> ALLOC_STEPS);
            if(tmp >= thresh[j] || done) {
                done = 1;
                /* Don't allocate more than we can actually use */
                psum += min(tmp, cap[j]);
            } else {
                if(tmp >= alloc_floor) psum += alloc_floor;
            }
        }
        if(psum > total) hi = mid;
        else
            lo = mid;
    }
    psum = 0;
    /*printf ("interp bisection gave %d\n", lo);*/
    done = 0;
    for(j = end; j-- > 0;) {
        int32_t tmp = bits1[j] + ((int32_t)lo * bits2[j] >> ALLOC_STEPS);
        if(tmp < thresh[j] && !done) {
            if(tmp >= alloc_floor) tmp = alloc_floor;
            else
                tmp = 0;
        } else
            done = 1;
        /* Don't allocate more than we can actually use */
        tmp = min(tmp, cap[j]);
        bits[j] = tmp;
        psum += tmp;
    }

    /* Decide which bands to skip, working backwards from the end. */
    for(codedBands = end;; codedBands--) {
        int32_t band_width;
        int32_t band_bits;
        int32_t rem;
        j = codedBands - 1;
        /* Never skip the first band, nor a band that has been boosted by
            dynalloc.
           In the first case, we'd be coding a bit to signal we're going to waste
            all the other bits.
           In the second case, we'd be coding a bit to redistribute all the bits
            we just signaled should be cocentrated in this band. */
        if(j <= skip_start) {
            /* Give the bit we reserved to end skipping back. */
            total += skip_rsv;
            break;
        }
        /*Figure out how many left-over bits we would be adding to this band.
          This can include bits we've stolen back from higher, skipped bands.*/
        left = total - psum;
        assert(eband5ms[codedBands] - eband5ms[0] > 0);
        percoeff = left / (eband5ms[codedBands] - eband5ms[0]);
        left -= (eband5ms[codedBands] - eband5ms[0]) * percoeff;
        rem = max(left - (eband5ms[j] - eband5ms[0]), 0);
        band_width = eband5ms[codedBands] - eband5ms[j];
        band_bits = (int32_t)(bits[j] + percoeff * band_width + rem);
        /*Only code a skip decision if we're above the threshold for this band.
          Otherwise it is force-skipped.
          This ensures that we have enough bits to code the skip flag.*/
        if(band_bits >= max(thresh[j], alloc_floor + (1 << BITRES))) {
            if(ec_dec_bit_logp(1)) { break; }
            /*We used a bit to skip this band.*/
            psum += 1 << BITRES;
            band_bits -= 1 << BITRES;
        }
        /*Reclaim the bits originally allocated to this band.*/
        psum -= bits[j] + intensity_rsv;
        if(intensity_rsv > 0) intensity_rsv = LOG2_FRAC_TABLE[j];
        psum += intensity_rsv;
        if(band_bits >= alloc_floor) {
            /*If we have enough for a fine energy bit per channel, use it.*/
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else {
            /*Otherwise this band gets nothing at all.*/
            bits[j] = 0;
        }
    }

    assert(codedBands > 0);
    /* Code the intensity and dual stereo parameters. */
    if(intensity_rsv > 0) {
        *intensity = ec_dec_uint(codedBands + 1);
    } else
        *intensity = 0;
    if(*intensity <= 0) {
        total += dual_stereo_rsv;
        dual_stereo_rsv = 0;
    }
    if(dual_stereo_rsv > 0) {
        *dual_stereo = ec_dec_bit_logp(1);
    } else
        *dual_stereo = 0;

    /* Allocate the remaining bits */
    left = total - psum;
    assert(eband5ms[codedBands] - eband5ms[0] > 0);
    percoeff = left / (eband5ms[codedBands] - eband5ms[0]);
    left -= (eband5ms[codedBands] - eband5ms[0]) * percoeff;
    for(j = 0; j < codedBands; j++)
        bits[j] += ((int32_t)percoeff * (eband5ms[j + 1] - eband5ms[j]));
    for(j = 0; j < codedBands; j++) {
        int32_t tmp = (int32_t)min(left, eband5ms[j + 1] - eband5ms[j]);
        bits[j] += tmp;
        left -= tmp;
    }
    /*for (j=0;j<end;j++)printf("%d ", bits[j]);printf("\n");*/

    balance = 0;
    for(j = 0; j < codedBands; j++) {
        int32_t N0, N, den;
        int32_t offset;
        int32_t NClogN;
        int32_t excess, bit;

        assert(bits[j] >= 0);
        N0 = eband5ms[j + 1] - eband5ms[j];
        N = N0 << LM;
        bit = (int32_t)bits[j] + balance;

        if(N > 1) {
            excess = max(bit - cap[j], 0);
            bits[j] = bit - excess;

            /* Compensate for the extra DoF in stereo */
            den = (C * N + ((C == 2 && N > 2 && !*dual_stereo && j < *intensity) ? 1 : 0));

            NClogN = den * (logN400[j] + logM);

            /* Offset for the number of fine bits by log2(N)/2 + 21 (FINE_OFFSET)
               compared to their "fair share" of total/N */
            offset = (NClogN >> 1) - den * 21;

            /* N=2 is the only point that doesn't match the curve */
            if(N == 2) offset += den << BITRES >> 2;

            /* Changing the offset for allocating the second and third
                fine energy bit */
            if(bits[j] + offset < den * 2 << BITRES) offset += NClogN >> 2;
            else if(bits[j] + offset < den * 3 << BITRES)
                offset += NClogN >> 3;

            /* Divide with rounding */
            ebits[j] = max(0, (bits[j] + offset + (den << (BITRES - 1))));
            assert(den > 0);
            ebits[j] = (ebits[j] / den) >> BITRES;

            /* Make sure not to bust */
            if(C * ebits[j] > (bits[j] >> BITRES)) ebits[j] = bits[j] >> stereo >> BITRES;

            /* More than that is useless because that's about as far as PVQ can go */
            ebits[j] = min(ebits[j], MAX_FINE_BITS);

            /* If we rounded down or capped this band, make it a candidate for the
                final fine energy pass */
            fine_priority[j] = ebits[j] * (den << BITRES) >= bits[j] + offset;

            /* Remove the allocated fine bits; the rest are assigned to PVQ */
            bits[j] -= C * ebits[j] << BITRES;

        } else {
            /* For N=1, all bits go to fine energy except for a single sign bit */
            excess = max(0, bit - (C << BITRES));
            bits[j] = bit - excess;
            ebits[j] = 0;
            fine_priority[j] = 1;
        }

        /* Fine energy can't take advantage of the re-balancing in
            quant_all_bands().
           Instead, do the re-balancing here.*/
        if(excess > 0) {
            int32_t extra_fine;
            int32_t extra_bits;
            extra_fine = min(excess >> (stereo + BITRES), MAX_FINE_BITS - ebits[j]);
            ebits[j] += extra_fine;
            extra_bits = extra_fine * C << BITRES;
            fine_priority[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;

        assert(bits[j] >= 0);
        assert(ebits[j] >= 0);
    }
    /* Save any remaining bits over the cap for the rebalancing in
        quant_all_bands(). */
    *_balance = balance;

    /* The skipped bands use all their bits for fine energy. */
    for(; j < end; j++) {
        ebits[j] = bits[j] >> stereo >> BITRES;
        assert(C * ebits[j] << BITRES == bits[j]);
        bits[j] = 0;
        fine_priority[j] = ebits[j] < 1;
    }

    return codedBands;
}
//----------------------------------------------------------------------------------------------------------------------

int32_t clt_compute_allocation(const int32_t *offsets, const int32_t *cap, int32_t alloc_trim,
                           int32_t *intensity, int32_t *dual_stereo, int32_t total, int32_t *balance, int32_t *pulses, int32_t *ebits,
                           int32_t *fine_priority, int32_t C, int32_t LM) {
    int32_t lo, hi, len, j;
    int32_t codedBands;
    int32_t skip_start = 0;
    int32_t skip_rsv;
    int32_t intensity_rsv;
    int32_t dual_stereo_rsv;
    const uint8_t end = cdec->end;  // 21

    total = max(total, 0);
    len = m_CELTMode.nbEBands; // =21
    /* Reserve a bit to signal the end of manually skipped bands. */
    skip_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
    total -= skip_rsv;
    /* Reserve bits for the intensity and dual stereo parameters. */
    intensity_rsv = dual_stereo_rsv = 0;
    if (C == 2) {
        intensity_rsv = LOG2_FRAC_TABLE[end];
        if (intensity_rsv > total)
            intensity_rsv = 0;
        else {
            total -= intensity_rsv;
            dual_stereo_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
            total -= dual_stereo_rsv;
        }
    }

    assert(len <= 21);
    int32_t* bits1       = s_bits1Buff;
    int32_t* bits2       = s_bits2Buff;
    int32_t* thresh      = s_threshBuff;
    int32_t* trim_offset = s_trim_offsetBuff;

    for (j = 0; j < end; j++) {
        /* Below this threshold, we're sure not to allocate any PVQ bits */
        thresh[j] = max((C) << BITRES, (3 * (eband5ms[j + 1] - eband5ms[j]) << LM << BITRES) >> 4);
        /* Tilt of the allocation curve */
        trim_offset[j] =
            C * (eband5ms[j + 1] - eband5ms[j]) * (alloc_trim - 5 - LM) * (end - j - 1) * (1 << (LM + BITRES)) >> 6;
        /* Giving less resolution to single-coefficient bands because they get
           more benefit from having one coarse value per coefficient*/
        if ((eband5ms[j + 1] - eband5ms[j]) << LM == 1) trim_offset[j] -= C << BITRES;
    }
    lo = 1;
    hi = m_CELTMode.nbAllocVectors - 1;
    do {
        int32_t done = 0;
        int32_t psum = 0;
        int32_t mid = (lo + hi) >> 1;
        for (j = end; j-- > 0;) {
            int32_t bitsj;
            int32_t N = eband5ms[j + 1] - eband5ms[j];
            bitsj = C * N * band_allocation[mid * len + j] << LM >> 2;
            if (bitsj > 0) bitsj = max(0, bitsj + trim_offset[j]);
            bitsj += offsets[j];
            if (bitsj >= thresh[j] || done) {
                done = 1;
                /* Don't allocate more than we can actually use */
                psum += min(bitsj, cap[j]);
            } else {
                if (bitsj >= C << BITRES) psum += C << BITRES;
            }
        }
        if (psum > total)
            hi = mid - 1;
        else
            lo = mid + 1;
        /*printf ("lo = %d, hi = %d\n", lo, hi);*/
    } while (lo <= hi);
    hi = lo--;
    /*printf ("interp between %d and %d\n", lo, hi);*/
    for (j = 0; j < end; j++) {
        int32_t bits1j, bits2j;
        int32_t N = eband5ms[j + 1] - eband5ms[j];
        bits1j = C * N * band_allocation[lo * len + j] << LM >> 2;
        bits2j = hi >= m_CELTMode.nbAllocVectors ? cap[j] : C * N * band_allocation[hi * len + j] << LM >> 2;
        if (bits1j > 0) bits1j = max(0, bits1j + trim_offset[j]);
        if (bits2j > 0) bits2j = max(0, bits2j + trim_offset[j]);
        if (lo > 0) bits1j += offsets[j];
        bits2j += offsets[j];
        if (offsets[j] > 0) skip_start = j;
        bits2j = max(0, bits2j - bits1j);
        bits1[j] = bits1j;
        bits2[j] = bits2j;
    }
    codedBands = interp_bits2pulses(end, skip_start, bits1, bits2, thresh, cap, total, balance, skip_rsv,
                                    intensity, intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits,
                                    fine_priority, C, LM);

    return codedBands;
}
//----------------------------------------------------------------------------------------------------------------------

void unquant_coarse_energy(int16_t *oldEBands, int32_t intra, int32_t C, int32_t LM) {
    const uint8_t *prob_model = e_prob_model[LM][intra];
    int32_t i, c;
    int32_t prev[2] = {0, 0};
    int16_t coef;
    int16_t beta;
    int32_t budget;
    int32_t tell;
    const uint8_t end = cdec->end;  // 21

    if (intra) {
        coef = 0;
        beta = beta_intra;
    } else {
        beta = beta_coef[LM];
        coef = pred_coef[LM];
    }

    budget = s_ec.storage * 8;

    /* Decode at a fixed coarse resolution */
    for (i = 0; i < end; i++) {
        c = 0;
        do {
            int32_t qi;
            int32_t q;
            int32_t tmp;
            /* It would be better to express this invariant as a
               test on C at function entry, but that isn't enough
               to make the static analyzer happy. */
            assert(c < 2);
            tell = ec_tell();
            if (budget - tell >= 15) {
                int32_t pi;
                pi = 2 * min(i, 20);
                qi = ec_laplace_decode(prob_model[pi] << 7, prob_model[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = ec_dec_icdf(small_energy_icdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) {
                qi = -ec_dec_bit_logp(1);
            } else
                qi = -1;
            q = (int32_t)SHL32(EXTEND32(qi), 10);

            oldEBands[i + c * m_CELTMode.nbEBands] = max(-QCONST16(9.f, 10), oldEBands[i + c * m_CELTMode.nbEBands]);
            tmp = PSHR(MULT16_16(coef, oldEBands[i + c * m_CELTMode.nbEBands]), 8) + prev[c] + SHL32(q, 7);
            tmp = max(-QCONST32(28.f, 10 + 7), tmp);
            oldEBands[i + c * m_CELTMode.nbEBands] = PSHR(tmp, 7);
            prev[c] = prev[c] + SHL32(q, 7) - MULT16_16(beta, PSHR(q, 8));
        } while (++c < C);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void unquant_fine_energy(int16_t *oldEBands, int32_t *fine_quant, int32_t C) {
    int32_t i, c;
    const uint8_t end = cdec->end;  // 21
    /* Decode finer resolution */
    for (i = 0; i < end; i++) {
        if (fine_quant[i] <= 0) continue;
        c = 0;
        do {
            int32_t q2;
            int16_t offset;
            q2 = ec_dec_bits(fine_quant[i]);
            offset = SUB16(SHR32(SHL32(EXTEND32(q2), 10) + QCONST16(.5f, 10), fine_quant[i]),
                           QCONST16(.5f, 10));
            oldEBands[i + c * m_CELTMode.nbEBands] += offset;
        } while (++c < C);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void unquant_energy_finalise(int16_t *oldEBands, int32_t *fine_quant,
                             int32_t *fine_priority, int32_t bits_left, int32_t C) {
    int32_t i, prio, c;
    const uint8_t  end = cdec->end;  // 21

    /* Use up the remaining bits */
    for (prio = 0; prio < 2; prio++) {
        for (i = 0; i < end && bits_left >= C; i++) {
            if (fine_quant[i] >= MAX_FINE_BITS || fine_priority[i] != prio) continue;
            c = 0;
            do {
                int32_t q2;
                int16_t offset;
                q2 = ec_dec_bits(1);
                offset = SHR16(SHL16(q2, 10) - QCONST16(.5f, 10), fine_quant[i] + 1);
                oldEBands[i + c * m_CELTMode.nbEBands] += offset;
                bits_left--;
            } while (++c < C);
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------
