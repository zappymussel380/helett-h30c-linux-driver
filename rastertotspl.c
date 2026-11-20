/*
 * rastertotspl - CUPS filter: 1-bit CUPS raster -> TSPL (TSC/4BARCODE label printers)
 *
 * Written for the Helett H30C Lite (USB ID 2d84:b028, reports as 4BARCODE 4B-2054N).
 * Replaces the closed macOS "rastertosnailtspl" filter from the vendor driver.
 *
 * Input: 8-bit grayscale (W, 0=black) raster, converted to 1-bit here with either a
 * hard threshold (default: crisp text/barcodes, no grey speckle) or Floyd-Steinberg
 * dithering (option Halftone=Dither, for photos). 1-bit K raster is accepted too.
 *
 * Build: gcc -O2 -Wall -o rastertotspl rastertotspl.c -lcupsimage -lcups
 */
#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t canceled = 0;
static void on_term(int sig) { (void)sig; canceled = 1; }

/* Returns the marked choice for a PPD option, or NULL. */
static const char *choice(ppd_file_t *ppd, const char *name)
{
    ppd_choice_t *c = ppd ? ppdFindMarkedChoice(ppd, name) : NULL;
    return c ? c->choice : NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        perror("ERROR: Unable to open raster file");
        return 1;
    }

    signal(SIGTERM, on_term);

    /* Resolve options: PPD defaults overridden by job options. */
    ppd_file_t *ppd = ppdOpenFile(getenv("PPD"));
    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    if (ppd) {
        ppdMarkDefaults(ppd);
        cupsMarkOptions(ppd, num_options, options);
    }

    const char *darkness  = choice(ppd, "Darkness");    /* "-1" or 0..15 */
    const char *speed     = choice(ppd, "PrintSpeed");  /* "Normal" or 2..6 */
    const char *papertype = choice(ppd, "PaperType");   /* Normal|LabelGaps|LabelMark|Continue */
    const char *gaps      = choice(ppd, "GapsHeight");  /* 0..10 (mm) */
    const char *rotate    = choice(ppd, "Rotate180");   /* True|False */
    const char *halftone  = choice(ppd, "Halftone");    /* Dither | <threshold 1..254> */
    int dither = halftone && !strcmp(halftone, "Dither");
    int threshold = (halftone && !dither) ? atoi(halftone) : 170;
    if (threshold < 1 || threshold > 254) threshold = 170;

    int copies = atoi(argv[4]);
    if (copies < 1) copies = 1;

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t h;
    int page = 0;

    while (!canceled && cupsRasterReadHeader2(ras, &h)) {
        page++;
        fprintf(stderr, "INFO: Printing label %d\n", page);
        fprintf(stderr, "PAGE: %d %d\n", page, copies);

        int gray8 = (h.cupsBitsPerPixel == 8 && (h.cupsColorSpace == CUPS_CSPACE_W ||
                                                 h.cupsColorSpace == CUPS_CSPACE_SW));
        int k1 = (h.cupsBitsPerPixel == 1 && h.cupsColorSpace == CUPS_CSPACE_K);
        if (!gray8 && !k1) {
            fprintf(stderr, "ERROR: Unsupported raster (bpp=%u cs=%u); need 8-bit W or 1-bit K\n",
                    h.cupsBitsPerPixel, h.cupsColorSpace);
            break;
        }

        unsigned width = h.cupsWidth, rows = h.cupsHeight;
        unsigned bpl = k1 ? h.cupsBytesPerLine : (width + 7) / 8;   /* TSPL bytes per row */
        unsigned char *bmp = malloc((size_t)bpl * rows);
        unsigned char *line = malloc(h.cupsBytesPerLine);
        /* Floyd-Steinberg error rows (2 extra cells for edges) */
        int *err_cur = calloc(width + 2, sizeof(int)), *err_nxt = calloc(width + 2, sizeof(int));
        if (!bmp || !line || !err_cur || !err_nxt) {
            fputs("ERROR: Out of memory\n", stderr);
            free(bmp); free(line); free(err_cur); free(err_nxt);
            break;
        }

        unsigned y;
        for (y = 0; y < rows && !canceled; y++) {
            unsigned char *out = bmp + (size_t)y * bpl;
            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < 1) break;
            if (k1) {
                /* CUPS K: 1 = black. TSPL BITMAP: 0 = print a dot. */
                for (unsigned x = 0; x < bpl; x++) out[x] = (unsigned char)~line[x];
                continue;
            }
            memset(out, 0xFF, bpl);                         /* all white */
            for (unsigned x = 0; x < width; x++) {
                int v = line[x], black;
                if (dither) {
                    v += err_cur[x + 1] / 16;
                    black = v < 128;
                    int e = v - (black ? 0 : 255);
                    err_cur[x + 2] += e * 7;
                    err_nxt[x]     += e * 3;
                    err_nxt[x + 1] += e * 5;
                    err_nxt[x + 2] += e * 1;
                } else {
                    black = v < threshold;
                }
                if (black) out[x >> 3] &= (unsigned char)~(0x80 >> (x & 7));
            }
            if (dither) {
                int *t = err_cur; err_cur = err_nxt; err_nxt = t;
                memset(err_nxt, 0, (width + 2) * sizeof(int));
            }
        }
        free(line); free(err_cur); free(err_nxt);
        if (canceled || y < rows) { free(bmp); break; }

        double w_mm = h.PageSize[0] * 25.4 / 72.0;
        double h_mm = h.PageSize[1] * 25.4 / 72.0;

        printf("SIZE %.1f mm,%.1f mm\r\n", w_mm, h_mm);
        if (papertype) {
            int gap = gaps ? atoi(gaps) : 3;
            if (!strcmp(papertype, "LabelGaps"))      printf("GAP %d mm,0 mm\r\n", gap);
            else if (!strcmp(papertype, "LabelMark")) printf("BLINE %d mm,0 mm\r\n", gap);
            else if (!strcmp(papertype, "Continue"))  printf("GAP 0 mm,0 mm\r\n");
        }
        if (speed && strcmp(speed, "Normal"))           printf("SPEED %s\r\n", speed);
        if (darkness && atoi(darkness) >= 0)            printf("DENSITY %s\r\n", darkness);
        printf("DIRECTION %d\r\n", (rotate && !strcmp(rotate, "True")) ? 0 : 1);
        printf("REFERENCE 0,0\r\nCLS\r\n");
        printf("BITMAP 0,0,%u,%u,0,", bpl, rows);
        fwrite(bmp, 1, (size_t)bpl * rows, stdout);
        printf("\r\nPRINT 1,%d\r\n", copies);
        fflush(stdout);
        free(bmp);
    }

    cupsRasterClose(ras);
    cupsFreeOptions(num_options, options);
    if (ppd) ppdClose(ppd);
    if (fd) close(fd);

    if (page == 0) {
        fputs("ERROR: No pages found\n", stderr);
        return 1;
    }
    fputs("INFO: Ready to print.\n", stderr);
    return canceled ? 1 : 0;
}
