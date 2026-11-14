/*
 * json.h - Just enough JSON for the Filen API: a reader that finds values
 * in a response without copying it, and a writer whose output is byte for
 * byte that of JavaScript's JSON.stringify() (the API checks a hash of
 * the body).
 *
 * No allocation, no libc beyond the string functions (also compiled to
 * WebAssembly).
 */
#ifndef CWTTY_JSON_H
#define CWTTY_JSON_H

enum { JSON_NONE, JSON_OBJ, JSON_ARR, JSON_STR, JSON_NUM, JSON_TRUE, JSON_FALSE, JSON_NULL };

/* A value inside a document: where it starts and how long it is. */
typedef struct {
    const char *p;
    int         n;
} json_t;

/* The whole document (len < 0: up to the NUL). Its type is JSON_NONE if it
 * is not one value. */
json_t json_doc(const char *text, int len);
int    json_type(json_t v);

/* A member of an object, or a path of members ("data.apiKey"). 1 if found. */
int json_get(json_t obj, const char *key, json_t *out);
int json_path(json_t v, const char *path, json_t *out);

/* The items of an array (or the members' values of an object): start with
 * it->p == NULL; 1 while there is one. */
int json_next(json_t arr, json_t *it);

/* A string, unescaped into out (NUL-terminated): its length, or -1 if it
 * is not a string or does not fit. */
int json_str(json_t v, char *out, int cap);
/* The string member key of obj into out, "" when missing: the length or -1. */
int json_get_str(json_t obj, const char *key, char *out, int cap);

/* A number or a numeric string as an integer: 1 if it is one. */
int json_int(json_t v, long long *out);
/* true, or a number other than 0. */
int json_truthy(json_t v);

/* The writer. */
typedef struct {
    char *p;
    int   n, cap;
    int   comma; /* a value came before: the next one needs a comma */
} jw_t;

void jw_init(jw_t *w, char *buf, int cap);
void jw_open(jw_t *w, char brace); /* '{' or '[' */
void jw_close(jw_t *w, char brace); /* '}' or ']' */
void jw_key(jw_t *w, const char *key);
void jw_str(jw_t *w, const char *s);
void jw_strn(jw_t *w, const char *s, int n);
void jw_int(jw_t *w, long long v);
void jw_bool(jw_t *w, int v);
/* NUL-terminates: the length, or -1 if it did not fit. */
int jw_end(jw_t *w);

#endif
