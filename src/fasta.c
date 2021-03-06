/*
 * GenomeTester4
 *
 * A toolkit for creating and manipulating k-mer lists from biological sequences
 * 
 * Copyright (C) 2014 University of Tartu
 *
 * Authors: Maarja Lepamets and Lauris Kaplinski
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "buffer.h"
#include "fasta.h"
#include "sequence.h"
#include "utils.h"
#include "common.h"

static unsigned int *c2n = NULL;

int
fasta_reader_init (FastaReader *reader, unsigned int wordlength, unsigned int canonize, int (* read) (void *), void *read_data)
{
  memset (reader, 0, sizeof (FastaReader));
  reader->wordlength = wordlength;
  reader->mask = create_mask (wordlength);
  reader->canonize = canonize;
  reader->read = read;
  reader->read_data = read_data;
  if (!c2n) {
    c2n = (unsigned int *) malloc (256 * sizeof (unsigned int));
    memset (c2n, 0xff, 256 * sizeof (unsigned int));
    c2n['A'] = c2n['a'] = 0;
    c2n['C'] = c2n['c'] = 1;
    c2n['G'] = c2n['g'] = 2;
    c2n['T'] = c2n['t'] = c2n['U'] = c2n['u'] = 3;
  }
  return 0;
}

int
fasta_reader_release (FastaReader *reader)
{
  if (reader->free_io_data) {
    reader->free_io_data (reader->read_data);
  }
  return 0;
}

struct BufferData {
	const unsigned char *cdata;
	unsigned long long csize;
	unsigned long long cpos;
};

static int
buffer_read (void *data)
{
	struct BufferData *bdata = (struct BufferData *) data;
	if (bdata->cpos >= bdata->csize) return 0;
	return bdata->cdata[bdata->cpos++];
}

static void
buffer_free (void *data)
{
	free (data);
}

int
fasta_reader_init_from_data (FastaReader *reader, unsigned int wordlength, unsigned int canonize, const unsigned char *cdata, unsigned long long csize)
{
	struct BufferData *bdata = (struct BufferData *) malloc (sizeof (struct BufferData));
	int result;
	bdata->cdata = cdata;
	bdata->csize = csize;
	bdata->cpos = 0;
	result = fasta_reader_init (reader, wordlength, canonize, buffer_read, bdata);
	reader->free_io_data = buffer_free;
	return result;
}

static int
file_read (void *data)
{
	FILE *ifs = (FILE *) data;
	int cval = fgetc (ifs);
	if (cval == EOF) return 0;
	return cval;
}

int
fasta_reader_init_from_file (FastaReader *reader, unsigned int wordlength, unsigned int canonize, FILE *ifs)
{
	int result;
	result = fasta_reader_init (reader, wordlength, canonize, file_read, ifs);
	return result;
}


int
fasta_reader_read_nwords (FastaReader *reader, unsigned long long maxwords,
  int (*start_sequence) (FastaReader *, void *),
  int (*end_sequence) (FastaReader *, void *),
  int (*read_character) (FastaReader *, unsigned int character, void *),
  int (*read_nucleotide) (FastaReader *, unsigned int nucleotide, void *),
  int (*read_word) (FastaReader *, unsigned long long word, void *),
  void *data)
{
  unsigned long long nwords = 0;

  while (!reader->in_eof && (nwords < maxwords)) {
    int cval = reader->read (reader->read_data);
    /* Read error */
    if (cval < 0) return cval;
    /* EOF */
    if (cval == 0) {
      reader->in_eof = 1;
      if ((reader->state == FASTA_READER_STATE_SEQUENCE) && end_sequence) {
        int result = end_sequence (reader, data);
	if (result) return result;
      }
      return 0;
    }
    /* Valid character */
    if (read_character) {
      int result = read_character (reader, cval, data);
      if (result) return result;
    }
    switch (reader->state) {
    case FASTA_READER_STATE_NONE:
      /* Start of file */
      if (cval == '>') {
      	reader->type = GT4FR_FASTA;
      } else if (cval == '@') {
        reader->type = GT4FR_FASTQ;
      } else {
        /* Invalid */
        return -1;
      }
      reader->state = FASTA_READER_STATE_NAME;
      reader->name_pos = reader->cpos;
      reader->name_length = 0;
      reader->cpos += 1;
      break;
    case FASTA_READER_STATE_NAME:
      if (cval == '\n') {
        unsigned int nlen;
        /* Name is complete */
        nlen = reader->name_length;
        if (nlen >= MAX_NAME_SIZE) nlen = MAX_NAME_SIZE;
        reader->name[nlen] = 0;
        /* Set up parsing state */
	reader->state = FASTA_READER_STATE_SEQUENCE;
	reader->wordfw = 0;
	reader->wordrv = 0;
	reader->currentlength = 0;
	/* Call start sequence from the next position */
	reader->cpos += 1;
        if (start_sequence) {
	  int result = start_sequence (reader, data);
	  if (result) return result;
	}
      } else {
	/* Append character to name */
	if (reader->name_length < MAX_NAME_SIZE) {
	  reader->name[reader->name_length] = cval;
	}
	reader->name_length += 1;
	reader->cpos += 1;
      }
      break;
    case FASTA_READER_STATE_SEQUENCE:
      if ((reader->type == GT4FR_FASTA) && (cval == '>')) {
	/* End of FastA sequence */
	if (end_sequence) {
	  int result = end_sequence (reader, data);
	  if (result) return result;
	}
	/* Start new name */
	reader->state = FASTA_READER_STATE_NAME;
	reader->seq_npos = 0;
	reader->name_pos = reader->cpos;
	reader->name_length = 0;
      } else if ((reader->type == GT4FR_FASTQ) && (cval == '\n')) {
      	/* End of FastQ sequence */
      	if (end_sequence) {
	  int result = end_sequence (reader, data);
	  if (result) return result;
	}
	/* Next characters should be '+\n' */
	cval = reader->read (reader->read_data);
	if (cval != '+') return -1;
	reader->cpos += 1;
	cval = reader->read (reader->read_data);
	if (cval != '\n') return -1;
	reader->cpos += 1;
	reader->state = FASTA_READER_STATE_QUALITY;
      } else {
	/* Process */
	unsigned int nuclval = c2n[cval];
	if (nuclval <= 3) {
	  /* Is nucleotide */
	  if (read_nucleotide) {
	    int result = read_nucleotide (reader, nuclval, data);
	    if (result) return result;
	  }
	  reader->wordfw <<= 2;
	  reader->wordfw |= nuclval;
	  if (reader->canonize) {
	    reader->wordrv >>= 2;
	    reader->wordrv |= ((unsigned long long) (~nuclval & 3) << ((reader->wordlength - 1) * 2));
	  }
	  reader->currentlength += 1;
	  if (reader->currentlength > reader->wordlength) {
	    reader->wordfw &= reader->mask;
	    reader->currentlength = reader->wordlength;
	  }
	  if (reader->currentlength == reader->wordlength) {
	    /* Update current word */
	    unsigned long long word = (!reader->canonize || reader->wordfw < reader->wordrv) ? reader->wordfw : reader->wordrv;
	    if (read_word) {
	      int result = read_word (reader, word, data);
	      if (result) return result;
	    }
	    reader->wpos += 1;
	    nwords += 1;
	  }
	  /* We increase nucleotide position for N too */
	  if (cval > ' ') {
	    reader->seq_npos += 1;
	  }
	} else if (cval >= ' ') {
	  reader->wordfw = 0;
	  reader->wordrv = 0;
	  reader->currentlength = 0;
	}
      }
      reader->cpos += 1;
      break;
    case FASTA_READER_STATE_QUALITY:
      if (cval == '\n') {
      	/* End of quality, next should be EOF or '@' */
      	cval = reader->read (reader->read_data);
      	if (cval == 0) return 0;
      	if (cval != '@') return -1;
      	reader->cpos += 1;
	reader->state = FASTA_READER_STATE_NAME;
	reader->seq_npos = 0;
	reader->name_pos = reader->cpos;
	reader->name_length = 0;
      }
      reader->cpos += 1;
      break;
    }
  }
  return 0;
}
                                                
