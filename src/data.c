/* data.c -- 
 * Created: Tue Jul 16 12:45:41 1996 by faith@dict.org
 * Copyright 1996, 1997, 1998, 2000, 2002 Rickard E. Faith (faith@dict.org)
 * Copyright 2002-2008 Aleksey Cheusov (vle@gmx.net)
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 1, or (at your option) any
 * later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include "data.h"
#include "dictzip.h"

#include <sys/stat.h>

#include <fcntl.h>
#include <assert.h>

#define USE_CACHE 1


int mmap_mode = 0;

static int dict_read_header(const char *filename, dictData *header, int computeCRC) {
    FILE          *str;
    int           id1, id2, si1, si2;
    int           customV2 = 0;
    char          buffer[BUFFERSIZE];
    int           extraLength, subLength;
    int           i;
    char          *pt;
    int           c;
    struct stat   sb;
    unsigned long crc   = crc32( 0L, Z_NULL, 0 );
    int           count;
    unsigned long offset;

    if (!(str = fopen(filename, "rb")))
        err_fatal_errno(__func__,
                        "Cannot open data file \"%s\" for read\n", filename);

    header->filename = filename; // str_find( filename );
    header->headerLength = GZ_XLEN - 1;
    header->type = DICT_UNKNOWN;

    id1 = getc(str);
    id2 = getc(str);

    if (id1 == 'D' && id2 == 'Z') {
        int m1 = getc(str), m2 = getc(str);
        int v0 = getc(str), v1 = getc(str);
        unsigned long long chunkCount = 0, sourceSize = 0;
        unsigned long chunkSize = 0;
        if (m1 != 'V' || m2 != '2' || v0 != 2 || v1 != 0) {
            fclose(str); return 5;
        }
        for (i = 0; i < 4; ++i) {
            int b = getc(str); if (b == EOF) { fclose(str); return 5; }
            chunkSize |= ((unsigned long)b) << (8 * i);
        }
        for (i = 0; i < 8; ++i) {
            int b = getc(str); if (b == EOF) { fclose(str); return 5; }
            chunkCount |= ((unsigned long long)b) << (8 * i);
        }
        for (i = 0; i < 8; ++i) {
            int b = getc(str); if (b == EOF) { fclose(str); return 5; }
            sourceSize |= ((unsigned long long)b) << (8 * i);
        }
        if (!chunkSize || !chunkCount || chunkCount > 10000000ULL || chunkSize > 0x7fffffffUL) {
            fclose(str); return 5;
        }
        header->chunkLength = (int)chunkSize;
        header->chunkCount = (int)chunkCount;
        header->length = sourceSize;
        header->version = 2;
        header->type = DICT_DZIP;
        header->chunks = malloc(sizeof(header->chunks[0]) * header->chunkCount);
        if (!header->chunks) { fclose(str); return 5; }
        for (i = 0; i < header->chunkCount; ++i) {
            unsigned long value = 0;
            for (int b = 0; b < 4; ++b) {
                int byte = getc(str); if (byte == EOF) { fclose(str); return 5; }
                value |= ((unsigned long)byte) << (b * 8);
            }
            if (value > 0x7fffffffUL) { fclose(str); return 5; }
            header->chunks[i] = (int)value;
        }
        header->headerLength = 26 + header->chunkCount * 4 + 10 - 1;
        if (getc(str) != GZ_MAGIC1 || getc(str) != GZ_MAGIC2 ||
            getc(str) != Z_DEFLATED || getc(str) != 0) { fclose(str); return 5; }
        if (fseek(str, 6, SEEK_CUR)) { fclose(str); return 5; }
        customV2 = 1;
        goto header_parsed;
    }

    if (id1 != GZ_MAGIC1 || id2 != GZ_MAGIC2) {
        header->type = DICT_TEXT;
        fstat(_fileno(str), &sb);
        header->compressedLength = header->length = sb.st_size;
        header->origFilename = filename; // str_find( filename );
        header->mtime = sb.st_mtime;
        if (computeCRC) {
            rewind(str);
            while (!feof(str)) {
                if ((count = fread(buffer, 1, BUFFERSIZE, str))) {
                    crc = crc32(crc, (Bytef *) buffer, count);
                }
            }
        }
        header->crc = crc;
        fclose(str);
        return 0;
    }
    header->type = DICT_GZIP;

    header->method = getc(str);
    header->flags = getc(str);
    header->mtime = getc(str) << 0;
    header->mtime |= getc(str) << 8;
    header->mtime |= getc(str) << 16;
    header->mtime |= getc(str) << 24;
    header->extraFlags = getc(str);
    header->os = getc(str);

    if (header->flags & GZ_FEXTRA) {
        extraLength = getc(str) << 0;
        extraLength |= getc(str) << 8;
        header->headerLength += extraLength + 2;
        si1 = getc(str);
        si2 = getc(str);

        if (si1 == GZ_RND_S1 && (si2 == GZ_RND_S2 || si2 == 'Z')) {
            subLength = getc(str) << 0;
            subLength |= getc(str) << 8;
            if (subLength < 6 || (unsigned long)subLength + 4 > (unsigned long)extraLength)
                err_fatal(__func__, "invalid dictzip extra field\n");
            header->version = getc(str) << 0;
            header->version |= getc(str) << 8;

            if (header->version != (si2 == 'Z' ? 2 : 1))
                err_internal(__func__,
                             "dzip header version %d not supported\n",
                             header->version);

            if (si2 == 'Z') {
                header->chunkLength = getc(str) << 0;
                header->chunkLength |= getc(str) << 8;
                int countBytes[4];
                for (int b = 0; b < 4; ++b) {
                    countBytes[b] = getc(str);
                    if (countBytes[b] == EOF) { fclose(str); return 5; }
                }
                header->chunkCount = countBytes[0] | (countBytes[1] << 8) |
                                     (countBytes[2] << 16) | (countBytes[3] << 24);
            } else {
                header->chunkLength = getc(str) << 0;
                header->chunkLength |= getc(str) << 8;
                header->chunkCount = getc(str) << 0;
                header->chunkCount |= getc(str) << 8;
            }

            if (header->chunkCount <= 0 ||
                2 + (si2 == 'Z' ? 2 + 4 : 2 + 2) +
                (unsigned long long)header->chunkCount * (si2 == 'Z' ? 4 : 2) +
                (si2 == 'Z' ? 8 : 0) != (unsigned long long)subLength) {
                fclose(str);
                return 5;
            }
            header->chunks = malloc(sizeof(header->chunks[0])
                                    * header->chunkCount);
            if (si2 == 'Z') {
                unsigned long long sourceSize = 0;
                for (i = 0; i < header->chunkCount; ++i) {
                    unsigned long value = 0;
                    for (int b = 0; b < 4; ++b) {
                        int byte = getc(str);
                        if (byte == EOF) { fclose(str); return 5; }
                        value |= ((unsigned long)byte) << (b * 8);
                    }
                    if (value > 0x7fffffffUL) { fclose(str); return 5; }
                    header->chunks[i] = (int)value;
                }
                for (int b = 0; b < 8; ++b) {
                    int byte = getc(str);
                    if (byte == EOF) {
                        fclose(str);
                        return 5;
                    }
                    sourceSize |= ((unsigned long long)byte) << (b * 8);
                }
                header->length = sourceSize;
            } else {
                for (i = 0; i < header->chunkCount; i++) {
                    header->chunks[i] = getc(str) << 0;
                    header->chunks[i] |= getc(str) << 8;
                }
            }
            header->type = DICT_DZIP;
        } else {
            fseek(str, header->headerLength, SEEK_SET);
        }
    }

    if (header->flags & GZ_FNAME) { /* FIXME! Add checking against header len */
        pt = buffer;
        while ((c = getc(str)) && c != EOF) {
            *pt++ = c;

            if (pt == buffer + sizeof(buffer)) {
                err_fatal(
                        __func__,
                        "too long FNAME field in dzip file \"%s\"\n", filename);
            }
        }

        *pt = '\0';
        header->origFilename = buffer; // str_find( buffer );
        header->headerLength += strlen(header->origFilename) + 1;
    } else {
        header->origFilename = NULL;
    }

    if (header->flags & GZ_COMMENT) { /* FIXME! Add checking for header len */
        pt = buffer;
        while ((c = getc(str)) && c != EOF) {
            *pt++ = c;

            if (pt == buffer + sizeof(buffer)) {
                err_fatal(
                        __func__,
                        "too long COMMENT field in dzip file \"%s\"\n", filename);
            }
        }

        *pt = '\0';
        header->comment = NULL; // str_find( buffer );
        header->headerLength += strlen(header->comment) + 1;
    } else {
        header->comment = NULL;
    }

    if (header->flags & GZ_FHCRC) {
        getc(str);
        getc(str);
        header->headerLength += 2;
    }

    if (ftell(str) != header->headerLength + 1)
        err_internal(__func__,
                     "File position (%lu) != header length + 1 (%d)\n",
                     ftell(str), header->headerLength + 1);

header_parsed:
    fseek(str, -8, SEEK_END);
    header->crc = getc(str) << 0;
    header->crc |= getc(str) << 8;
    header->crc |= getc(str) << 16;
    header->crc |= getc(str) << 24;
    if (!customV2 && header->version != 2) {
        header->length = getc(str) << 0;
        header->length |= getc(str) << 8;
        header->length |= getc(str) << 16;
        header->length |= getc(str) << 24;
    }
    header->compressedLength = ftell(str);

    /* Compute offsets */
    header->offsets = malloc(sizeof(header->offsets[0])
                             * header->chunkCount);
    for (offset = header->headerLength + 1, i = 0;
         i < header->chunkCount;
         i++) {
        header->offsets[i] = offset;
        offset += header->chunks[i];
    }

    fclose(str);
    return 0;
}

dictData *dict_data_open(const char *filename, int computeCRC) {
    dictData *h = NULL;
    struct stat sb;
    int j;

    if (!filename)
        return NULL;

    h = malloc(sizeof(struct dictData));

    memset(h, 0, sizeof(struct dictData));
    h->initialized = 0;

    if (stat(filename, &sb) || !S_ISREG(sb.st_mode)) {
        err_warning(__func__,
                    "%s is not a regular file -- ignoring\n", filename);
        return h;
    }

    if (dict_read_header(filename, h, computeCRC)) {
        err_fatal(__func__,
                  "\"%s\" not in text or dzip format\n", filename);
    }

    if ((h->fd = _open(filename, O_RDONLY)) < 0)
        err_fatal_errno(__func__,
                        "Cannot open data file \"%s\"\n", filename);
    if (fstat(h->fd, &sb))
        err_fatal_errno(__func__,
                        "Cannot stat data file \"%s\"\n", filename);
    h->size = sb.st_size;

    if (mmap_mode) {
        err_fatal(__func__, "This should not happen");
    } else {
        h->start = malloc(h->size);
        if (-1 == _read(h->fd, (char *) h->start, h->size))
            err_fatal_errno(
                    __func__,
                    "Cannot read data file \"%s\"\n", filename);

        _close(h->fd);
        h->fd = 0;
    }

    h->end = h->start + h->size;

    for (j = 0; j < DICT_CACHE_SIZE; j++) {
        h->cache[j].chunk = -1;
        h->cache[j].stamp = -1;
        h->cache[j].inBuffer = NULL;
        h->cache[j].count = 0;
    }

    return h;
}

void dict_data_close(dictData *header) {
    int i;

    if (!header)
        return;

    if (header->fd >= 0) {
        if (mmap_mode) {
            err_fatal(__func__, "This should not happen");
        } else {
            if (header->start)
                free((char *) header->start);
        }
    }

    if (header->chunks)     free(header->chunks);
    if (header->offsets)    free(header->offsets);

    if (header->initialized) {
        if (inflateEnd(&header->zStream))
            err_internal(__func__,
                         "Cannot shut down inflation engine: %s\n",
                         header->zStream.msg);
    }

    for (i = 0; i < DICT_CACHE_SIZE; ++i) {
        if (header->cache[i].inBuffer)
            free(header->cache[i].inBuffer);
    }

    memset(header, 0, sizeof(struct dictData));
    free(header);
}

char *dict_data_read_(
        dictData *h, unsigned long start, unsigned long size,
        const char *preFilter, const char *postFilter) {
    char          *buffer, *pt;
    unsigned long end;
    int           count;
    char          *inBuffer;
    int           firstChunk, lastChunk;
    int           firstOffset, lastOffset;
    int           i, j;
    int           found, target, lastStamp;
    static int    stamp = 0;

    end  = start + size;

    buffer = malloc( size + 1 );

    PRINTF(DBG_UNZIP,
           ("dict_data_read( %p, %lu, %lu, %s, %s )\n",
                   h, start, size, preFilter, postFilter));

    assert(h != NULL);
    switch (h->type) {
        case DICT_GZIP:
            err_fatal(__func__,
                      "Cannot seek on pure gzip format files.\n"
                      "Use plain text (for performance)"
                      " or dzip format (for space savings).\n");
            break;
        case DICT_TEXT:
            memcpy(buffer, h->start + start, size);
            buffer[size] = '\0';
            break;
        case DICT_DZIP:
            if (!h->initialized) {
                ++h->initialized;
                h->zStream.zalloc    = NULL;
                h->zStream.zfree     = NULL;
                h->zStream.opaque    = NULL;
                h->zStream.next_in   = 0;
                h->zStream.avail_in  = 0;
                h->zStream.next_out  = NULL;
                h->zStream.avail_out = 0;
                if (inflateInit2(&h->zStream, -15) != Z_OK)
                    err_internal(__func__,
                                 "Cannot initialize inflation engine: %s\n",
                                 h->zStream.msg);
            }
            firstChunk  = start / h->chunkLength;
            firstOffset = start - firstChunk * h->chunkLength;
            lastChunk   = (end - 1) / h->chunkLength;
            if (lastChunk < firstChunk) {
                lastChunk = firstChunk;
            }
            lastOffset = end - lastChunk * h->chunkLength;
            PRINTF(DBG_UNZIP,
                   ("   start = %lu, end = %lu\n"
                    "firstChunk = %d, firstOffset = %d,"
                    " lastChunk = %d, lastOffset = %d\n",
                           start, end, firstChunk, firstOffset, lastChunk, lastOffset));
            for (pt = buffer, i = firstChunk; i <= lastChunk; i++) {

                /* Access cache */
                found = 0;
                target = 0;
                lastStamp = INT_MAX;
                for (j = 0; j < DICT_CACHE_SIZE; j++) {
#if USE_CACHE
                    if (h->cache[j].chunk == i) {
                        found = 1;
                        target = j;
                        break;
                    }
#endif
                    if (h->cache[j].stamp < lastStamp) {
                        lastStamp = h->cache[j].stamp;
                        target = j;
                    }
                }

                h->cache[target].stamp = ++stamp;
                if (found) {
                    count = h->cache[target].count;
                    inBuffer = h->cache[target].inBuffer;
                } else {
                    h->cache[target].chunk = i;
                    if (!h->cache[target].inBuffer)
                        h->cache[target].inBuffer = malloc(
                                h->chunkLength > IN_BUFFER_SIZE ? h->chunkLength : IN_BUFFER_SIZE);
                    inBuffer = h->cache[target].inBuffer;

                    h->zStream.next_in   = (Bytef *)(h->start + h->offsets[i]);
                    h->zStream.avail_in  = h->chunks[i];
                    h->zStream.next_out  = (Bytef *) inBuffer;
                    h->zStream.avail_out = h->chunkLength > IN_BUFFER_SIZE
                                            ? h->chunkLength : IN_BUFFER_SIZE;
                    if (inflate(&h->zStream, Z_PARTIAL_FLUSH) != Z_OK)
                        err_fatal(__func__, "inflate: %s\n", h->zStream.msg);
                    if (h->zStream.avail_in)
                        err_internal(__func__,
                                     "inflate did not flush (%d pending, %d avail)\n",
                                     h->zStream.avail_in, h->zStream.avail_out);

                    count = (h->chunkLength > IN_BUFFER_SIZE
                             ? h->chunkLength : IN_BUFFER_SIZE) - h->zStream.avail_out;

                    h->cache[target].count = count;
                }

                if (i == firstChunk) {
                    if (i == lastChunk) {
                        memcpy(pt, inBuffer + firstOffset, lastOffset - firstOffset);
                        pt += lastOffset - firstOffset;
                    } else {
                        if (count != h->chunkLength)
                            err_internal(__func__,
                                         "Length = %d instead of %d\n",
                                         count, h->chunkLength);
                        memcpy(pt, inBuffer + firstOffset,
                               h->chunkLength - firstOffset);
                        pt += h->chunkLength - firstOffset;
                    }
                } else if (i == lastChunk) {
                    memcpy(pt, inBuffer, lastOffset);
                    pt += lastOffset;
                } else {
                    assert(count == h->chunkLength);
                    memcpy(pt, inBuffer, h->chunkLength);
                    pt += h->chunkLength;
                }
            }
            *pt = '\0';
            break;
        case DICT_UNKNOWN:
            err_fatal(__func__, "Cannot read unknown file type\n");
            break;
    }

    return buffer;
}
