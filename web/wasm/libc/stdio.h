/*
 * stdio.h - There are no files in the browser. These declarations let
 * settings.c compile; its file functions are never called, so the linker
 * drops them, and any use of these would fail to link.
 */
#ifndef CWTTY_LIBC_STDIO_H
#define CWTTY_LIBC_STDIO_H

typedef struct FILE FILE;

FILE *fopen(const char *path, const char *mode);
char *fgets(char *buf, int n, FILE *f);
int   fprintf(FILE *f, const char *fmt, ...);
int   fclose(FILE *f);

#endif
