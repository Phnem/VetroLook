/* bench_decode_mt - multithreaded (inter-file) TinyEXR v3 decode benchmark.
 *
 * Reads newline-separated file paths from stdin into a work list, then decodes
 * them across N worker threads (one whole file per task). Each file is slurped
 * into RAM inside the worker; only exr_load_from_memory is counted toward the
 * aggregate CPU-time, while wall-clock spans the whole run.
 *
 * usage: find DIR -name '*.exr' | bench_decode_mt [num_threads]
 *        (default num_threads = online CPUs)
 *
 * SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "exr.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static char **g_files = NULL;
static long g_nfiles = 0;
static long g_next = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

struct wresult {
    long files;
    long fails;
    double cpu_s;   /* summed decode time across this worker */
    double mpix;
    double in_mb;
};

static void *worker(void *arg) {
    struct wresult *res = (struct wresult *)arg;
    for (;;) {
        long i;
        pthread_mutex_lock(&g_mtx);
        i = g_next++;
        pthread_mutex_unlock(&g_mtx);
        if (i >= g_nfiles) break;

        const char *path = g_files[i];
        FILE *fp = fopen(path, "rb");
        if (!fp) { res->fails++; continue; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { fclose(fp); res->fails++; continue; }
        unsigned char *buf = malloc((size_t)sz);
        if (!buf) { fclose(fp); res->fails++; continue; }
        if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); res->fails++; continue; }
        fclose(fp);

        exr_image img;
        memset(&img, 0, sizeof(img));
        double t0 = now_s();
        exr_result rc = exr_load_from_memory(buf, (size_t)sz, NULL, &img);
        res->cpu_s += now_s() - t0;

        if (!EXR_OK(rc)) { res->fails++; free(buf); continue; }

        double mpix = 0;
        for (int32_t p = 0; p < img.num_parts; p++)
            mpix += (double)img.parts[p].width * (double)img.parts[p].height *
                    (double)img.parts[p].header.num_channels;
        res->mpix += mpix / 1e6;
        res->in_mb += sz / 1e6;
        res->files++;

        exr_image_free(&img);
        free(buf);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (argc >= 2) nthreads = atoi(argv[1]);
    if (nthreads < 1) nthreads = 1;

    /* slurp file list */
    long cap = 1024;
    g_files = malloc(cap * sizeof(char *));
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n == 0) continue;
        if (g_nfiles == cap) { cap *= 2; g_files = realloc(g_files, cap * sizeof(char *)); }
        g_files[g_nfiles++] = strdup(line);
    }

    pthread_t *th = malloc(nthreads * sizeof(pthread_t));
    struct wresult *rs = calloc(nthreads, sizeof(struct wresult));

    double wall0 = now_s();
    for (int t = 0; t < nthreads; t++) pthread_create(&th[t], NULL, worker, &rs[t]);
    for (int t = 0; t < nthreads; t++) pthread_join(th[t], NULL);
    double wall = now_s() - wall0;

    long files = 0, fails = 0;
    double cpu = 0, mpix = 0, in_mb = 0;
    for (int t = 0; t < nthreads; t++) {
        files += rs[t].files; fails += rs[t].fails;
        cpu += rs[t].cpu_s; mpix += rs[t].mpix; in_mb += rs[t].in_mb;
    }

    printf("threads        : %d (of %ld online)\n", nthreads, sysconf(_SC_NPROCESSORS_ONLN));
    printf("files / fail   : %ld / %ld\n", files, fails);
    printf("Mpix-channels  : %.1f\n", mpix);
    printf("input MB       : %.1f\n", in_mb);
    printf("wall-clock s   : %.3f\n", wall);
    printf("sum decode s   : %.3f  (parallel speedup %.2fx)\n", cpu, wall > 0 ? cpu / wall : 0);
    printf("Mpix/s (wall)  : %.1f\n", wall > 0 ? mpix / wall : 0);
    printf("in MB/s (wall) : %.1f\n", wall > 0 ? in_mb / wall : 0);
    printf("files/s (wall) : %.1f\n", wall > 0 ? files / wall : 0);
    return 0;
}
