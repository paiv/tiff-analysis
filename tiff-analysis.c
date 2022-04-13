// cc -O2 -o tiff-analysis tiff-analysis.c
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


#define APP_EXTRACT_STRIPS 1
#define APP_DECOMPRESS 1


typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef float r32;

#ifndef __LITTLE_ENDIAN__
#error "This code is limited to little endian"
#endif

#define read16(a) (*((u16*)(a)))
#define read32(a) (*((u32*)(a)))


typedef struct __attribute__((__packed__)) {
    u16 tag;
    u16 type;
    u32 count;
    u32 value;
} tiff_ifd_entry;


typedef struct __attribute__((__packed__)) {
    u32 num;
    u32 den;
} tiff_rational;


static u32
value_u32(const tiff_ifd_entry* entry) {
    switch (entry->type) {
        case 1: return entry->value;
        case 3: return entry->value;
        case 4: return entry->value;
        default: return 0;
    }
}


static u32
value_u32a(const u8*mem, const tiff_ifd_entry* entry, u32 index) {
    if (entry->count == 1) {
        if (index == 0) {
            switch (entry->type) {
                case 1: return entry->value;
                case 3: return entry->value;
                case 4: return entry->value;
            }
        }
        return 0;
    }
    switch (entry->type) {
        case 3: return read16(mem + entry->value + index * sizeof(u16));
        case 4: return read32(mem + entry->value + index * sizeof(u32));
        default: return 0;
    }
}


static tiff_rational
rational(const u8* mem, const tiff_ifd_entry* entry) {
    switch (entry->type) {
        case 5: return *(tiff_rational*)(mem + entry->value);
        default: {
            tiff_rational r = {};
            return r;
        };
    }
}


static const char*
value_ascii(const u8* mem, const tiff_ifd_entry* entry) {
    switch (entry->type) {
        case 2: return (const char*)(mem + entry->value);
        default: return NULL;
    }
}


static const char*
tiff_compression_string(u32 scheme) {
    switch (scheme) {
        case 1: return "Uncompressed";
        case 2: return "CCITT 1D";
        case 3: return "Group 3 Fax";
        case 4: return "Group 4 Fax";
        case 5: return "LZW";
        case 6: return "JPEG";
        case 32773: return "PackBits";
        default: return NULL;
    }
}


static const char*
tiff_photometric_string(u32 scheme) {
    switch (scheme) {
        case 0: return "WhiteIsZero";
        case 1: return "BlackIsZero";
        case 2: return "RGB";
        case 3: return "RGB Palette";
        case 4: return "Transparency mask";
        case 5: return "CMYK";
        case 6: return "YCbCr";
        case 8: return "CIELab";
        default: return NULL;
    }
}


static const char*
tiff_orientation_string(u32 scheme) {
    switch (scheme) {
        case 1: return "top-left";
        case 2: return "top-right";
        case 3: return "bottom-right";
        case 4: return "bottom-left";
        case 5: return "left-top";
        case 6: return "right-top";
        case 7: return "right-bottom";
        case 8: return "left-bottom";
        default: return NULL;
    }
}


static const char*
tiff_resolution_unit_string(u32 scheme) {
    switch (scheme) {
        case 1: return "None";
        case 2: return "Inch";
        case 3: return "Centimeter";
        default: return NULL;
    }
}


static const char*
tiff_planar_string(u32 scheme) {
    switch (scheme) {
        case 1: return "Chunky";
        case 2: return "Planar";
        default: return NULL;
    }
}


static const char*
tiff_predictor_string(u32 scheme) {
    switch (scheme) {
        case 1: return "None";
        case 2: return "Horizontal differencing";
        default: return NULL;
    }
}


static void
dump_uncompressed_base(const u8* mem, u32 len, u32 base) {
    const u8* p = mem;
    for (u32 off = 0; off < len; ) {
        printf("%04X: ", off + base);
        for (u32 r = 0; r < 16 && off < len; r += 2) {
            if (r) { printf(" "); }
            for (u32 i = 0; i < 2 && off < len; ++i, ++off) {
                printf("%02X", *p++);
            }
        }
        printf("\n");
    }
}


static void
dump_uncompressed(const u8* mem, u32 len) {
    dump_uncompressed_base(mem, len, 0);
}


static u32
decompress_lzw(u8* dst, u32 dst_size, const u8* mem, u32 len) {
    u8 table_data[60000];
    u8* table_ptr = table_data;
    u8* table_ix[4096];
    u32 table_count = 0;
    u32 old_code = 0;
    u32 read_state = 0;
    const u8* pmem = mem;
    u8* pdst = dst;
    u32 total_write = 0;
    for (u8 bit_offset = 0; len > 0 && total_write < dst_size; ) {
        u32 code = 0;
        u32 need_bits = 9;
        for (u32 x = table_count >> 9; x > 0; ) {
            ++need_bits;
            table_count >>= 1;
        }
        for (; need_bits > 0 && len > 0; ) {
            u32 avail = 8 - bit_offset;
            u32 take = need_bits > avail ? avail : need_bits;
            u32 shift = take < avail ? (avail - take) : 0;
            code = (code << take) | ((*pmem >> shift) & ((1 << take) - 1));
            need_bits -= take;
            bit_offset += take;
            if (bit_offset >= 8) {
                bit_offset = 0;
                ++pmem;
                --len;
            }
        }
        switch (code) {
            case 256: {
                table_ptr = table_data;
                for (u32 i = 0; i < 256; ++i) {
                    table_ix[i] = table_ptr;
                    *table_ptr++ = 1;
                    *table_ptr++ = i;
                }
                table_count = 258;
                read_state = 0;
                break;
            }
            case 257:
                return total_write;
            default: {
                if (code < table_count) {
                    u8* p = table_ix[code];
                    for (u32 len = *p++; len > 0; --len) {
                        *pdst++ = *p++;
                        total_write = pdst - dst;
                    }
                    if (read_state) {
                        table_ix[table_count++] = table_ptr;
                        u8* p = table_ix[old_code];
                        *table_ptr++ = *p + 1;
                        for (u32 len = *p++; len > 0; --len) {
                            *table_ptr++ = *p++;
                        }
                        p = table_ix[code];
                        *table_ptr++ = *(p+1);
                    }
                    else {
                        read_state = 1;
                    }
                    old_code = code;
                }
                else {
                    u8* p = table_ix[old_code];
                    for (u32 len = *p++; len > 0; --len) {
                        if (total_write < dst_size) {
                            *pdst++ = *p;
                            total_write = pdst - dst;
                        }
                        ++p;
                    }
                    p = table_ix[old_code];
                    if (total_write < dst_size) {
                        *pdst++ = *(p+1);
                        total_write = pdst - dst;
                    }
                    {
                        table_ix[table_count++] = table_ptr;
                        u8* p = table_ix[old_code];
                        *table_ptr++ = *p + 1;
                        for (u32 len = *p++; len > 0; --len) {
                            *table_ptr++ = *p++;
                        }
                        p = table_ix[old_code];
                        *table_ptr++ = *(++p);
                    }
                    old_code = code;
                }
                break;
            }
        }
    }
    return total_write;
}


static void
reverse_differencing(u8* buf, u32 len, u32 stride, u32 samples) {
    for (u32 i = 0; i + samples < len; ) {
        u8* back = buf + i;
        i += samples;
        u8* fore = buf + i;
        for (u32 j = samples; j < stride && i < len; ++j, ++i) {
            *fore++ += *back++;
        }
    }
}


static void
dump(const u8* mem, u32 len, u32 compression, u32 predictor, u32 stride, u32 samples) {
    switch (compression) {
        default:
        case 0:
            dump_uncompressed(mem, len);
            break;
        #if APP_DECOMPRESS
        case 5: {
            u8 buf[0x40000];
            u32 n = decompress_lzw(buf, sizeof(buf), mem, len);
            if (predictor == 2) {
                reverse_differencing(buf, n, stride, samples);
            }
            dump_uncompressed(buf, n);
            break;
        }
        #endif
    }
}


int main(int argc, char const *argv[]) {
    // setbuf(stdout, NULL);

    if (argc < 2) {
        puts("usage: tiff-analysis <filename>");
        return 0;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        perror(NULL);
        return 1;
    }

    struct stat st;
    int res = fstat(fd, &st);
    if (res == -1) {
        goto perr0;
    }

    if (st.st_size == 0) {
        puts("the file is empty");
        goto exit0;
    }

    const u8* mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem == MAP_FAILED) {
        goto perr0;
    }

    u16 byte_order = read16(mem + 0);
    switch (byte_order) {
        case 0x4949:
            puts("byte order: little-endian");
            break;
        case 0x4D4D:
            puts("byte order: big-endian");
            goto err1;
        default:
            printf("unrecognized byte order %X.H\n", byte_order);
            goto err1;
    }

    u16 x42 = read16(mem + 2);
    if (x42 != 42) {
        printf("x: %u\n", x42);
        goto err1;
    }

    u32 pifd = read32(mem + 4);
    for (; pifd; ) {
        puts("image file directory:");
        u16 ifd_count = read16(mem + pifd);
        const tiff_ifd_entry* strip_offsets = NULL;
        const tiff_ifd_entry* strip_byte_counts = NULL;
        u32 image_width = 0, image_height = 0;
        u32 compression_scheme = 0;
        u32 predictor_scheme = 0;
        u32 samples_per_pixel = 1;
        for (u32 iifd = 0; iifd < ifd_count; ++iifd) {
            tiff_ifd_entry* entry = (tiff_ifd_entry*) (mem + pifd + sizeof(ifd_count) + iifd * sizeof(tiff_ifd_entry));
            printf("  ");
            switch (entry->tag) {
                case 256: {
                    image_width = value_u32(entry);
                    printf("ImageWidth:%u\n", image_width);
                    break;
                }
                case 257: {
                    image_height = value_u32(entry);
                    printf("ImageHeight:%u\n", image_height);
                    break;
                }
                case 258: {
                    printf("BitsPerSample:(");
                    for (u32 i = 0; i < entry->count; ++i) {
                        u32 x = value_u32a(mem, entry, i);
                        if (i) { printf(","); }
                        printf("%u", x);
                    }
                    printf(")\n");
                    break;
                }
                case 259: {
                    compression_scheme = value_u32(entry);
                    const char* s = tiff_compression_string(compression_scheme);
                    if (s) {
                        printf("Compression:%s\n", s);
                    }
                    else {
                        printf("Compression:%u\n", compression_scheme);
                    }
                    break;
                }
                case 262: {
                    u32 scheme = value_u32(entry);
                    const char* s = tiff_photometric_string(scheme);
                    if (s) {
                        printf("PhotometricInterpretation:%s\n", s);
                    }
                    else {
                        printf("PhotometricInterpretation:%u\n", scheme);
                    }
                    break;
                }
                case 273: {
                    strip_offsets = entry;
                    printf("StripOffsets:[");
                    for (u32 i = 0; i < entry->count; ++i) {
                        u32 x = value_u32a(mem, entry, i);
                        if (i) { printf(","); }
                        printf("%u", x);
                    }
                    printf("]\n");
                    break;
                }
                case 274: {
                    u32 scheme = value_u32(entry);
                    const char* s = tiff_orientation_string(scheme);
                    if (s) {
                        printf("Orientation:%s\n", s);
                    }
                    else {
                        printf("Orientation:%u\n", scheme);
                    }
                    break;
                }
                case 277: {
                    samples_per_pixel = value_u32(entry);
                    printf("SamplesPerPixel:%u\n", samples_per_pixel);
                    break;
                }
                case 278: {
                    u32 x = value_u32(entry);
                    printf("RowsPerStrip:%u\n", x);
                    break;
                }
                case 279: {
                    strip_byte_counts = entry;
                    printf("StripByteCounts:[");
                    for (u32 i = 0; i < entry->count; ++i) {
                        u32 x = value_u32a(mem, entry, i);
                        if (i) { printf(","); }
                        printf("%u", x);
                    }
                    printf("]\n");
                    break;
                }
                case 282: {
                    tiff_rational r = rational(mem, entry);
                    if (r.den != 1) {
                        printf("XResolution:%u/%u\n", r.num, r.den);
                    }
                    else {
                        printf("XResolution:%u\n", r.num);
                    }
                    break;
                }
                case 283: {
                    tiff_rational r = rational(mem, entry);
                    if (r.den != 1) {
                        printf("YResolution:%u/%u\n", r.num, r.den);
                    }
                    else {
                        printf("YResolution:%u\n", r.num);
                    }
                    break;
                }
                case 284: {
                    u32 scheme = value_u32(entry);
                    const char* s = tiff_planar_string(scheme);
                    if (s) {
                        printf("PlanarConfiguration:%s\n", s);
                    }
                    else {
                        printf("PlanarConfiguration:%u\n", scheme);
                    }
                    break;
                }
                case 285: {
                    const char* s = value_ascii(mem, entry);
                    if (s) {
                        printf("PageName:%s\n", s);
                    }
                    else {
                        printf("PageName:%u\n", entry->value);
                    }
                    break;
                }
                case 296: {
                    u32 scheme = value_u32(entry);
                    const char* s = tiff_resolution_unit_string(scheme);
                    if (s) {
                        printf("ResolutionUnit:%s\n", s);
                    }
                    else {
                        printf("ResolutionUnit:%u\n", scheme);
                    }
                    break;
                }
                case 317: {
                    predictor_scheme = value_u32(entry);
                    const char* s = tiff_predictor_string(predictor_scheme);
                    if (s) {
                        printf("Predictor:%s\n", s);
                    }
                    else {
                        printf("Predictor:%u\n", predictor_scheme);
                    }
                    break;
                }
                case 339: {
                    printf("SampleFormat:(");
                    for (u32 i = 0; i < entry->count; ++i) {
                        u32 x = value_u32a(mem, entry, i);
                        if (i) { printf(","); }
                        printf("%u", x);
                    }
                    printf(")\n");
                    break;
                }
                default:
                    printf("(tag:%u type:%u count:%u value:%u)\n", entry->tag, entry->type, entry->count, entry->value);
            }
        }
        pifd = read32(mem + pifd + sizeof(ifd_count) + ifd_count * sizeof(tiff_ifd_entry));
        if (pifd) {
            printf("next ifd is at %u\n", pifd);
        }

        #if APP_EXTRACT_STRIPS
        for (u32 sid = 0; strip_offsets && sid < strip_offsets->count; ++sid) {
            u32 off = value_u32a(mem, strip_offsets, sid);
            u32 len = value_u32a(mem, strip_byte_counts, sid);
            printf("\nStrip %u\n", sid);
            u32 stride = image_width * samples_per_pixel; // actually should depend on orientation
            dump(mem + off, len, compression_scheme, predictor_scheme, stride, samples_per_pixel);
        }
        #endif
    }

    munmap((void*)mem, st.st_size);
exit0:
    close(fd);
    return 0;

perr0:
    perror(NULL);
err1:
    close(fd);
    return 1;
}
