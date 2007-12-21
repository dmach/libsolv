/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "attr_store.h"
#include "repo_susetags.h"

static int
split(char *l, char **sp, int m)
{
  int i;
  for (i = 0; i < m;)
    {
      while (*l == ' ')
	l++;
      if (!*l)
	break;
      sp[i++] = l;
      while (*l && *l != ' ')
	l++;
      if (!*l)
	break;
      *l++ = 0;
    }
  return i;
}

struct parsedata {
  char *kind;
  Repo *repo;
  char *tmp;
  int tmpl;
  char **sources;
  int nsources;
  int last_found_source;
};

static Id
makeevr(Pool *pool, char *s)
{
  if (!strncmp(s, "0:", 2) && s[2])
    s += 2;
  return str2id(pool, s, 1);
}

static char *flagtab[] = {
  ">",
  "=",
  ">=",
  "<",
  "!=",
  "<="
};

static char *
join(struct parsedata *pd, char *s1, char *s2, char *s3)
{
  int l = 1;
  char *p;

  if (s1)
    l += strlen(s1);
  if (s2)
    l += strlen(s2);
  if (s3)
    l += strlen(s3);
  if (l > pd->tmpl)
    {
      pd->tmpl = l + 256;
      if (!pd->tmp)
	pd->tmp = malloc(pd->tmpl);
      else
	pd->tmp = realloc(pd->tmp, pd->tmpl);
    }
  p = pd->tmp;
  if (s1)
    {
      strcpy(p, s1);
      p += strlen(s1);
    }
  if (s2)
    {
      strcpy(p, s2);
      p += strlen(s2);
    }
  if (s3)
    {
      strcpy(p, s3);
      p += strlen(s3);
    }
  return pd->tmp;
}

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, int isreq, char *kind)
{
  int i, flags;
  Id id, evrid;
  char *sp[4];

  i = split(line + 5, sp, 4);
  if (i != 1 && i != 3)
    {
      fprintf(stderr, "Bad dependency line: %s\n", line);
      exit(1);
    }
  if (kind)
    id = str2id(pool, join(pd, kind, ":", sp[0]), 1);
  else
    id = str2id(pool, sp[0], 1);
  if (i == 3)
    {
      evrid = makeevr(pool, sp[2]);
      for (flags = 0; flags < 6; flags++)
        if (!strcmp(sp[1], flagtab[flags]))
          break;
      if (flags == 6)
	{
	  fprintf(stderr, "Unknown relation '%s'\n", sp[1]);
	  exit(1);
	}
      id = rel2id(pool, id, evrid, flags + 1, 1);
    }
  return repo_addid_dep(pd->repo, olddeps, id, isreq);
}

Attrstore *attr;
static Id id_authors;
static Id id_description;
static Id id_downloadsize;
static Id id_eula;
static Id id_group;
static Id id_installsize;
static Id id_keywords;
static Id id_license;
static Id id_messagedel;
static Id id_messageins;
static Id id_mediadir;
static Id id_mediafile;
static Id id_medianr;
static Id id_nosource;
static Id id_source;
static Id id_sourceid;
static Id id_summary;
static Id id_time;

static void
add_location (char *line, Solvable *s, unsigned entry)
{
  Pool *pool = s->repo->pool;
  char *sp[3];
  int i;

  i = split(line, sp, 3);
  if (i != 2 && i != 3)
    {
      fprintf(stderr, "Bad location line: %s\n", line);
      exit(1);
    }
  /* If we have a dirname, let's see if it's the same as arch.  In that
     case don't store it.  */
  if (i == 3 && !strcmp (sp[2], id2str (pool, s->arch)))
    sp[2] = 0, i = 2;
  if (i == 3 && sp[2])
    {
      /* medianr filename dir
         don't optimize this one */
      add_attr_special_int (attr, entry, id_medianr, atoi (sp[0]));
      add_attr_localids_id (attr, entry, id_mediadir, str2localid (attr, sp[2], 1));
      add_attr_string (attr, entry, id_mediafile, sp[1]);
      return;
    }
  else
    {
      /* Let's see if we can optimize this a bit.  If the media file name
         can be formed by the base rpm information we don't store it, but
	 only a flag that we've seen it.  */
      unsigned int medianr = atoi (sp[0]);
      const char *n1 = sp[1];
      const char *n2 = id2str (pool, s->name);
      for (n2 = id2str (pool, s->name); *n2; n1++, n2++)
        if (*n1 != *n2)
	  break;
      if (*n2 || *n1 != '-')
        goto nontrivial;

      n1++;
      for (n2 = id2str (pool, s->evr); *n2; n1++, n2++)
	if (*n1 != *n2)
	  break;
      if (*n2 || *n1 != '.')
        goto nontrivial;
      n1++;
      for (n2 = id2str (pool, s->arch); *n2; n1++, n2++)
	if (*n1 != *n2)
	  break;
      if (*n2 || strcmp (n1, ".rpm"))
        goto nontrivial;
      add_attr_special_int (attr, entry, id_medianr, medianr);
      add_attr_void (attr, entry, id_mediafile);
      return;

nontrivial:
      add_attr_special_int (attr, entry, id_medianr, medianr);
      add_attr_string (attr, entry, id_mediafile, sp[1]);
      return;
    }
}

static void
add_source (char *line, struct parsedata *pd, Solvable *s, unsigned entry, int first)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  char *sp[5];

  if (split(line, sp, 5) != 4)
    {
      fprintf(stderr, "Bad source line: %s\n", line);
      exit(1);
    }

  Id name = str2id(pool, sp[0], 1);
  Id evr = makeevr(pool, join(pd, sp[1], "-", sp[2]));
  Id arch = str2id(pool, sp[3], 1);

  /* Now, if the source of a package only differs in architecture
     (src or nosrc), code only that fact.  */
  if (s->name == name && s->evr == evr
      && (arch == ARCH_SRC || arch == ARCH_NOSRC))
    add_attr_void (attr, entry, arch == ARCH_SRC ? id_source : id_nosource);
  else if (first)
    {
      if (entry >= pd->nsources)
        {
	  if (pd->nsources)
	    {
	      pd->sources = realloc (pd->sources, (entry + 256) * sizeof (*pd->sources));
	      memset (pd->sources + pd->nsources, 0, (entry + 256 - pd->nsources) * sizeof (*pd->sources));
	    }
	  else
	    pd->sources = calloc (entry + 256, sizeof (*pd->sources));
	  pd->nsources = entry + 256;
	}
      /* Uarrr.  Unsplit.  */
      sp[0][strlen (sp[0])] = ' ';
      sp[1][strlen (sp[1])] = ' ';
      sp[2][strlen (sp[2])] = ' ';
      pd->sources[entry] = strdup (sp[0]);
    }
  else
    {
      unsigned n, nn;
      Solvable *found = 0;
      /* Otherwise we may find a solvable with exactly matching name, evr, arch
         in the repository already.  In that case encode its ID.  */
      for (n = repo->start, nn = repo->start + pd->last_found_source;
           n < repo->end; n++, nn++)
        {
	  if (nn >= repo->end)
	    nn = repo->start;
	  found = pool->solvables + nn;
	  if (found->repo == repo
	      && found->name == name
	      && found->evr == evr
	      && found->arch == arch)
	    {
	      pd->last_found_source = nn - repo->start;
	      break;
	    }
        }
      if (n != repo->end)
        add_attr_intlist_int (attr, entry, id_sourceid, nn - repo->start);
      else
        {
          add_attr_localids_id (attr, entry, id_source, str2localid (attr, sp[0], 1));
          add_attr_localids_id (attr, entry, id_source, str2localid (attr, join (pd, sp[1], "-", sp[2]), 1));
          add_attr_localids_id (attr, entry, id_source, str2localid (attr, sp[3], 1));
	}
    }
}

/* Unfortunately "a"[0] is no constant expression in the C languages,
   so we need to pass the four characters individually :-/  */
#define CTAG(a,b,c,d) ((unsigned)(((unsigned char)a) << 24) \
 | ((unsigned char)b << 16) \
 | ((unsigned char)c << 8) \
 | ((unsigned char)d))

static inline unsigned
tag_from_string (char *cs)
{
  unsigned char *s = (unsigned char*) cs;
  return ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]);
}

void
repo_add_susetags(Repo *repo, FILE *fp, Id vendor, int with_attr)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s;
  int intag = 0;
  int cummulate = 0;
  int indesc = 0;
  int last_found_pack = 0;
  char *sp[5];
  struct parsedata pd;

  if (with_attr)
    {
      attr = new_store(pool);
      id_authors = str2id (pool, "authors", 1);
      id_description = str2id (pool, "description", 1);
      id_downloadsize = str2id (pool, "downloadsize", 1);
      id_eula = str2id (pool, "eula", 1);
      id_group = str2id (pool, "group", 1);
      id_installsize = str2id (pool, "installsize", 1);
      id_keywords = str2id (pool, "keywords", 1);
      id_license = str2id (pool, "license", 1);
      id_messagedel = str2id (pool, "messagedel", 1);
      id_messageins = str2id (pool, "messageins", 1);
      id_mediadir = str2id (pool, "mediadir", 1);
      id_mediafile = str2id (pool, "mediafile", 1);
      id_medianr = str2id (pool, "medianr", 1);
      id_nosource = str2id (pool, "nosource", 1);
      id_source = str2id (pool, "source", 1);
      id_sourceid = str2id (pool, "sourceid", 1);
      id_summary = str2id (pool, "summary", 1);
      id_time = str2id (pool, "time", 1);
    }

  memset(&pd, 0, sizeof(pd));
  line = malloc(1024);
  aline = 1024;

  pd.repo = repo;

  linep = line;
  s = 0;

  for (;;)
    {
      unsigned tag;
      if (linep - line + 16 > aline)
	{
	  aline = linep - line;
	  line = realloc(line, aline + 512);
	  linep = line + aline;
	  aline += 512;
	}
      if (!fgets(linep, aline - (linep - line), fp))
	break;
      linep += strlen(linep);
      if (linep == line || linep[-1] != '\n')
        continue;
      *--linep = 0;
      if (intag)
	{
	  int isend = linep[-intag - 2] == '-' && linep[-1] == ':' && !strncmp(linep - 1 - intag, line + 1, intag) && (linep == line + 1 + intag + 1 + 1 + 1 + intag + 1 || linep[-intag - 3] == '\n');
	  if (cummulate && !isend)
	    {
	      *linep++ = '\n';
	      continue;
	    }
	  if (cummulate && isend)
	    {
	      linep[-intag - 2] = 0;
	      if (linep[-intag - 3] == '\n')
	        linep[-intag - 3] = 0;
	      linep = line;
	      intag = 0;
	    }
	  if (!cummulate && isend)
	    {
	      intag = 0;
	      linep = line;
	      continue;
	    }
	  if (!cummulate && !isend)
	    linep = line + intag + 3;
	}
      else
	linep = line;
      if (!intag && line[0] == '+' && line[1] && line[1] != ':')
	{
	  char *tagend = strchr(line, ':');
	  if (!tagend)
	    {
	      fprintf(stderr, "bad line: %s\n", line);
	      exit(1);
	    }
	  intag = tagend - (line + 1);
	  cummulate = 0;
	  switch (tag_from_string (line))
	    {
	      case CTAG('+', 'D', 'e', 's'):
	      case CTAG('+', 'E', 'u', 'l'):
	      case CTAG('+', 'I', 'n', 's'):
	      case CTAG('+', 'D', 'e', 'l'):
	      case CTAG('+', 'A', 'u', 't'):
	        if (line[4] == ':')
	          cummulate = 1;
	    }
	  line[0] = '=';
	  line[intag + 2] = ' ';
	  linep = line + intag + 3;
	  continue;
	}
      if (*line == '#' || !*line)
	continue;
      if (! (line[0] && line[1] && line[2] && line[3] && line[4] == ':'))
        continue;
      tag = tag_from_string (line);
      if (indesc < 2
          && (tag == CTAG('=', 'P', 'k', 'g')
	      || tag == CTAG('=', 'P', 'a', 't')))
	{
	  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	  if (s)
	    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
	  pd.kind = 0;
	  if (line[3] == 't')
	    pd.kind = "pattern";
	  s = pool_id2solvable(pool, repo_add_solvable(repo));
	  last_found_pack = (s - pool->solvables) - repo->start;
          if (split(line + 5, sp, 5) != 4)
	    {
	      fprintf(stderr, "Bad line: %s\n", line);
	      exit(1);
	    }
	  if (pd.kind)
	    s->name = str2id(pool, join(&pd, pd.kind, ":", sp[0]), 1);
	  else
	    s->name = str2id(pool, sp[0], 1);
	  s->evr = makeevr(pool, join(&pd, sp[1], "-", sp[2]));
	  s->arch = str2id(pool, sp[3], 1);
	  s->vendor = vendor;
	  continue;
	}
      if (indesc == 2
          && (tag == CTAG('=', 'P', 'k', 'g')
	      || tag == CTAG('=', 'P', 'a', 't')))
	{
	  Id name, evr, arch;
	  int n, nn;
	  pd.kind = 0;
	  if (line[3] == 't')
	    pd.kind = "pattern";
          if (split(line + 5, sp, 5) != 4)
	    {
	      fprintf(stderr, "Bad line: %s\n", line);
	      exit(1);
	    }
	  s = 0;
	  if (pd.kind)
	    name = str2id(pool, join(&pd, pd.kind, ":", sp[0]), 0);
	  else
	    name = str2id(pool, sp[0], 0);
	  evr = makeevr(pool, join(&pd, sp[1], "-", sp[2]));
	  arch = str2id(pool, sp[3], 0);
	  /* If we found neither the name nor the arch at all in this repo
	     there's no chance of finding the exact solvable either.  */
	  if (!name || !arch)
	    continue;
	  /* Now look for a solvable with the given name,evr,arch.
	     Our input is structured so, that the second set of =Pkg
	     lines comes in roughly the same order as the first set, so we 
	     have a hint at where to start our search, namely were we found
	     the last entry.  */
	  for (n = repo->start, nn = n + last_found_pack; n < repo->end; n++, nn++)
	    {
	      if (nn >= repo->end)
	        nn = repo->start;
	      s = pool->solvables + nn;
	      if (s->repo == repo && s->name == name && s->evr == evr && s->arch == arch)
	        break;
	    }
	  if (n == repo->end)
	    s = 0;
	  else
	    last_found_pack = nn - repo->start;
	  continue;
	}
      /* If we have no current solvable to add to, ignore all further lines
         for it.  Probably invalid input data in the second set of
	 solvables.  */
      if (indesc >= 2 && !s)
        {
	  fprintf (stderr, "Huh?\n");
          continue;
	}
      switch (tag)
        {
	  case CTAG('=', 'P', 'r', 'v'):
	    s->provides = adddep(pool, &pd, s->provides, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'R', 'e', 'q'):
	    s->requires = adddep(pool, &pd, s->requires, line, 1, pd.kind);
	    continue;
          case CTAG('=', 'P', 'r', 'q'):
	    if (pd.kind)
	      s->requires = adddep(pool, &pd, s->requires, line, 0, 0);
	    else
	      s->requires = adddep(pool, &pd, s->requires, line, 2, 0);
	    continue;
	  case CTAG('=', 'O', 'b', 's'):
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'C', 'o', 'n'):
	    s->conflicts = adddep(pool, &pd, s->conflicts, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'R', 'e', 'c'):
	    s->recommends = adddep(pool, &pd, s->recommends, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'S', 'u', 'p'):
	    s->supplements = adddep(pool, &pd, s->supplements, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'E', 'n', 'h'):
	    s->enhances = adddep(pool, &pd, s->enhances, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'S', 'u', 'g'):
	    s->suggests = adddep(pool, &pd, s->suggests, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'F', 'r', 'e'):
	    s->freshens = adddep(pool, &pd, s->freshens, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'P', 'r', 'c'):
	    s->recommends = adddep(pool, &pd, s->recommends, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 's', 'g'):
	    s->suggests = adddep(pool, &pd, s->suggests, line, 0, 0);
	    continue;
	}
      if (!with_attr)
        continue;
      switch (tag)
        {
          case CTAG('=', 'G', 'r', 'p'):
	    add_attr_localids_id (attr, last_found_pack, id_group, str2localid (attr, line + 6, 1));
	    continue;
          case CTAG('=', 'L', 'i', 'c'):
	    add_attr_localids_id (attr, last_found_pack, id_license, str2localid (attr, line + 6, 1));
	    continue;
          case CTAG('=', 'L', 'o', 'c'):
	    add_location (line + 6, s, last_found_pack);
	    continue;
          case CTAG('=', 'S', 'r', 'c'):
	    add_source (line + 6, &pd, s, last_found_pack, 1);
	    continue;
          case CTAG('=', 'S', 'i', 'z'):
	    if (split (line + 6, sp, 3) == 2)
	      {
	        add_attr_int (attr, last_found_pack, id_downloadsize, (atoi (sp[0]) + 1023) / 1024);
	        add_attr_int (attr, last_found_pack, id_installsize, (atoi (sp[1]) + 1023) / 1024);
	      }
	    continue;
          case CTAG('=', 'T', 'i', 'm'):
	    {
	      unsigned int t = atoi (line + 6);
	      if (t)
	        add_attr_int (attr, last_found_pack, id_time, t);
	    }
	    continue;
          case CTAG('=', 'K', 'w', 'd'):
	    add_attr_localids_id (attr, last_found_pack, id_keywords, str2localid (attr, line + 6, 1));
	    continue;
          case CTAG('=', 'A', 'u', 't'):
	    add_attr_blob (attr, last_found_pack, id_authors, line + 6, strlen (line + 6) + 1);
	    continue;
          case CTAG('=', 'S', 'u', 'm'):
	    add_attr_string (attr, last_found_pack, id_summary, line + 6);
	    continue;
          case CTAG('=', 'D', 'e', 's'):
	    add_attr_blob (attr, last_found_pack, id_description, line + 6, strlen (line + 6) + 1);
	    continue;
          case CTAG('=', 'E', 'u', 'l'):
	    add_attr_blob (attr, last_found_pack, id_eula, line + 6, strlen (line + 6) + 1);
	    continue;
          case CTAG('=', 'I', 'n', 's'):
	    add_attr_blob (attr, last_found_pack, id_messageins, line + 6, strlen (line + 6) + 1);
	    continue;
          case CTAG('=', 'D', 'e', 'l'):
	    add_attr_blob (attr, last_found_pack, id_messagedel, line + 6, strlen (line + 6) + 1);
	    continue;
          case CTAG('=', 'S', 'h', 'r'):
	    /* XXX Not yet handled.  Two possibilities: either include all
	       referenced data verbatim here, or write out the sharing
	       information.  */
	    continue;
          case CTAG('=', 'V', 'e', 'r'):
	    last_found_pack = 0;
	    indesc++;
	    continue;
	}
    }
  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  if (s)
    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
    
  if (pd.sources)
    {
      int i;
      for (i = 0; i < pd.nsources; i++)
        if (pd.sources[i])
	  {
	    add_source (pd.sources[i], &pd, pool->solvables + repo->start + i, i, 0);
	    free (pd.sources[i]);
	  }
      free (pd.sources);
    }
  if (pd.tmp)
    free(pd.tmp);
  free(line);
}
