/* ber-help.c - BER herlper functions
 *      Copyright (C) 2001, 2012 g10 Code GmbH
 *
 * This file is part of KSBA.
 *
 * KSBA is free software; you can redistribute it and/or modify
 * it under the terms of either
 *
 *   - the GNU Lesser General Public License as published by the Free
 *     Software Foundation; either version 3 of the License, or (at
 *     your option) any later version.
 *
 * or
 *
 *   - the GNU General Public License as published by the Free
 *     Software Foundation; either version 2 of the License, or (at
 *     your option) any later version.
 *
 * or both in parallel, as here.
 *
 * KSBA is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copies of the GNU General Public License
 * and the GNU Lesser General Public License along with this program;
 * if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "util.h"

#include "asn1-func.h" /* need some constants */
#include "convert.h"
#include "ber-help.h"

/* Fixme: The parser functions should check that primitive types don't
   have the constructed bit set (which is not allowed).  This saves us
   some work when using these parsers */

static int
read_byte (ksba_reader_t reader)
{
  unsigned char buf;
  size_t nread;
  int rc;

  do
    rc = ksba_reader_read (reader, &buf, 1, &nread);
  while (!rc && !nread);
  return rc? -1: buf;
}


static int
premature_eof (struct tag_info *ti)
{
  /* Note: We do an strcmp on this string at other places. */
  ti->err_string = "premature EOF";
  return gpg_error (GPG_ERR_BAD_BER);
}



static gpg_error_t
eof_or_error (ksba_reader_t reader, struct tag_info *ti, int premature)
{
  gpg_error_t err;

  err = ksba_reader_error (reader);
  if (err)
    {
      ti->err_string = "read error";
      return err;
    }
  if (premature)
    return premature_eof (ti);

  return gpg_error (GPG_ERR_EOF);
}



/*
   Read the tag and the length part from the TLV triplet.
 */
gpg_error_t
_ksba_ber_read_tl (ksba_reader_t reader, struct tag_info *ti)
{
  int c;
  unsigned long tag;

  ti->length = 0;
  ti->ndef = 0;
  ti->nhdr = 0;
  ti->err_string = NULL;
  ti->non_der = 0;

  /* Get the tag */
  c = read_byte (reader);
  if (c==-1)
    return eof_or_error (reader, ti, 0);

  ti->buf[ti->nhdr++] = c;
  ti->class = (c & 0xc0) >> 6;
  ti->is_constructed = !!(c & 0x20);
  tag = c & 0x1f;

  if (tag == 0x1f)
    {
      tag = 0;
      do
        {
          /* We silently ignore an overflow in the tag value.  It is
             not worth checking for it. */
          tag <<= 7;
          c = read_byte (reader);
          if (c == -1)
            return eof_or_error (reader, ti, 1);
          if (ti->nhdr >= DIM (ti->buf))
            {
              ti->err_string = "tag+length header too large";
              return gpg_error (GPG_ERR_BAD_BER);
            }
          ti->buf[ti->nhdr++] = c;
          tag |= c & 0x7f;
        }
      while (c & 0x80);
    }
  ti->tag = tag;

  /* Get the length */
  c = read_byte (reader);
  if (c == -1)
    return eof_or_error (reader, ti, 1);
  if (ti->nhdr >= DIM (ti->buf))
    {
      ti->err_string = "tag+length header too large";
      return gpg_error (GPG_ERR_BAD_BER);
    }
  ti->buf[ti->nhdr++] = c;

  if ( !(c & 0x80) )
    ti->length = c;
  else if (c == 0x80)
    {
      ti->ndef = 1;
      ti->non_der = 1;
    }
  else if (c == 0xff)
    {
      ti->err_string = "forbidden length value";
      return gpg_error (GPG_ERR_BAD_BER);
    }
  else
    {
      unsigned long len = 0;
      int count = c & 0x7f;

      if (count > sizeof (len) || count > sizeof (size_t))
        return gpg_error (GPG_ERR_BAD_BER);

      for (; count; count--)
        {
          len <<= 8;
          c = read_byte (reader);
          if (c == -1)
            return eof_or_error (reader, ti, 1);
          if (ti->nhdr >= DIM (ti->buf))
            {
              ti->err_string = "tag+length header too large";
              return gpg_error (GPG_ERR_BAD_BER);
            }
          ti->buf[ti->nhdr++] = c;
          len |= c & 0xff;
        }
      ti->length = len;
    }

  if (ti->length > ti->nhdr && (ti->nhdr + ti->length) < ti->length)
    {
      ti->err_string = "header+length would overflow";
      return gpg_error (GPG_ERR_EOVERFLOW);
    }

  /* Without this kludge some example certs can't be parsed */
  if (ti->class == CLASS_UNIVERSAL && !ti->tag)
    ti->length = 0;

  return 0;
}


/* Parse the buffer at the address BUFFER which of SIZE and return the
 * tag and the length part from the TLV triplet.  Update BUFFER and
 * SIZE on success.  Note that this function will never return
 * GPG_ERR_INV_OBJ so that this error code can be used by the parse_foo
 * functions below to return an error for unexpected tags and the
 * caller is able to backoff in that case.  */
gpg_error_t
_ksba_ber_parse_tl (unsigned char const **buffer, size_t *size,
                    struct tag_info *ti)
{
  int c;
  unsigned long tag;
  const unsigned char *buf = *buffer;
  size_t length = *size;

  ti->length = 0;
  ti->ndef = 0;
  ti->nhdr = 0;
  ti->err_string = NULL;
  ti->non_der = 0;

  /* Get the tag */
  if (!length)
    return premature_eof (ti);
  c = *buf++; length--;

  ti->buf[ti->nhdr++] = c;
  ti->class = (c & 0xc0) >> 6;
  ti->is_constructed = !!(c & 0x20);
  tag = c & 0x1f;

  if (tag == 0x1f)
    {
      tag = 0;
      do
        {
          /* We silently ignore an overflow in the tag value.  It is
             not worth checking for it. */
          tag <<= 7;
          if (!length)
            return premature_eof (ti);
          c = *buf++; length--;
          if (ti->nhdr >= DIM (ti->buf))
            {
              ti->err_string = "tag+length header too large";
              return gpg_error (GPG_ERR_BAD_BER);
            }
          ti->buf[ti->nhdr++] = c;
          tag |= c & 0x7f;
        }
      while (c & 0x80);
    }
  ti->tag = tag;

  /* Get the length */
  if (!length)
    return premature_eof (ti);
  c = *buf++; length--;
  if (ti->nhdr >= DIM (ti->buf))
    {
      ti->err_string = "tag+length header too large";
      return gpg_error (GPG_ERR_BAD_BER);
    }
  ti->buf[ti->nhdr++] = c;

  if ( !(c & 0x80) )
    ti->length = c;
  else if (c == 0x80)
    {
      ti->ndef = 1;
      ti->non_der = 1;
    }
  else if (c == 0xff)
    {
      ti->err_string = "forbidden length value";
      return gpg_error (GPG_ERR_BAD_BER);
    }
  else
    {
      unsigned long len = 0;
      int count = c & 0x7f;

      if (count > sizeof (len) || count > sizeof (size_t))
        return gpg_error (GPG_ERR_BAD_BER);

      for (; count; count--)
        {
          len <<= 8;
          if (!length)
            return premature_eof (ti);
          c = *buf++; length--;
          if (ti->nhdr >= DIM (ti->buf))
            {
              ti->err_string = "tag+length header too large";
              return gpg_error (GPG_ERR_BAD_BER);
            }
          ti->buf[ti->nhdr++] = c;
          len |= c & 0xff;
        }
      /* Sanity check for the length: This is done so that we can take
       * the value for malloc plus some additional bytes without
       * risking an overflow.  */
      if (len > (1 << 30))
        return gpg_error (GPG_ERR_BAD_BER);
      ti->length = len;
    }


  /* Without this kludge some example certs can't be parsed */
  if (ti->class == CLASS_UNIVERSAL && !ti->tag)
    ti->length = 0;

  *buffer = buf;
  *size = length;
  return 0;
}


/* Write TAG of CLASS to WRITER.  constructed is a flag telling
   whether the value is a constructed one.  length gives the length of
   the value, if it is 0 undefinite length is assumed.  length is
   ignored for the NULL tag. */
gpg_error_t
_ksba_ber_write_tl (ksba_writer_t writer,
                    unsigned long tag,
                    enum tag_class class,
                    int constructed,
                    unsigned long length)
{
  unsigned char buf[50];
  int buflen = 0;

  if (tag < 0x1f)
    {
      *buf = (class << 6) | tag;
      if (constructed)
        *buf |= 0x20;
      buflen++;
    }
  else
    {
      return gpg_error (GPG_ERR_NOT_IMPLEMENTED);
    }

  if (!tag && !class)
    buf[buflen++] = 0; /* end tag */
  else if (tag == TYPE_NULL && !class)
    buf[buflen++] = 0; /* NULL tag */
  else if (!length)
    buf[buflen++] = 0x80; /* indefinite length */
  else if (length < 128)
    buf[buflen++] = length;
  else
    {
      int i;

      /* fixme: if we know the sizeof an ulong we could support larger
         objects - however this is pretty ridiculous */
      i = (length <= 0xff ? 1:
           length <= 0xffff ? 2:
           length <= 0xffffff ? 3: 4);

      buf[buflen++] = (0x80 | i);
      if (i > 3)
        buf[buflen++] = length >> 24;
      if (i > 2)
        buf[buflen++] = length >> 16;
      if (i > 1)
        buf[buflen++] = length >> 8;
      buf[buflen++] = length;
    }

  return ksba_writer_write (writer, buf, buflen);
}

/* Encode TAG of CLASS in BUFFER.  CONSTRUCTED is a flag telling
   whether the value is a constructed one.  LENGTH gives the length of
   the value, if it is 0 undefinite length is assumed.  LENGTH is
   ignored for the NULL tag. It is assumed that the provide buffer is
   large enough for storing the result - this is usually achieved by
   using _ksba_ber_count_tl() in advance.  Returns 0 in case of an
   error or the length of the encoding.*/
size_t
_ksba_ber_encode_tl (unsigned char *buffer,
                     unsigned long tag,
                     enum tag_class class,
                     int constructed,
                     unsigned long length)
{
  unsigned char *buf = buffer;

  if (tag < 0x1f)
    {
      *buf = (class << 6) | tag;
      if (constructed)
        *buf |= 0x20;
      buf++;
    }
  else
    {
      return 0; /*Not implemented*/
    }

  if (!tag && !class)
    *buf++ = 0; /* end tag */
  else if (tag == TYPE_NULL && !class)
    *buf++ = 0; /* NULL tag */
  else if (!length)
    *buf++ = 0x80; /* indefinite length */
  else if (length < 128)
    *buf++ = length;
  else
    {
      int i;

      /* fixme: if we know the sizeof an ulong we could support larger
         objetcs - however this is pretty ridiculous */
      i = (length <= 0xff ? 1:
           length <= 0xffff ? 2:
           length <= 0xffffff ? 3: 4);

      *buf++ = (0x80 | i);
      if (i > 3)
        *buf++ = length >> 24;
      if (i > 2)
        *buf++ = length >> 16;
      if (i > 1)
        *buf++ = length >> 8;
      *buf++ = length;
    }

  return buf - buffer;
}


/* Calculate the length of the TL needed to encode a TAG of CLASS.
   CONSTRUCTED is a flag telling whether the value is a constructed
   one.  LENGTH gives the length of the value; if it is 0 an
   indefinite length is assumed.  LENGTH is ignored for the NULL
   tag. */
size_t
_ksba_ber_count_tl (unsigned long tag,
                    enum tag_class class,
                    int constructed,
                    unsigned long length)
{
  int buflen = 0;

  (void)constructed;  /* Not used, but passed for uniformity of such calls.  */

  /* coverity[identical_branches] */
  if (tag < 0x1f)
    {
      buflen++;
    }
  else
    {
      buflen++; /* assume one and let the actual write function bail out */
    }

  if (!tag && !class)
    buflen++; /* end tag */
  else if (tag == TYPE_NULL && !class)
    buflen++; /* NULL tag */
  else if (!length)
    buflen++; /* indefinite length */
  else if (length < 128)
    buflen++;
  else
    {
      int i;

      /* fixme: if we know the sizeof an ulong we could support larger
         objetcs - however this is pretty ridiculous */
      i = (length <= 0xff ? 1:
           length <= 0xffff ? 2:
           length <= 0xffffff ? 3: 4);

      buflen++;
      if (i > 3)
        buflen++;
      if (i > 2)
        buflen++;
      if (i > 1)
        buflen++;
      buflen++;
    }

  return buflen;
}


gpg_error_t
_ksba_parse_sequence (unsigned char const **buf, size_t *len,
                      struct tag_info *ti)
{
  gpg_error_t err;

  err = _ksba_ber_parse_tl (buf, len, ti);
  if (err)
    ;
  else if (!(ti->class == CLASS_UNIVERSAL && ti->tag == TYPE_SEQUENCE
             && ti->is_constructed) )
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (ti->length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);
  return err;
}


/* Note that this function returns GPG_ERR_FALSE if the TLV is valid
 * but the tag does not match.  The caller may thus check for this
 * error code and compare against other tag values.  */
gpg_error_t
_ksba_parse_context_tag (unsigned char const **buf, size_t *len,
                         struct tag_info *ti, int tag)
{
  gpg_error_t err;

  err = _ksba_ber_parse_tl (buf, len, ti);
  if (err)
    ;
  else if (!(ti->class == CLASS_CONTEXT && ti->is_constructed))
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (ti->length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);
  else if (ti->tag != tag)
    err = gpg_error (GPG_ERR_FALSE);

  return err;
}


gpg_error_t
_ksba_parse_enumerated (unsigned char const **buf, size_t *len,
                        struct tag_info *ti, size_t maxlen)
{
  gpg_error_t err;

  err = _ksba_ber_parse_tl (buf, len, ti);
  if (err)
     ;
  else if (!(ti->class == CLASS_UNIVERSAL && ti->tag == TYPE_ENUMERATED
             && !ti->is_constructed) )
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (!ti->length)
    err = gpg_error (GPG_ERR_TOO_SHORT);
  else if (maxlen && ti->length > maxlen)
    err = gpg_error (GPG_ERR_TOO_LARGE);
  else if (ti->length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);

  return err;
}


gpg_error_t
_ksba_parse_integer (unsigned char const **buf, size_t *len,
                     struct tag_info *ti)
{
  gpg_error_t err;

  err = _ksba_ber_parse_tl (buf, len, ti);
  if (err)
     ;
  else if (!(ti->class == CLASS_UNIVERSAL && ti->tag == TYPE_INTEGER
             && !ti->is_constructed) )
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (!ti->length)
    err = gpg_error (GPG_ERR_TOO_SHORT);
  else if (ti->length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);

  return err;
}


gpg_error_t
_ksba_parse_octet_string (unsigned char const **buf, size_t *len,
                          struct tag_info *ti)
{
  gpg_error_t err;

  err= _ksba_ber_parse_tl (buf, len, ti);
  if (err)
    ;
  else if (!(ti->class == CLASS_UNIVERSAL && ti->tag == TYPE_OCTET_STRING
             && !ti->is_constructed) )
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (!ti->length)
    err = gpg_error (GPG_ERR_TOO_SHORT);
  else if (ti->length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);

  return err;
}


/* Note that R_BOOL will only be set if a value has been given. Thus
   the caller should set it to the default value prior to calling this
   function.  Obviously no call to parse_skip is required after
   calling this function. */
gpg_error_t
_ksba_parse_optional_boolean (unsigned char const **buf, size_t *len,
                              int *r_bool)
{
  gpg_error_t err;
  struct tag_info ti;

  err = _ksba_ber_parse_tl (buf, len, &ti);
  if (err)
    ;
  else if (!ti.length)
    err = gpg_error (GPG_ERR_TOO_SHORT);
  else if (ti.length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);
  else if (ti.class == CLASS_UNIVERSAL && ti.tag == TYPE_BOOLEAN
           && !ti.is_constructed)
    {
      if (ti.length != 1)
        err = gpg_error (GPG_ERR_BAD_BER);
      *r_bool = !!**buf;
      parse_skip (buf, len, &ti);
    }
  else
    { /* Undo the read. */
      *buf -= ti.nhdr;
      *len += ti.nhdr;
    }

  return err;
}


/* Parse an optional Null tag.  Ir R_SEEN is not NULL it is set to
 * true if a NULL tag was encountered.  */
gpg_error_t
_ksba_parse_optional_null (unsigned char const **buf, size_t *len,
                           int *r_seen)
{
  gpg_error_t err;
  struct tag_info ti;

  if (r_seen)
    *r_seen = 0;
  err = _ksba_ber_parse_tl (buf, len, &ti);
  if (err)
    ;
  else if (ti.length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);
  else if (ti.class == CLASS_UNIVERSAL && ti.tag == TYPE_NULL
           && !ti.is_constructed)
    {
      if (ti.length)
        err = gpg_error (GPG_ERR_BAD_BER);
      if (r_seen)
        *r_seen = 1;
      parse_skip (buf, len, &ti);
    }
  else
    { /* Undo the read. */
      *buf -= ti.nhdr;
      *len += ti.nhdr;
    }

  return err;
}



gpg_error_t
_ksba_parse_object_id_into_str (unsigned char const **buf, size_t *len,
                                char **oid)
{
  struct tag_info ti;
  gpg_error_t err;

  *oid = NULL;
  err = _ksba_ber_parse_tl (buf, len, &ti);
  if (err)
    ;
  else if (!(ti.class == CLASS_UNIVERSAL && ti.tag == TYPE_OBJECT_ID
                && !ti.is_constructed) )
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (!ti.length)
    err = gpg_error (GPG_ERR_TOO_SHORT);
  else if (ti.length > *len)
    err = gpg_error (GPG_ERR_BAD_BER);
  else if (!(*oid = ksba_oid_to_str (*buf, ti.length)))
    err = gpg_error_from_syserror ();
  else
    {
      *buf += ti.length;
      *len -= ti.length;
    }
  return err;
}


gpg_error_t
_ksba_parse_asntime_into_isotime (unsigned char const **buf, size_t *len,
                                  ksba_isotime_t isotime)
{
  struct tag_info ti;
  gpg_error_t err;

  err = _ksba_ber_parse_tl (buf, len, &ti);
  if (err)
    ;
  else if ( !(ti.class == CLASS_UNIVERSAL
              && (ti.tag == TYPE_UTC_TIME || ti.tag == TYPE_GENERALIZED_TIME)
              && !ti.is_constructed) )
    err = gpg_error (GPG_ERR_INV_OBJ);
  else if (ti.length > *len)
    err = gpg_error (GPG_ERR_INV_BER);
  else if (!(err = _ksba_asntime_to_iso (*buf, ti.length,
                                         ti.tag == TYPE_UTC_TIME, isotime)))
    parse_skip (buf, len, &ti);

  return err;
}
