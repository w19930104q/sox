/* libSoX file format: W64 (Sonic Foundry Wave64)
 *
 * Native reader/writer for uncompressed PCM (16/24/32-bit) and
 * IEEE float Wave64 files.  Compressed encodings are rejected with
 * an explicit error rather than producing a broken file.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "sox_i.h"
#include <string.h>

/* Wave64 chunk GUIDs, stored as the 16 raw little-endian bytes that
 * appear in the file. */
static const uint8_t w64_riff_guid[16] = {
  0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
  0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00
};
static const uint8_t w64_wave_guid[16] = {
  0x77, 0x61, 0x76, 0x65, 0xF3, 0xAC, 0xD3, 0x11,
  0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};
static const uint8_t w64_fmt_guid[16] = {
  0x66, 0x6D, 0x74, 0x20, 0xF3, 0xAC, 0xD3, 0x11,
  0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};
static const uint8_t w64_data_guid[16] = {
  0x64, 0x61, 0x74, 0x61, 0xF3, 0xAC, 0xD3, 0x11,
  0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A
};
/* Last 14 bytes of a WAVEFORMATEXTENSIBLE sub-format GUID
 * ({00000000-0000-0010-8000-00AA00389B71} minus the 2-byte tag). */
static const uint8_t w64_subformat_tail[14] = {
  0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
  0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

#define W64_CHUNK_HEADER 24          /* 16-byte GUID + 8-byte length */
#define W64_ALIGN        8           /* chunks are 8-byte aligned    */

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

typedef struct {
  uint64_t data_length;   /* bytes of sample data in the data chunk  */
  uint64_t frames_left;   /* frames not yet read (per channel count) */
  sox_bool ignore_size;   /* read to EOF, ignoring the header length */
  off_t    data_start;    /* file offset of the sample data          */
  off_t    riff_size_pos; /* where the 8-byte RIFF length is stored  */
  off_t    data_size_pos; /* where the 8-byte data length is stored  */
} priv_t;

static int w64_read_guid(sox_format_t * ft, uint8_t guid[16])
{
  return lsx_readchars(ft, (char *)guid, 16);
}

/* Skip n bytes, works on non-seekable streams too. */
static int w64_skip(sox_format_t * ft, uint64_t n)
{
  while (n) {
    size_t chunk = n > 0x40000000 ? 0x40000000 : (size_t)n;
    if (lsx_skipbytes(ft, chunk))
      return SOX_EOF;
    n -= chunk;
  }
  return SOX_SUCCESS;
}

static int w64_read_fmt(sox_format_t * ft, uint64_t payload)
{
  uint16_t format_tag, channels, block_align, bits_per_sample;
  uint32_t sample_rate, avg_bytes_per_sec;
  uint32_t len;
  int      err;

  if (payload < 16 || payload > UINT32_MAX) {
    lsx_fail_errno(ft, SOX_EHDR, "W64: bad fmt chunk size");
    return SOX_EOF;
  }
  len = (uint32_t)payload;

  err = lsx_read_fields(ft, &len, "hhiihh",
                        &format_tag, &channels, &sample_rate,
                        &avg_bytes_per_sec, &block_align, &bits_per_sample);
  if (err)
    return SOX_EOF;

  if (format_tag == WAVE_FORMAT_EXTENSIBLE) {
    uint16_t ext_size, valid_bits, sub_tag;
    uint32_t channel_mask;
    uint8_t  tail[14];

    if (len < 24) {
      lsx_fail_errno(ft, SOX_EHDR,
                     "W64: extensible fmt chunk is too short");
      return SOX_EOF;
    }
    err = lsx_read_fields(ft, &len, "hhi", &ext_size, &valid_bits,
                          &channel_mask);
    if (err)
      return SOX_EOF;
    err = lsx_read_fields(ft, &len, "h", &sub_tag);
    if (err)
      return SOX_EOF;
    if (lsx_readchars(ft, (char *)tail, 14))
      return SOX_EOF;
    len -= 14;
    if (memcmp(tail, w64_subformat_tail, 14)) {
      lsx_fail_errno(ft, SOX_EHDR,
                     "W64: unsupported extensible sub-format GUID");
      return SOX_EOF;
    }
    format_tag = sub_tag;
  }

  /* Skip anything left of the fmt payload. */
  if (w64_skip(ft, len))
    return SOX_EOF;

  switch (format_tag) {
    case WAVE_FORMAT_PCM:
      if (bits_per_sample == 8)
        ft->encoding.encoding = SOX_ENCODING_UNSIGNED;
      else if (bits_per_sample == 16 || bits_per_sample == 24 ||
               bits_per_sample == 32)
        ft->encoding.encoding = SOX_ENCODING_SIGN2;
      else {
        lsx_fail_errno(ft, SOX_EHDR,
                       "W64: %u-bit PCM not supported", bits_per_sample);
        return SOX_EOF;
      }
      break;
    case WAVE_FORMAT_IEEE_FLOAT:
      if (bits_per_sample != 32 && bits_per_sample != 64) {
        lsx_fail_errno(ft, SOX_EHDR,
                       "W64: %u-bit float not supported", bits_per_sample);
        return SOX_EOF;
      }
      ft->encoding.encoding = SOX_ENCODING_FLOAT;
      break;
    default:
      lsx_fail_errno(ft, SOX_EHDR,
                     "W64: format tag 0x%04x not supported "
                     "(compressed encodings are not handled)",
                     format_tag);
      return SOX_EOF;
  }

  if (!channels) {
    lsx_fail_errno(ft, SOX_EHDR, "W64: channel count is zero");
    return SOX_EOF;
  }
  if (ft->signal.channels == 0 || ft->signal.channels == channels)
    ft->signal.channels = channels;
  else
    lsx_report("User options overriding channels read in W64 header");

  if (ft->signal.rate == 0 || ft->signal.rate == sample_rate)
    ft->signal.rate = sample_rate;
  else
    lsx_report("User options overriding rate read in W64 header");

  if (ft->encoding.bits_per_sample == 0 ||
      ft->encoding.bits_per_sample == bits_per_sample)
    ft->encoding.bits_per_sample = bits_per_sample;
  else
    lsx_report("User options overriding size read in W64 header");

  return SOX_SUCCESS;
}

static int startread(sox_format_t * ft)
{
  priv_t  * w64 = (priv_t *)ft->priv;
  uint8_t   guid[16];
  uint64_t  riff_length;
  sox_bool  have_fmt = sox_false, have_data = sox_false;

  ft->sox_errno = SOX_SUCCESS;

  if (w64_read_guid(ft, guid) || memcmp(guid, w64_riff_guid, 16)) {
    lsx_fail_errno(ft, SOX_EHDR, "W64: RIFF GUID header not found");
    return SOX_EOF;
  }
  if (lsx_readqw(ft, &riff_length))
    return SOX_EOF;
  if (w64_read_guid(ft, guid) || memcmp(guid, w64_wave_guid, 16)) {
    lsx_fail_errno(ft, SOX_EHDR, "W64: WAVE GUID header not found");
    return SOX_EOF;
  }

  while (!have_data || !have_fmt) {
    uint64_t chunk_size, payload, padded;
    size_t   got = lsx_readbuf(ft, guid, 16);

    if (got == 0)
      break;                       /* clean EOF */
    if (got != 16) {
      lsx_fail_errno(ft, SOX_EHDR, "W64: truncated chunk header");
      return SOX_EOF;
    }
    if (lsx_readqw(ft, &chunk_size))
      return SOX_EOF;
    if (chunk_size < W64_CHUNK_HEADER) {
      lsx_fail_errno(ft, SOX_EHDR, "W64: invalid chunk size");
      return SOX_EOF;
    }
    payload = chunk_size - W64_CHUNK_HEADER;
    padded = (payload + (W64_ALIGN - 1)) & ~(uint64_t)(W64_ALIGN - 1);

    if (!memcmp(guid, w64_fmt_guid, 16)) {
      if (w64_read_fmt(ft, payload))
        return SOX_EOF;
      have_fmt = sox_true;
      if (w64_skip(ft, padded - payload))
        return SOX_EOF;
    } else if (!memcmp(guid, w64_data_guid, 16)) {
      if (!have_fmt && !ft->seekable) {
        lsx_fail_errno(ft, SOX_EHDR,
                       "W64: data chunk found before fmt chunk");
        return SOX_EOF;
      }
      w64->data_length = payload;
      w64->data_start = lsx_tell(ft);
      have_data = sox_true;
      if (!ft->seekable)
        break;                     /* start streaming the samples */
      if (w64_skip(ft, padded))
        return SOX_EOF;
    } else {
      lsx_debug("W64: skipping unknown chunk, size %lu",
                (unsigned long)chunk_size);
      if (w64_skip(ft, padded))
        return SOX_EOF;
    }
  }

  if (!have_fmt) {
    lsx_fail_errno(ft, SOX_EHDR, "W64: fmt chunk not found");
    return SOX_EOF;
  }
  if (!have_data) {
    lsx_fail_errno(ft, SOX_EHDR, "W64: data chunk not found");
    return SOX_EOF;
  }

  {
    unsigned bytes_per_frame =
        ft->encoding.bits_per_sample / 8 * ft->signal.channels;
    w64->frames_left =
        bytes_per_frame ? w64->data_length / bytes_per_frame : 0;
  }

  if (ft->seekable && lsx_seeki(ft, w64->data_start, SEEK_SET))
    return SOX_EOF;

  /* A zero data length is how a streaming writer says "unknown";
   * read to EOF in that case (a genuinely empty file yields 0
   * samples either way). */
  w64->ignore_size = ft->signal.length == SOX_IGNORE_LENGTH ||
                     w64->data_length == 0;

  if (w64->ignore_size)
    ft->signal.length = SOX_UNSPEC;
  else
    ft->signal.length = w64->frames_left * ft->signal.channels;

  return lsx_rawstartread(ft);
}

static size_t read_samples(sox_format_t * ft, sox_sample_t * buf, size_t len)
{
  priv_t * w64 = (priv_t *)ft->priv;
  size_t   done;

  ft->sox_errno = SOX_SUCCESS;

  if (!w64->ignore_size &&
      w64->frames_left < len / ft->signal.channels)
    len = (size_t)w64->frames_left * ft->signal.channels;
  done = lsx_rawread(ft, buf, len);
  done -= done % ft->signal.channels;
  if (!w64->ignore_size)
    w64->frames_left -= done / ft->signal.channels;

  if (done == 0 && !w64->ignore_size && w64->frames_left)
    lsx_warn("Premature EOF on W64 input file");

  return done;
}

static int stopread(sox_format_t * ft)
{
  ft->sox_errno = SOX_SUCCESS;
  return SOX_SUCCESS;
}

static int w64_write_header(sox_format_t * ft, uint64_t data_length)
{
  priv_t  * w64 = (priv_t *)ft->priv;
  uint16_t  format_tag;
  uint16_t  channels = ft->signal.channels;
  uint16_t  bits = ft->encoding.bits_per_sample;
  uint16_t  block_align = channels * (bits / 8);
  uint32_t  avg_bytes_per_sec = (uint32_t)(ft->signal.rate * block_align);
  uint64_t  riff_length, padded;

  switch (ft->encoding.encoding) {
    case SOX_ENCODING_SIGN2:
    case SOX_ENCODING_UNSIGNED:
      format_tag = WAVE_FORMAT_PCM;
      break;
    case SOX_ENCODING_FLOAT:
      format_tag = WAVE_FORMAT_IEEE_FLOAT;
      break;
    default:
      lsx_fail_errno(ft, SOX_EFMT,
                     "W64: encoding not supported; use 16/24/32-bit PCM "
                     "or 32/64-bit float");
      return SOX_EOF;
  }

  padded = (data_length + (W64_ALIGN - 1)) & ~(uint64_t)(W64_ALIGN - 1);
  /* RIFF length is the total file size. */
  riff_length = 16 + 8 + 16 +                    /* riff header        */
                W64_CHUNK_HEADER + 16 +          /* fmt chunk          */
                W64_CHUNK_HEADER + padded;       /* data chunk         */

  if (lsx_writebuf(ft, w64_riff_guid, 16) != 16)
    return SOX_EOF;
  w64->riff_size_pos = lsx_tell(ft);
  if (lsx_writeqw(ft, riff_length))
    return SOX_EOF;
  if (lsx_writebuf(ft, w64_wave_guid, 16) != 16)
    return SOX_EOF;

  if (lsx_writebuf(ft, w64_fmt_guid, 16) != 16)
    return SOX_EOF;
  if (lsx_writeqw(ft, W64_CHUNK_HEADER + 16) ||
      lsx_writew(ft, format_tag) ||
      lsx_writew(ft, channels) ||
      lsx_writedw(ft, (unsigned)ft->signal.rate) ||
      lsx_writedw(ft, avg_bytes_per_sec) ||
      lsx_writew(ft, block_align) ||
      lsx_writew(ft, bits))
    return SOX_EOF;

  if (lsx_writebuf(ft, w64_data_guid, 16) != 16)
    return SOX_EOF;
  w64->data_size_pos = lsx_tell(ft);
  if (lsx_writeqw(ft, W64_CHUNK_HEADER + data_length))
    return SOX_EOF;

  return SOX_SUCCESS;
}

static int startwrite(sox_format_t * ft)
{
  priv_t * w64 = (priv_t *)ft->priv;
  uint64_t data_length = 0;
  int      rc;

  ft->sox_errno = SOX_SUCCESS;

  if (!ft->signal.channels || ft->signal.channels > UINT16_MAX) {
    lsx_fail_errno(ft, SOX_EFMT, "W64: bad channel count (%u)",
                   ft->signal.channels);
    return SOX_EOF;
  }

  /* Reject unsupported encodings up front, before any bytes are written. */
  switch (ft->encoding.encoding) {
    case SOX_ENCODING_SIGN2:
      if (ft->encoding.bits_per_sample != 16 &&
          ft->encoding.bits_per_sample != 24 &&
          ft->encoding.bits_per_sample != 32)
        goto bad_encoding;
      break;
    case SOX_ENCODING_FLOAT:
      if (ft->encoding.bits_per_sample != 32 &&
          ft->encoding.bits_per_sample != 64)
        goto bad_encoding;
      break;
    default:
      goto bad_encoding;
  }

  rc = lsx_rawstartwrite(ft);
  if (rc)
    return rc;

  w64->data_length = 0;

  if (ft->signal.length)
    data_length = (uint64_t)ft->signal.length *
                  (ft->encoding.bits_per_sample / 8);
  else if (!ft->seekable)
    lsx_warn("Length in output W64 header will be wrong since can't seek "
             "to fix it");

  return w64_write_header(ft, data_length);

bad_encoding:
  lsx_fail_errno(ft, SOX_EFMT,
                 "W64: encoding not supported; use 16/24/32-bit PCM "
                 "or 32/64-bit float");
  return SOX_EOF;
}

static size_t write_samples(sox_format_t * ft, const sox_sample_t * buf,
                            size_t len)
{
  priv_t * w64 = (priv_t *)ft->priv;
  size_t   done = lsx_rawwrite(ft, buf, len);

  w64->data_length += (uint64_t)done * (ft->encoding.bits_per_sample / 8);
  return done;
}

static int stopwrite(sox_format_t * ft)
{
  priv_t * w64 = (priv_t *)ft->priv;
  uint64_t data_length = w64->data_length;
  uint64_t padded =
      (data_length + (W64_ALIGN - 1)) & ~(uint64_t)(W64_ALIGN - 1);

  ft->sox_errno = SOX_SUCCESS;

  /* Pad the data chunk to an 8-byte boundary; the padding is not
   * counted in the chunk length fields. */
  while (data_length < padded) {
    if (lsx_writeb(ft, 0))
      return SOX_EOF;
    data_length++;
  }

  if (ft->seekable) {
    uint64_t riff_length = (uint64_t)lsx_tell(ft);
    if (lsx_seeki(ft, w64->data_size_pos, SEEK_SET) ||
        lsx_writeqw(ft, W64_CHUNK_HEADER + w64->data_length) ||
        lsx_seeki(ft, w64->riff_size_pos, SEEK_SET) ||
        lsx_writeqw(ft, riff_length) ||
        lsx_seeki(ft, (off_t)riff_length, SEEK_SET))
      return SOX_EOF;
  }
  return SOX_SUCCESS;
}

LSX_FORMAT_HANDLER(w64)
{
  static char const * const names[] = {"w64", NULL};
  static unsigned const write_encodings[] = {
    SOX_ENCODING_SIGN2, 16, 24, 32, 0,
    SOX_ENCODING_FLOAT, 32, 64, 0,
    0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Sonic Foundry's 64-bit RIFF/WAV format", names, SOX_FILE_LIT_END,
    startread, read_samples, stopread,
    startwrite, write_samples, stopwrite,
    NULL, write_encodings, NULL, sizeof(priv_t)
  };
  return &handler;
}
