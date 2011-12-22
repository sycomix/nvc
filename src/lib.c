//
//  Copyright (C) 2011  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "lib.h"
#include "tree.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_UNITS 16

#define MAX_SEARCH_PATHS 64

struct lib_unit {
   tree_t top;
   bool   dirty;
};

struct lib {
   char            path[PATH_MAX];
   ident_t         name;
   unsigned        n_units;
   struct lib_unit *units;
};

struct lib_list {
   lib_t           item;
   struct lib_list *next;
};

static lib_t           work = NULL;
static struct lib_list *loaded = NULL;

static ident_t upcase_name(const char *name)
{
   char *name_up = strdup(name);
   for (char *p = name_up; *p != '\0'; p++)
      *p = toupper(*p);

   ident_t i = ident_new(name_up);
   free(name_up);
   return i;
}

static lib_t lib_init(const char *name, const char *rpath)
{
   struct lib *l = xmalloc(sizeof(struct lib));
   l->n_units = 0;
   l->units   = NULL;
   l->name    = upcase_name(name);

   realpath(rpath, l->path);

   struct lib_list *el = xmalloc(sizeof(struct lib_list));
   el->item = l;
   el->next = loaded;
   loaded = el;

   return l;
}

static void lib_put_aux(lib_t lib, tree_t unit, bool dirty)
{
   assert(lib != NULL);
   assert(unit != NULL);
   assert(lib->n_units < MAX_UNITS);

   if (lib->n_units == 0)
      lib->units = xmalloc(sizeof(struct lib_unit) * MAX_UNITS);

   unsigned n = lib->n_units++;
   lib->units[n].top   = unit;
   lib->units[n].dirty = dirty;
}

static lib_t lib_find_at(const char *name, const char *path)
{
   char dir[PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/%s", path, name);

   // Convert to lower case
   for (char *p = dir; *p != '\0'; p++)
      *p = tolower(*p);

   if (access(dir, F_OK) < 0)
      return NULL;

   char marker[PATH_MAX];
   snprintf(marker, sizeof(marker), "%s/_NVC_LIB", dir);
   if (access(marker, F_OK) < 0)
      return NULL;

   return lib_init(name, dir);
}

lib_t lib_new(const char *name)
{
   if (access(name, F_OK) == 0) {
      errorf("file %s already exists", name);
      return NULL;
   }

   if (mkdir(name, 0777) != 0) {
      perror("mkdir");
      return NULL;
   }

   lib_t l = lib_init(name, name);

   FILE *tag = lib_fopen(l, "_NVC_LIB", "w");
   fprintf(tag, "%s\n", PACKAGE_STRING);
   fclose(tag);

   return l;
}

lib_t lib_tmp(void)
{
   // For unit tests, avoids creating files
   return lib_init("work", "");
}

static void push_path(const char **base, size_t *pidx, const char *path)
{
   if (*pidx < MAX_SEARCH_PATHS - 1) {
      base[(*pidx)++] = path;
      base[*pidx] = NULL;
   }
}

lib_t lib_find(const char *name, bool verbose, bool search)
{
   // Search in already loaded libraries
   ident_t name_i = upcase_name(name);
   for (struct lib_list *it = loaded; it != NULL; it = it->next) {
      if (lib_name(it->item) == name_i)
         return it->item;
   }

   const char *paths[MAX_SEARCH_PATHS];
   size_t idx = 0;

   push_path(paths, &idx, ".");

   char *env_copy = NULL;
   if (search) {
      const char *libpath_env = getenv("NVC_LIBPATH");
      if (libpath_env) {
         env_copy = strdup(libpath_env);

         const char *path_tok = strtok(env_copy, ":");
         do {
            push_path(paths, &idx, path_tok);
         } while ((path_tok = strtok(NULL, ":")));
      }

      push_path(paths, &idx, DATADIR);
   }

   lib_t lib;
   for (const char **p = paths; *p != NULL; p++) {
      if ((lib = lib_find_at(name, *p))) {
         free(env_copy);
         return lib;
      }
   }

   if (verbose) {
      fprintf(stderr, "library %s not found in:\n", name);
      for (const char **p = paths; *p != NULL; p++) {
         fprintf(stderr, "  %s\n", *p);
      }
   }

   free(env_copy);
   return NULL;
}

FILE *lib_fopen(lib_t lib, const char *name, const char *mode)
{
   assert(lib != NULL);

   char buf[PATH_MAX];
   snprintf(buf, sizeof(buf), "%s/%s", lib->path, name);

   return fopen(buf, mode);
}

void lib_free(lib_t lib)
{
   assert(lib != NULL);

   for (struct lib_list *it = loaded, *prev = NULL;
        it != NULL; loaded = it, it = it->next) {

      if (it->item == lib) {
         if (prev)
            prev->next = it->next;
         else
            loaded = it->next;
         free(it);
         break;
      }
   }

   if (lib->units != NULL)
      free(lib->units);
   free(lib);
}

void lib_destroy(lib_t lib)
{
   // This is convenience function for testing: remove all
   // files associated with a library

   assert(lib != NULL);

   DIR *d = opendir(lib->path);
   if (d == NULL) {
      perror("opendir");
      return;
   }

   char buf[PATH_MAX];
   struct dirent *e;
   while ((e = readdir(d))) {
      if (e->d_name[0] != '.') {
         snprintf(buf, sizeof(buf), "%s/%s", lib->path, e->d_name);
         if (unlink(buf) < 0)
            perror("unlink");
      }
   }

   closedir(d);

   if (rmdir(lib->path) < 0)
      perror("rmdir");
}

lib_t lib_work(void)
{
   assert(work != NULL);
   return work;
}

void lib_set_work(lib_t lib)
{
   work = lib;
}

void lib_put(lib_t lib, tree_t unit)
{
   lib_put_aux(lib, unit, true);
}

tree_t lib_get_ctx(lib_t lib, ident_t ident, tree_rd_ctx_t *ctx)
{
   assert(lib != NULL);

   // Search in the list of already loaded libraries
   for (unsigned n = 0; n < lib->n_units; n++) {
      if (tree_ident(lib->units[n].top) == ident)
         return lib->units[n].top;
   }

   if (*(lib->path) == '\0')   // Temporary library
      return NULL;

   // Otherwise search in the filesystem
   DIR *d = opendir(lib->path);
   if (d == NULL)
      fatal("%s: %s", lib->path, strerror(errno));

   tree_t unit = NULL;
   const char *search = istr(ident);
   struct dirent *e;
   while ((e = readdir(d))) {
      if (strcmp(e->d_name, search) == 0) {
         FILE *f = lib_fopen(lib, e->d_name, "r");
         *ctx = tree_read_begin(f);
         unit = tree_read(*ctx);
         lib_put_aux(lib, unit, false);
         break;
      }
   }

   closedir(d);

   return unit;
}

tree_t lib_get(lib_t lib, ident_t ident)
{
   tree_rd_ctx_t ctx = NULL;
   tree_t t = lib_get_ctx(lib, ident, &ctx);
   if (ctx != NULL)
      tree_read_end(ctx);
   return t;
}

void lib_load_all(lib_t lib)
{
   assert(lib != NULL);

   if (*(lib->path) == '\0')   // Temporary library
      return;

   DIR *d = opendir(lib->path);
   if (d == NULL)
      fatal("%s: %s", lib->path, strerror(errno));

   struct dirent *e;
   while ((e = readdir(d))) {
      if (e->d_name[0] != '.' && e->d_name[0] != '_')
         (void)lib_get(lib, ident_new(e->d_name));
   }

   closedir(d);

}

ident_t lib_name(lib_t lib)
{
   assert(lib != NULL);
   return lib->name;
}

void lib_save(lib_t lib)
{
   assert(lib != NULL);

   for (unsigned n = 0; n < lib->n_units; n++) {
      if (lib->units[n].dirty) {
         const char *name = istr(tree_ident(lib->units[n].top));
         FILE *f = lib_fopen(lib, name, "w");
         tree_wr_ctx_t ctx = tree_write_begin(f);
         tree_write(lib->units[n].top, ctx);
         tree_write_end(ctx);
         fclose(f);

         lib->units[n].dirty = false;
      }
   }
}

void lib_foreach(lib_t lib, lib_iter_fn_t fn, void *context)
{
   assert(lib != NULL);

   for (unsigned i = 0; i < lib->n_units; i++)
      (*fn)(lib->units[i].top, context);
}

void lib_realpath(lib_t lib, const char *name, char *buf, size_t buflen)
{
   assert(lib != NULL);

   if (name)
      snprintf(buf, buflen, "%s/%s", lib->path, name);
   else
      strncpy(buf, lib->path, buflen);
}
