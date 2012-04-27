/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include <assert.h>

#include "common.h"
#include "buffer.h"

#include "git2/revparse.h"
#include "git2/object.h"
#include "git2/oid.h"
#include "git2/refs.h"
#include "git2/tag.h"
#include "git2/commit.h"

GIT_BEGIN_DECL

typedef enum {
   REVPARSE_STATE_INIT,
   
   /* for "^{...}" and ^... */
   REVPARSE_STATE_CARET,

   /* For "~..." */
   REVPARSE_STATE_LINEAR,
} revparse_state;

static void set_invalid_syntax_err(const char *spec)
{
   giterr_set(GITERR_INVALID, "Refspec '%s' is not valid.", spec);
}

static int revparse_lookup_fully_qualifed_ref(git_object **out, git_repository *repo, const char*spec)
{
   git_reference *ref;
   git_object *obj = NULL;

   if (!git_reference_lookup(&ref, repo, spec)) {
      git_reference *resolved_ref;
      if (!git_reference_resolve(&resolved_ref, ref)) {
         if (!git_object_lookup(&obj, repo, git_reference_oid(resolved_ref), GIT_OBJ_ANY)) {
            *out = obj;
         }
         git_reference_free(resolved_ref);
      }
      git_reference_free(ref);
   }
   if (obj) {
      return 0;
   }

   return GIT_ERROR;
}

static int revparse_lookup_object(git_object **out, git_repository *repo, const char *spec)
{
   size_t speclen = strlen(spec);
   git_object *obj = NULL;
   git_oid oid;
   git_buf refnamebuf = GIT_BUF_INIT;
   static const char* formatters[] = {
      "refs/%s",
      "refs/tags/%s",
      "refs/heads/%s",
      "refs/remotes/%s",
      "refs/remotes/%s/HEAD",
      NULL
   };
   unsigned int i;
   const char *substr;

   /* "git describe" output; snip everything before/including "-g" */
   substr = strstr(spec, "-g");
   if (substr) {
      spec = substr + 2;
      speclen = strlen(spec);
   }

   /* SHA or prefix */
   if (!git_oid_fromstrn(&oid, spec, speclen)) {
      if (!git_object_lookup_prefix(&obj, repo, &oid, speclen, GIT_OBJ_ANY)) {
         *out = obj;
         return 0;
      }
   }

   /* Fully-named ref */
   if (!revparse_lookup_fully_qualifed_ref(&obj, repo, spec)) {
      *out = obj;
      return 0;
   }

   /* Partially-named ref; match in this order: */
   for (i=0; formatters[i]; i++) {
      git_buf_clear(&refnamebuf);
      if (git_buf_printf(&refnamebuf, formatters[i], spec) < 0) {
         return GIT_ERROR;
      }

      if (!revparse_lookup_fully_qualifed_ref(&obj, repo, git_buf_cstr(&refnamebuf))) {
         git_buf_free(&refnamebuf);
         *out = obj;
         return 0;
      }
   }
   git_buf_free(&refnamebuf);

   giterr_set(GITERR_REFERENCE, "Refspec '%s' not found.", spec);
   return GIT_ERROR;
}


static int walk_ref_history(git_object **out, const char *refspec, const char *reflogspec)
{
   /* TODO */

   /* Empty refspec means current branch */

   /* Possible syntaxes for reflogspec:
    * "8" -> 8th prior value for the ref
    * "-2" -> nth branch checked out before the current one (refspec must be empty)
    * "yesterday", "1 month 2 weeks 3 days 4 hours 5 seconds ago", "1979-02-26 18:30:00"
    *   -> value of ref at given point in time
    * "upstream" or "u" -> branch the ref is set to build on top of
    * */
   return 0;
}

static git_object* dereference_object(git_object *obj)
{
   git_otype type = git_object_type(obj);
   git_object *newobj = NULL;
   git_tree *tree = NULL;

   switch (type) {
   case GIT_OBJ_COMMIT:
      if (0 == git_commit_tree(&tree, (git_commit*)obj)) {
         return (git_object*)tree;
      }
      break;
   case GIT_OBJ_TAG:
      if (0 == git_tag_target(&newobj, (git_tag*)obj)) {
         return newobj;
      }
      break;

   default:
   case GIT_OBJ_TREE:
   case GIT_OBJ_BLOB:
   case GIT_OBJ_OFS_DELTA:
   case GIT_OBJ_REF_DELTA:
      break;
   }

   /* Can't dereference some types */
   return NULL;
}

static int dereference_to_type(git_object **out, git_object *obj, git_otype target_type)
{
   git_object *obj1 = obj, *obj2 = obj;

   while (1) {
      git_otype this_type = git_object_type(obj1);

      if (this_type == target_type) {
         *out = obj1;
         return 0;
      }

      /* Dereference once, if possible. */
      obj2 = dereference_object(obj1);
      if (!obj2) {
         giterr_set(GITERR_REFERENCE, "Can't dereference to type");
         return GIT_ERROR;
      }
      if (obj1 != obj) {
         git_object_free(obj1);
      }
      obj1 = obj2;
   }
}

static git_otype parse_obj_type(const char *str)
{
   if (!strcmp(str, "{commit}")) return GIT_OBJ_COMMIT;
   if (!strcmp(str, "{tree}")) return GIT_OBJ_TREE;
   if (!strcmp(str, "{blob}")) return GIT_OBJ_BLOB;
   if (!strcmp(str, "{tag}")) return GIT_OBJ_TAG;
   return GIT_OBJ_BAD;
}

static int handle_caret_syntax(git_object **out, git_object *obj, const char *movement)
{
   git_commit *commit;
   int n;

   if (*movement == '{') {
      if (movement[strlen(movement)-1] != '}') {
         set_invalid_syntax_err(movement);
         return GIT_ERROR;
      }

      /* {} -> Dereference until we reach an object that isn't a tag. */
      if (strlen(movement) == 2) {
         git_object *newobj = obj;
         git_object *newobj2 = newobj;
         while (git_object_type(newobj2) == GIT_OBJ_TAG) {
            newobj2 = dereference_object(newobj);
            if (newobj != obj) git_object_free(newobj);
            if (!newobj2) {
               giterr_set(GITERR_REFERENCE, "Couldn't find object of target type.");
               return GIT_ERROR;
            }
            newobj = newobj2;
         }
         *out = newobj2;
         return 0;
      }
      
      /* {/...} -> Walk all commits until we see a commit msg that matches the phrase. */
      if (movement[1] == '/') {
         /* TODO */
         return GIT_ERROR;
      }

      /* {...} -> Dereference until we reach an object of a certain type. */
      if (dereference_to_type(out, obj, parse_obj_type(movement)) < 0) {
         return GIT_ERROR;
      }
      return 0;
   }

   /* Dereference until we reach a commit. */
   if (dereference_to_type(&obj, obj, GIT_OBJ_COMMIT) < 0) {
      /* Can't dereference to a commit; fail */
      return GIT_ERROR;
   }

   /* "^" is the same as "^1" */
   if (strlen(movement) == 0) {
      n = 1;
   } else {
      git__strtol32(&n, movement, NULL, 0);
   }
   commit = (git_commit*)obj;

   /* "^0" just returns the input */
   if (n == 0) {
      *out = obj;
      return 0;
   }

   if (git_commit_parent(&commit, commit, n-1) < 0) {
      return GIT_ERROR;
   }

   *out = (git_object*)commit;
   return 0;
}

int git_revparse_single(git_object **out, git_repository *repo, const char *spec)
{
   revparse_state current_state = REVPARSE_STATE_INIT;
   revparse_state next_state = REVPARSE_STATE_INIT;
   const char *spec_cur = spec;
   git_object *cur_obj = NULL;
   git_object *next_obj = NULL;
   git_buf specbuffer = GIT_BUF_INIT;
   git_buf stepbuffer = GIT_BUF_INIT;
   int retcode = 0;

   assert(out && repo && spec);

   while (1) {
      switch (current_state) {
      case REVPARSE_STATE_INIT:
         if (!*spec_cur) {
            /* No operators, just a name. Find it and return. */
            return revparse_lookup_object(out, repo, spec);
         } else if (*spec_cur == '@') {
            /* '@' syntax doesn't allow chaining */
            git_buf_puts(&stepbuffer, spec_cur);
            retcode = walk_ref_history(out, git_buf_cstr(&specbuffer), git_buf_cstr(&stepbuffer));
            goto cleanup;
         } else if (*spec_cur == '^') {
            next_state = REVPARSE_STATE_CARET;
         } else if (*spec_cur == '~') {
            next_state = REVPARSE_STATE_LINEAR;
         } else {
            git_buf_putc(&specbuffer, *spec_cur);
         }
         spec_cur++;

         if (current_state != next_state) {
            /* Leaving INIT state, find the object specified, in case that state needs it */
            retcode = revparse_lookup_object(&next_obj, repo, git_buf_cstr(&specbuffer));
            if (retcode < 0) {
               goto cleanup;
            }
         }
         break;


      case REVPARSE_STATE_CARET:
         /* Gather characters until NULL, '~', or '^' */
         if (!*spec_cur) {
            retcode = handle_caret_syntax(out,
                                          cur_obj,
                                          git_buf_cstr(&stepbuffer));
            goto cleanup;
         } else if (*spec_cur == '~') {
            retcode = handle_caret_syntax(&next_obj,
                                          cur_obj,
                                          git_buf_cstr(&stepbuffer));
            git_buf_clear(&stepbuffer);
            if (retcode < 0) goto cleanup;
            next_state = REVPARSE_STATE_LINEAR;
         } else if (*spec_cur == '^') {
            retcode = handle_caret_syntax(&next_obj,
                                          cur_obj,
                                          git_buf_cstr(&stepbuffer));
            git_buf_clear(&stepbuffer);
            if (retcode < 0) goto cleanup;
            next_state = REVPARSE_STATE_CARET;
         } else {
            git_buf_putc(&stepbuffer, *spec_cur);
         }
         spec_cur++;
         break;

      case REVPARSE_STATE_LINEAR:
         if (!*spec_cur) {
         } else if (*spec_cur == '~') {
         } else if (*spec_cur == '^') {
         } else {
            git_buf_putc(&stepbuffer, *spec_cur);
         }
         spec_cur++;
         break;
      }

      current_state = next_state;
      if (cur_obj != next_obj) {
         git_object_free(cur_obj);
         cur_obj = next_obj;
      }
   }

cleanup:
   git_buf_free(&specbuffer);
   git_buf_free(&stepbuffer);
   return retcode;
}


GIT_END_DECL
