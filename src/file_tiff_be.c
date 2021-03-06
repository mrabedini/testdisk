/*

    File: file_tiff_be.c

    Copyright (C) 1998-2005,2007-2009, 2017 Christophe GRENIER <grenier@cgsecurity.org>
  
    This software is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
  
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
  
    You should have received a copy of the GNU General Public License along
    with this program; if not, write the Free Software Foundation, Inc., 51
    Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <stdio.h>
#include "types.h"
#include "filegen.h"
#include "common.h"
#include "file_tiff.h"
#include "log.h"

extern const file_hint_t file_hint_jpg;

static const char *find_tag_from_tiff_header_be_aux(const TIFFHeader *tiff, const unsigned int tiff_size, const unsigned int tag, const char**potential_error, const struct ifd_header *hdr)
{
  const TIFFDirEntry *tmp;
  unsigned int i;
  unsigned int nbr_fields;
  /* Bound checking */
  if((const char*)(hdr) <= (const char*)tiff ||
      (const char*)(hdr+1) > (const char*)tiff+tiff_size)
    return NULL;
  nbr_fields=be16(hdr->nbr_fields);
  for(i=0, tmp=&hdr->ifd;
      i < nbr_fields && (const char*)(tmp+1) <= (const char*)tiff+tiff_size;
      i++, tmp++)
  {
    if(be16(tmp->tdir_type) > 18 && (*potential_error==NULL || *potential_error > (const char*)&tmp->tdir_type+1))
    {
      *potential_error = (const char*)&tmp->tdir_type+1;
    }
    if(be16(tmp->tdir_tag)==tag)
      return (const char*)tiff+be32(tmp->tdir_offset);
  }
  return NULL;
}

const char *find_tag_from_tiff_header_be(const TIFFHeader *tiff, const unsigned int tiff_size, const unsigned int tag, const char**potential_error)
{
  const struct ifd_header *ifd0;
  const struct ifd_header *exififd;
  const uint32_t *tiff_next_diroff;
  if(tiff_size < sizeof(TIFFHeader))
    return NULL;
  if(tiff_size < be32(tiff->tiff_diroff)+sizeof(TIFFDirEntry))
    return NULL;
  ifd0=(const struct ifd_header *)((const char*)tiff + be32(tiff->tiff_diroff));
  /* Bound checking */
  if((const char*)ifd0 < (const char*)tiff ||
      (const char*)(ifd0+1) > (const char*)tiff + tiff_size)
    return NULL;
  {
    const char *tmp=find_tag_from_tiff_header_be_aux(tiff, tiff_size, tag, potential_error, ifd0);
    if(tmp)
      return tmp;
  }
  exififd=(const struct ifd_header *)find_tag_from_tiff_header_be_aux(tiff, tiff_size, TIFFTAG_EXIFIFD, potential_error, ifd0);
  if(exififd!=NULL)
  {
    /* Exif */
    const char *tmp=find_tag_from_tiff_header_be_aux(tiff, tiff_size, tag, potential_error, exififd);
    if(tmp)
      return tmp;
  }
  tiff_next_diroff=(const uint32_t *)(&ifd0->ifd + be16(ifd0->nbr_fields));
  if( (const char *)tiff_next_diroff >= (const char *)tiff &&
      (const char *)(tiff_next_diroff + 1) < (const char*)tiff + tiff_size &&
      be32(*tiff_next_diroff)>0)
  {
    /* IFD1 */
    const struct ifd_header *ifd1=(const struct ifd_header*)((const char *)tiff+be32(*tiff_next_diroff));
    return find_tag_from_tiff_header_be_aux(tiff, tiff_size, tag, potential_error, ifd1);
  }
  return NULL;
}

static uint64_t parse_strip_be(FILE *handle, const TIFFDirEntry *entry_strip_offsets, const TIFFDirEntry *entry_strip_bytecounts)
{
  const unsigned int nbr=(be32(entry_strip_offsets->tdir_count)<2048?
      be32(entry_strip_offsets->tdir_count):
      2048);
  unsigned int i;
  uint32_t *offsetp;
  uint32_t *sizep;
  uint64_t max_offset=0;
  if(be32(entry_strip_offsets->tdir_count) != be32(entry_strip_bytecounts->tdir_count))
    return -1;
  if(be32(entry_strip_offsets->tdir_count)==0 ||
      be16(entry_strip_offsets->tdir_type)!=4 ||
      be16(entry_strip_bytecounts->tdir_type)!=4)
    return -1;
  offsetp=(uint32_t *)MALLOC(nbr*sizeof(*offsetp));
  if(fseek(handle, be32(entry_strip_offsets->tdir_offset), SEEK_SET) < 0 ||
      fread(offsetp, sizeof(*offsetp), nbr, handle) != nbr)
  {
    free(offsetp);
    return -1;
  }
  sizep=(uint32_t *)MALLOC(nbr*sizeof(*sizep));
  if(fseek(handle, be32(entry_strip_bytecounts->tdir_offset), SEEK_SET) < 0 ||
      fread(sizep, sizeof(*sizep), nbr, handle) != nbr)
  {
    free(offsetp);
    free(sizep);
    return -1;
  }
  for(i=0; i<nbr; i++)
  {
    const uint64_t tmp=be32(offsetp[i]) + be32(sizep[i]);
    if(max_offset < tmp)
      max_offset=tmp;
  }
  free(offsetp);
  free(sizep);
  return max_offset;
}

static unsigned int tiff_be_read(const void *val, const unsigned int type)
{
  switch(type)
  {
    case 1:
      return *((const uint8_t*)val);
    case 3:
      return be16(*((const uint16_t*)val));
    case 4:
      return be32(*((const uint32_t*)val));
    default:
      return 0;
  }
}

#ifdef ENABLE_TIFF_MAKERNOTE
static uint64_t tiff_be_makernote(FILE *in, const uint32_t tiff_diroff)
{
  const unsigned char sign_nikon1[7]={'N', 'i', 'k', 'o', 'n', 0x00, 0x01};
  const unsigned char sign_nikon2[7]={'N', 'i', 'k', 'o', 'n', 0x00, 0x02};
  const unsigned char sign_pentax[4]={'A', 'O', 'C', 0x00};
  unsigned char buffer[8192];
  unsigned int i,n;
  int data_read;
  uint64_t max_offset=0;
  uint64_t alphaoffset=0;
  uint64_t alphabytecount=0;
  uint64_t imageoffset=0;
  uint64_t imagebytecount=0;
  uint64_t jpegifoffset=0;
  uint64_t jpegifbytecount=0;
  uint64_t strip_offsets=0;
  uint64_t strip_bytecounts=0;
  uint64_t tile_offsets=0;
  uint64_t tile_bytecounts=0;
  const TIFFDirEntry *entry;
  if(tiff_diroff < sizeof(TIFFHeader))
    return -1;
  if(fseek(in, tiff_diroff, SEEK_SET) < 0)
    return -1;
  data_read=fread(buffer, 1, sizeof(buffer), in);
  if(data_read<2)
    return -1;
  if( memcmp(buffer, sign_nikon1, sizeof(sign_nikon1))==0 ||
      memcmp(buffer, sign_nikon2, sizeof(sign_nikon2))==0 ||
      memcmp(buffer, sign_pentax, sizeof(sign_pentax))==0 )
    return tiff_diroff;
  entry=(const TIFFDirEntry *)&buffer[2];
  n=(buffer[0]<<8)+buffer[1];
#ifdef DEBUG_TIFF
  log_info("tiff_be_makernote(%lu) => %u entries\n", (long unsigned)tiff_diroff, n);
#endif
  //sizeof(TIFFDirEntry)=12;
  if(n > (unsigned)(data_read-2)/12)
    n=(data_read-2)/12;
  if(n==0)
    return -1;
  for(i=0;i<n;i++)
  {
    const uint64_t val=(uint64_t)be32(entry->tdir_count) * tiff_type2size(be16(entry->tdir_type));
#ifdef DEBUG_TIFF
    log_info("%u tag=%u(0x%x) %s type=%u count=%lu offset=%lu(0x%lx)\n",
	i,
	be16(entry->tdir_tag),
	be16(entry->tdir_tag),
	tag_name(be16(entry->tdir_tag)),
	be16(entry->tdir_type),
	(long unsigned)be32(entry->tdir_count),
	(long unsigned)be32(entry->tdir_offset),
	(long unsigned)be32(entry->tdir_offset));
#endif
    if(val>4)
    {
      const uint64_t new_offset=be32(entry->tdir_offset)+val;
      if(new_offset==0)
	return -1;
      if(max_offset < new_offset)
	max_offset=new_offset;
    }
    if(be32(entry->tdir_count)==1)
    {
      const unsigned int tmp=tiff_be_read(&entry->tdir_offset, be16(entry->tdir_type));
      switch(be16(entry->tdir_tag))
      {
	case TIFFTAG_JPEGIFOFFSET: 	jpegifoffset=tmp;	break;
	case TIFFTAG_JPEGIFBYTECOUNT:	jpegifbytecount=tmp;	break;
	case TIFFTAG_ALPHAOFFSET:	alphaoffset=tmp;	break;
	case TIFFTAG_ALPHABYTECOUNT:	alphabytecount=tmp;	break;
	case TIFFTAG_IMAGEOFFSET:	imageoffset=tmp;	break;
	case TIFFTAG_IMAGEBYTECOUNT:	imagebytecount=tmp;	break;
	case TIFFTAG_STRIPOFFSETS:	strip_offsets=tmp;	break;
	case TIFFTAG_STRIPBYTECOUNTS:	strip_bytecounts=tmp;	break;
	case TIFFTAG_TILEBYTECOUNTS:	tile_bytecounts=tmp;	break;
	case TIFFTAG_TILEOFFSETS:	tile_offsets=tmp;	break;
      }
    }
    entry++;
  }
  if(alphabytecount > 0 && max_offset < alphaoffset + alphabytecount)
    max_offset = alphaoffset + alphabytecount;
  if(imagebytecount > 0 && max_offset < imageoffset + imagebytecount)
    max_offset = imageoffset + imagebytecount;
  if(jpegifbytecount > 0 && max_offset < jpegifoffset + jpegifbytecount)
    max_offset = jpegifoffset + jpegifbytecount;
  if(strip_bytecounts > 0 && strip_offsets!=0xffffffff &&
      max_offset < strip_offsets + strip_bytecounts)
    max_offset = strip_offsets + strip_bytecounts;
  if(tile_bytecounts > 0 && tile_offsets!=0xffffffff &&
      max_offset < tile_offsets + tile_bytecounts)
    max_offset = tile_offsets + tile_bytecounts;
  return max_offset;
}
#endif

uint64_t header_check_tiff_be(file_recovery_t *fr, const uint32_t tiff_diroff, const unsigned int depth, const unsigned int count)
{
  unsigned char buffer[8192];
  unsigned int i,n;
  int data_read;
  const uint32_t *tiff_next_diroff;
  uint64_t max_offset=0;
  uint64_t alphaoffset=0;
  uint64_t alphabytecount=0;
  uint64_t imageoffset=0;
  uint64_t imagebytecount=0;
  uint64_t jpegifoffset=0;
  uint64_t jpegifbytecount=0;
  uint64_t strip_offsets=0;
  uint64_t strip_bytecounts=0;
  uint64_t tile_offsets=0;
  uint64_t tile_bytecounts=0;
  const TIFFDirEntry *entry=(const TIFFDirEntry *)&buffer[2];
  const TIFFDirEntry *entry_strip_offsets=NULL;
  const TIFFDirEntry *entry_strip_bytecounts=NULL;
  const TIFFDirEntry *entry_tile_offsets=NULL;
  const TIFFDirEntry *entry_tile_bytecounts=NULL;
#ifdef DEBUG_TIFF
  log_info("header_check_tiff_be(fr, %lu, %u, %u)\n", (long unsigned)tiff_diroff, depth, count);
#endif
  if(depth>4)
    return -1;
  if(count>16)
    return -1;
  if(tiff_diroff < sizeof(TIFFHeader))
    return -1;
  if(fseek(fr->handle, tiff_diroff, SEEK_SET) < 0)
    return -1;
  data_read=fread(buffer, 1, sizeof(buffer), fr->handle);
  if(data_read<2)
    return -1;
  n=(buffer[0]<<8)+buffer[1];
#ifdef DEBUG_TIFF
  log_info("header_check_tiff_be(fr, %lu, %u, %u) => %u entries\n", (long unsigned)tiff_diroff, depth, count, n);
#endif
  //sizeof(TIFFDirEntry)=12;
  if(n > (unsigned)(data_read-2)/12)
    n=(data_read-2)/12;
  if(n==0)
    return -1;
  for(i=0;i<n;i++)
  {
    const uint64_t val=(uint64_t)be32(entry->tdir_count) * tiff_type2size(be16(entry->tdir_type));
#ifdef DEBUG_TIFF
    log_info("%u tag=%u(0x%x) %s type=%u count=%lu offset=%lu(0x%lx) val=%lu\n",
	i,
	be16(entry->tdir_tag),
	be16(entry->tdir_tag),
	tag_name(be16(entry->tdir_tag)),
	be16(entry->tdir_type),
	(long unsigned)be32(entry->tdir_count),
	(long unsigned)be32(entry->tdir_offset),
	(long unsigned)be32(entry->tdir_offset),
	(long unsigned)val);
#endif
    if(val>4)
    {
      const uint64_t new_offset=be32(entry->tdir_offset)+val;
      if(new_offset==0)
	return -1;
      if(max_offset < new_offset)
	max_offset=new_offset;
    }
    if(be32(entry->tdir_count)==1 && val<=4)
    {
      const unsigned int tmp=tiff_be_read(&entry->tdir_offset, be16(entry->tdir_type));
      switch(be16(entry->tdir_tag))
      {
	case TIFFTAG_ALPHABYTECOUNT:	alphabytecount=tmp;	break;
	case TIFFTAG_ALPHAOFFSET:	alphaoffset=tmp;	break;
	case TIFFTAG_IMAGEBYTECOUNT:	imagebytecount=tmp;	break;
	case TIFFTAG_IMAGEOFFSET:	imageoffset=tmp;	break;
	case TIFFTAG_JPEGIFBYTECOUNT:	jpegifbytecount=tmp;	break;
	case TIFFTAG_JPEGIFOFFSET: 	jpegifoffset=tmp;	break;
	case TIFFTAG_STRIPBYTECOUNTS:	strip_bytecounts=tmp;	break;
	case TIFFTAG_STRIPOFFSETS:	strip_offsets=tmp;	break;
	case TIFFTAG_TILEBYTECOUNTS:	tile_bytecounts=tmp;	break;
	case TIFFTAG_TILEOFFSETS:	tile_offsets=tmp;	break;
	case TIFFTAG_EXIFIFD:
	case TIFFTAG_KODAKIFD:
	  {
	    const uint64_t new_offset=header_check_tiff_be(fr, tmp, depth+1, 0);
	    if(new_offset==-1)
	      return -1;
	    if(max_offset < new_offset)
	      max_offset=new_offset;
	  }
	  break;
	case TIFFTAG_SUBIFD:
	  {
	    const uint64_t new_offset=header_check_tiff_be(fr, tmp, depth+1, 0);
	    if(new_offset==-1)
	      return -1;
	    if(max_offset < new_offset)
	      max_offset=new_offset;
	  }
	  break;
#ifdef ENABLE_TIFF_MAKERNOTE
	case EXIFTAG_MAKERNOTE:
	  {
	    const uint64_t new_offset=tiff_be_makernote(fr->handle, tmp);
	    if(new_offset==-1)
	      return -1;
	    if(max_offset < new_offset)
	      max_offset=new_offset;
	  }
	  break;
#endif
      }
    }
    else if(be32(entry->tdir_count) > 1)
    {
      switch(be16(entry->tdir_tag))
      {
	case TIFFTAG_EXIFIFD:
	case TIFFTAG_KODAKIFD:
	case TIFFTAG_SUBIFD:
	  if(be16(entry->tdir_type)==4)
	  {
	    const unsigned int nbr=(be32(entry->tdir_count)<32?be32(entry->tdir_count):32);
	    unsigned int j;
	    uint32_t *subifd_offsetp;
	    if(fseek(fr->handle, be32(entry->tdir_offset), SEEK_SET) < 0)
	    {
	      return -1;
	    }
	    subifd_offsetp=(uint32_t *)MALLOC(nbr*sizeof(*subifd_offsetp));
	    if(fread(subifd_offsetp, sizeof(*subifd_offsetp), nbr, fr->handle) != nbr)
	    {
	      free(subifd_offsetp);
	      return -1;
	    }
	    for(j=0; j<nbr; j++)
	    {
	      const uint64_t new_offset=header_check_tiff_be(fr, be32(subifd_offsetp[j]), depth+1, 0);
	      if(new_offset==-1)
	      {
		free(subifd_offsetp);
		return -1;
	      }
	      if(max_offset < new_offset)
		max_offset = new_offset;
	    }
	    free(subifd_offsetp);
	  }
	  break;
	case TIFFTAG_STRIPOFFSETS:
	  entry_strip_offsets=entry;
	  break;
	case TIFFTAG_STRIPBYTECOUNTS:
	  entry_strip_bytecounts=entry;
	  break;
	case TIFFTAG_TILEBYTECOUNTS:
	  entry_tile_bytecounts=entry;
	  break;
	case TIFFTAG_TILEOFFSETS:
	  entry_tile_offsets=entry;
	  break;
      }
    }
    entry++;
  }
  if(alphabytecount > 0 && max_offset < alphaoffset + alphabytecount)
    max_offset = alphaoffset + alphabytecount;
  if(imagebytecount > 0 && max_offset < imageoffset + imagebytecount)
    max_offset = imageoffset + imagebytecount;
  if(jpegifbytecount > 0 && max_offset < jpegifoffset + jpegifbytecount)
    max_offset = jpegifoffset + jpegifbytecount;
  if(strip_bytecounts > 0 && strip_offsets!=0xffffffff &&
      max_offset < strip_offsets + strip_bytecounts)
    max_offset = strip_offsets + strip_bytecounts;
  if(tile_bytecounts > 0 && tile_offsets!=0xffffffff &&
      max_offset < tile_offsets + tile_bytecounts)
    max_offset = tile_offsets + tile_bytecounts;
  if(entry_strip_offsets != NULL && entry_strip_bytecounts != NULL)
  {
    const uint64_t tmp=parse_strip_be(fr->handle, entry_strip_offsets, entry_strip_bytecounts);
    if(tmp==-1)
      return -1;
    if(max_offset < tmp)
      max_offset=tmp;
  }
  if(entry_tile_offsets != NULL && entry_tile_bytecounts != NULL)
  {
    const uint64_t tmp=parse_strip_be(fr->handle, entry_tile_offsets, entry_tile_bytecounts);
    if(tmp==-1)
      return -1;
    if(max_offset < tmp)
      max_offset=tmp;
  }
  tiff_next_diroff=(const uint32_t *)entry;
  if(be32(*tiff_next_diroff) > 0)
  {
    const uint64_t new_offset=header_check_tiff_be(fr, be32(*tiff_next_diroff), depth, count+1);
    if(new_offset != -1 && max_offset < new_offset)
      max_offset=new_offset;
  }
  return max_offset;
}

int header_check_tiff_be_new(const unsigned char *buffer, const unsigned int buffer_size, const unsigned int safe_header_only, const file_recovery_t *file_recovery, file_recovery_t *file_recovery_new)
{
  const char *potential_error=NULL;
  const TIFFHeader *header=(const TIFFHeader *)buffer;
  if((uint32_t)be32(header->tiff_diroff) < sizeof(TIFFHeader))
    return 0;
  if(file_recovery->file_stat!=NULL &&
      file_recovery->file_stat->file_hint==&file_hint_jpg)
  {
    if(header_ignored_adv(file_recovery, file_recovery_new)==0)
      return 0;
  }
  reset_file_recovery(file_recovery_new);
  file_recovery_new->extension="tif";
  if(find_tag_from_tiff_header_be(header, buffer_size, TIFFTAG_DNGVERSION, &potential_error)!=NULL)
  {
    /* Adobe Digital Negative, ie. PENTAX K-30 */
    file_recovery_new->extension="dng";
  }
  else
  {
    const char *tag_make;
    tag_make=find_tag_from_tiff_header_be(header, buffer_size, TIFFTAG_MAKE, &potential_error);
    if(tag_make!=NULL && tag_make >= (const char *)buffer && tag_make < (const char *)buffer + buffer_size - 20)
    {
      if(strcmp(tag_make, "PENTAX Corporation ")==0 ||
	  strcmp(tag_make, "PENTAX             ")==0)
	file_recovery_new->extension="pef";
      else if(strcmp(tag_make, "NIKON CORPORATION")==0)
	file_recovery_new->extension="nef";
      else if(strcmp(tag_make, "Kodak")==0)
	file_recovery_new->extension="dcr";
    }
  }
  file_recovery_new->time=get_date_from_tiff_header(header, buffer_size);
  file_recovery_new->file_check=&file_check_tiff;
  return 1;
}
