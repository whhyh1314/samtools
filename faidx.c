/*  faidx.c -- faidx subcommand.

    Copyright (C) 2008, 2009, 2013, 2016, 2018 Genome Research Ltd.
    Portions copyright (C) 2011 Broad Institute.

    Author: Heng Li <lh3@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

History:

  * 2016-01-12: Pierre Lindenbaum @yokofakun : added options -o -n

*/

#include <config.h>

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <htslib/faidx.h>
#include <htslib/hts.h>
#include <htslib/hfile.h>
#include <htslib/kstring.h>
#include "samtools.h"

#define DEFAULT_FASTA_LINE_LEN 60


static int write_line(FILE *file, const char *line, const char *name, const int ignore,
                      const int length, const int seq_len) {
    int beg, end;

    if (seq_len < 0) {
        fprintf(stderr, "[faidx] Failed to fetch sequence in %s\n", name);

        if (ignore && seq_len == -2) {
            return EXIT_SUCCESS;
        } else {
            return EXIT_FAILURE;
        }
    } else if (seq_len == 0) {
        fprintf(stderr, "[faidx] Zero length sequence: %s\n", name);
    } else if (hts_parse_reg(name, &beg, &end) && (end < INT_MAX) && (seq_len != end - beg)) {
        fprintf(stderr, "[faidx] Truncated sequence: %s\n", name);
    }

    size_t i, seq_sz = seq_len;

    for (i = 0; i < seq_sz; i += length)
    {
        size_t len = i + length < seq_sz ? length : seq_sz - i;
        if (fwrite(line + i, 1, len, file) < len ||
            fputc('\n', file) == EOF) {
            print_error_errno("faidx", "failed to write output");
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}


static int write_output(faidx_t *faid, FILE *file, const char *name, const int ignore,
                        const int length, enum fai_format_options format) {
    int seq_len;
    char *seq = fai_fetch(faid, name, &seq_len);

    if (format == FAI_FASTA) {
        fprintf(file, ">%s\n", name);
    } else {
        fprintf(file, "@%s\n", name);
    }

    if (write_line(file, seq, name, ignore, length, seq_len) == EXIT_FAILURE) {
        free(seq);
        return EXIT_FAILURE;
    }

    free(seq);

    if (format == FAI_FASTQ) {
        fprintf(file, "+\n");

        char *qual = fai_fetchqual(faid, name, &seq_len);

        if (write_line(file, qual, name, ignore, length, seq_len) == EXIT_FAILURE) {
            free(seq);
            return EXIT_FAILURE;
        }

        free(qual);
    }

    return EXIT_SUCCESS;
}


static int read_regions_from_file(faidx_t *faid, hFILE *in_file, FILE *file, const int ignore,
                                  const int length, enum fai_format_options format) {
    kstring_t line = {0, 0, NULL};
    int ret = EXIT_FAILURE;

    while (line.l = 0, kgetline(&line, (kgets_func *)hgets, in_file) >= 0) {
        if ((ret = write_output(faid, file, line.s, ignore, length, format)) == EXIT_FAILURE) {
            break;
        }
    }

    free(line.s);

    return ret;
}


static int usage(FILE *fp, enum fai_format_options format, int exit_status)
{
    char *tool, *file_type;

    if (format == FAI_FASTA) {
        tool = "faidx <file.fa|file.fa.gz>";
        file_type = "FASTA";
    } else {
        tool = "fqidx <file.fq|file.fq.gz>";
        file_type = "FASTQ";
    }

    fprintf(fp, "Usage: samtools %s [<reg> [...]]\n", tool);
    fprintf(fp, "Option: \n"
                " -o, --output      FILE Write %s to file.\n"
                " -n, --length      INT  Length of %s sequence line. [60]\n"
                " -c, --continue         Continue after trying to retrieve missing region.\n"
                " -r, --region-file FILE File of regions.  Format is chr:from-to. One per line.\n",
                file_type, file_type);

    if (format == FAI_FASTA) {
       fprintf(fp, " -f, --fastq            File and index in FASTQ format.\n");
    }

    fprintf(fp, " -h, --help             This message.\n");

    return exit_status;
}


int faidx_core(int argc, char *argv[], enum fai_format_options format)
{
    int c, ignore_error = 0;
    int line_len = DEFAULT_FASTA_LINE_LEN ;/* fasta line len */
    char* output_file = NULL; /* output file (default is stdout ) */
    char *region_file = NULL; // list of regions from file, one per line
    FILE* file_out = stdout;/* output stream */

    static const struct option lopts[] = {
        { "output", required_argument,      NULL, 'o' },
        { "help",   no_argument,            NULL, 'h' },
        { "length", required_argument,      NULL, 'n' },
        { "continue", no_argument,          NULL, 'c' },
        { "region-file", required_argument, NULL, 'r' },
        { "fastq", no_argument,             NULL, 'f' },
        { NULL, 0, NULL, 0 }
    };

    while ((c = getopt_long(argc, argv, "ho:n:cr:f", lopts, NULL)) >= 0) {
        switch (c) {
            case 'o': output_file = optarg; break;
            case 'n': line_len = atoi(optarg);
                      if(line_len<1) {
                        fprintf(stderr,"[faidx] bad line length '%s', using default:%d\n",optarg,DEFAULT_FASTA_LINE_LEN);
                        line_len= DEFAULT_FASTA_LINE_LEN ;
                        }
                      break;
            case 'c': ignore_error = 1; break;
            case 'r': region_file = optarg; break;
            case 'f': format = FAI_FASTQ; break;
            case '?': return usage(stderr, format, EXIT_FAILURE);
            case 'h': return usage(stdout, format, EXIT_SUCCESS);
            default:  break;
        }
    }

    if ( argc==optind )
        return usage(stdout, format, EXIT_SUCCESS);

    if ( optind+1 == argc && !region_file)
    {
        if (fai_build(argv[optind]) != 0) {
            fprintf(stderr, "[faidx] Could not build fai index %s.fai\n", argv[optind]);
            return EXIT_FAILURE;
        }
        return 0;
    }

    faidx_t *fai = fai_load_format(argv[optind], format);

    if ( !fai ) {
        fprintf(stderr, "[faidx] Could not load fai index of %s\n", argv[optind]);
        return EXIT_FAILURE;
    }

    /** output file provided by user */
    if( output_file != NULL ) {
        if( strcmp( output_file, argv[optind] ) == 0 ) {
            fprintf(stderr,"[faidx] Same input/output : %s\n", output_file);
            return EXIT_FAILURE;
        }

        file_out = fopen( output_file, "w" );

        if( file_out == NULL) {
            fprintf(stderr,"[faidx] Cannot open \"%s\" for writing :%s.\n", output_file, strerror(errno) );
            return EXIT_FAILURE;
        }
    }

    int exit_status = EXIT_SUCCESS;

    if (region_file) {
        hFILE *rf;

        if ((rf = hopen(region_file, "r"))) {
            exit_status = read_regions_from_file(fai, rf, file_out, ignore_error, line_len, format);

            if (hclose(rf) != 0) {
                fprintf(stderr, "[faidx] Warning: failed to close %s", region_file);
            }
        } else {
            fprintf(stderr, "[faidx] Failed to open \"%s\" for reading.\n", region_file);
            exit_status = EXIT_FAILURE;
        }
    }

    while ( ++optind<argc && exit_status == EXIT_SUCCESS) {
        exit_status = write_output(fai, file_out, argv[optind], ignore_error, line_len, format);
    }

    fai_destroy(fai);

    if (fflush(file_out) == EOF) {
        print_error_errno("faidx", "failed to flush output");
        exit_status = EXIT_FAILURE;
    }

    if( output_file != NULL) fclose(file_out);

    return exit_status;
}


int faidx_main(int argc, char *argv[]) {
    return faidx_core(argc, argv, FAI_FASTA);
}


int fqidx_main(int argc, char *argv[]) {
    return faidx_core(argc, argv, FAI_FASTQ);
}

