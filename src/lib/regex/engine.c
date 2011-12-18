//
// engine.c
//
// Matching engine for regular expression library
//
// Ported to sanos by Michael Ringgaard.
//
// Copyright 1992, 1993, 1994, 1997 Henry Spencer.  All rights reserved.
// This software is not subject to any license of the American Telephone
// and Telegraph Company or of the Regents of the University of California.
// 
// Permission is granted to anyone to use this software for any purpose on
// any computer system, and to alter it and redistribute it, subject
// to the following restrictions:
// 
// 1. The author is not responsible for the consequences of use of this
//    software, no matter how awful, even if they arise from flaws in it.
// 
// 2. The origin of this software must not be misrepresented, either by
//    explicit claim or by omission.  Since few users ever read sources,
//    credits must appear in the documentation.
// 
// 3. Altered versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.  Since few users
//    ever read sources, credits must appear in the documentation.
// 
// 4. This notice may not be removed or altered.
// 

//
// The matching engine and friends.  This file is #included by regexec.c
// after suitable #defines of a variety of macros used herein, so that
// different state representations can be used without duplicating masses
// of code.
//

#ifdef SNAMES
#define matcher smatcher
#define fast    sfast
#define slow    sslow
#define dissect sdissect
#define backref sbackref
#define step    sstep
#define print   sprint
#define at      sat
#define match   smat
#endif

#ifdef LNAMES
#define matcher lmatcher
#define fast    lfast
#define slow    lslow
#define dissect ldissect
#define backref lbackref
#define step    lstep
#define print   lprint
#define at      lat
#define match   lmat
#endif

// Another structure passed up and down to avoid zillions of parameters
struct match 
{
  struct re_guts *g;
  int eflags;
  regmatch_t *pmatch; // [nsub+1] (0 element unused)
  char *offp;         // offsets work from here
  char *beginp;       // start of string -- virtual NUL precedes
  char *endp;         // end of string -- virtual NUL here
  char *coldp;        // can be no match starting before here
  char **lastpos;     // [nplus+1]
  STATEVARS;
  states st;          // current states
  states fresh;       // states for a fresh start
  states tmp;         // temporary
  states empty;       // empty set of states
};

static int matcher(struct re_guts *g, char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
static char *dissect(struct match *m, char *start, char *stop, sopno startst, sopno stopst);
static char *backref(struct match *m, char *start, char *stop, sopno startst, sopno stopst, sopno lev);
static char *fast(struct match *m, char *start, char *stop, sopno startst, sopno stopst);
static char *slow(struct match *m, char *start, char *stop, sopno startst, sopno stopst);
static states step(struct re_guts *g, sopno start, sopno stop, states bef, int ch, states aft);

#define BOL         (OUT + 1)
#define EOL         (BOL + 1)
#define BOLEOL      (BOL + 2)
#define NOTHING     (BOL + 3)
#define BOW         (BOL + 4)
#define EOW         (BOL + 5)
#define CODEMAX     (BOL + 5)   // highest code used
#define NONCHAR(c)  ((c) > CHAR_MAX)
#define NNONCHAR    (CODEMAX-CHAR_MAX)

#ifdef REDEBUG
static void print(struct match *m, char *caption, states st, int ch, FILE *d);
static void at(struct match *m, char *title, char *start, char *stop, sopno startst, sopno stopst);
static char *pchar(int ch);
#endif

#ifdef REDEBUG
#define SP(t, s, c) print(m, t, s, c, stdout)
#define AT(t, p1, p2, s1, s2) at(m, t, p1, p2, s1, s2)
#define NOTE(str) { if (m->eflags & REG_TRACE) printf("=%s\n", (str)); }
#else
#define SP(t, s, c) // nothing
#define AT(t, p1, p2, s1, s2) // nothing
#define NOTE(s) // nothing
#endif

//
// matcher - the actual matching engine
//
static int matcher(struct re_guts *g, char *string, size_t nmatch, regmatch_t pmatch[], int eflags)
{
  char *endp;
  int i;
  struct match mv;
  struct match *m = &mv;
  char *dp;
  const sopno gf = g->firststate + 1;  // +1 for OEND
  const sopno gl = g->laststate;
  char *start;
  char *stop;

  // Simplify the situation where possible
  if (g->cflags & REG_NOSUB) nmatch = 0;
  if (eflags & REG_STARTEND) 
  {
    start = string + pmatch[0].rm_so;
    stop = string + pmatch[0].rm_eo;
  } 
  else 
  {
    start = string;
    stop = start + strlen(start);
  }
  if (stop < start) return REG_INVARG;

  // Prescreening; this does wonders for this rather slow code
  if (g->must != NULL) 
  {
    for (dp = start; dp < stop; dp++)
      if (*dp == g->must[0] && stop - dp >= g->mlen && memcmp(dp, g->must, (size_t) g->mlen) == 0) 
        break;
    if (dp == stop) return REG_NOMATCH;  // we didn't find g->must
  }

  // Match struct setup
  m->g = g;
  m->eflags = eflags;
  m->pmatch = NULL;
  m->lastpos = NULL;
  m->offp = string;
  m->beginp = start;
  m->endp = stop;
  STATESETUP(m, 4);
  SETUP(m->st);
  SETUP(m->fresh);
  SETUP(m->tmp);
  SETUP(m->empty);
  CLEAR(m->empty);

  // This loop does only one repetition except for backrefs
  for (;;) 
  {
    endp = fast(m, start, stop, gf, gl);
    if (endp == NULL) 
    {
      // a miss
      STATETEARDOWN(m);
      return REG_NOMATCH;
    }
    if (nmatch == 0 && !g->backrefs) break; // no further info needed

    // where?
    assert(m->coldp != NULL);
    for (;;) 
    {
      NOTE("finding start");
      endp = slow(m, m->coldp, stop, gf, gl);
      if (endp != NULL) break;
      assert(m->coldp < m->endp);
      m->coldp++;
    }
    if (nmatch == 1 && !g->backrefs) break; // no further info needed

    // oh my, he wants the subexpressions...
    if (m->pmatch == NULL)
    {
      m->pmatch = (regmatch_t *) malloc((m->g->nsub + 1) * sizeof(regmatch_t));
    }
    if (m->pmatch == NULL) 
    {
      STATETEARDOWN(m);
      return REG_ESPACE;
    }
    for (i = 1; i <= m->g->nsub; i++)
    {
      m->pmatch[i].rm_so = m->pmatch[i].rm_eo = -1;
    }
    if (!g->backrefs && !(m->eflags & REG_BACKR)) 
    {
      NOTE("dissecting");
      dp = dissect(m, m->coldp, endp, gf, gl);
    } 
    else 
    {
      if (g->nplus > 0 && m->lastpos == NULL)
      {
        m->lastpos = (char **) malloc((g->nplus + 1) * sizeof(char *));
      }
      if (g->nplus > 0 && m->lastpos == NULL) 
      {
        free(m->pmatch);
        STATETEARDOWN(m);
        return REG_ESPACE;
      }
      NOTE("backref dissect");
      dp = backref(m, m->coldp, endp, gf, gl, (sopno) 0);
    }
    if (dp != NULL) break;

    // uh-oh... we couldn't find a subexpression-level match
    assert(g->backrefs);  // must be back references doing it
    assert(g->nplus == 0 || m->lastpos != NULL);
    for (;;) 
    {
      if (dp != NULL || endp <= m->coldp) break;    // defeat
      NOTE("backoff");
      endp = slow(m, m->coldp, endp-1, gf, gl);
      if (endp == NULL) break;    // defeat
      // try it on a shorter possibility
#ifndef NDEBUG
      for (i = 1; i <= m->g->nsub; i++) 
      {
        assert(m->pmatch[i].rm_so == -1);
        assert(m->pmatch[i].rm_eo == -1);
      }
#endif
      NOTE("backoff dissect");
      dp = backref(m, m->coldp, endp, gf, gl, (sopno) 0);
    }
    assert(dp == NULL || dp == endp);
    if (dp != NULL) break; // found a shorter one
      
    // Despite initial appearances, there is no match here
    NOTE("false alarm");
    start = m->coldp + 1; // recycle starting later
    assert(start <= stop);
  }

  // Fill in the details if requested
  if (nmatch > 0) 
  {
    pmatch[0].rm_so = m->coldp - m->offp;
    pmatch[0].rm_eo = endp - m->offp;
  }
  if (nmatch > 1) 
  {
    assert(m->pmatch != NULL);
    for (i = 1; i < nmatch; i++)
    {
      if (i <= m->g->nsub)
        pmatch[i] = m->pmatch[i];
      else 
      {
        pmatch[i].rm_so = -1;
        pmatch[i].rm_eo = -1;
      }
    }
  }

  if (m->pmatch != NULL) free(m->pmatch);
  if (m->lastpos != NULL) free(m->lastpos);
  STATETEARDOWN(m);
  return 0;
}

//
// dissect - figure out what matched what, no back references
//
static char *dissect(struct match *m, char *start, char *stop, sopno startst, sopno stopst)
{
  int i;
  sopno ss;     // start sop of current subRE
  sopno es;     // end sop of current subRE
  char *sp;     // start of string matched by it
  char *stp;    // string matched by it cannot pass here
  char *rest;   // start of rest of string
  char *tail;   // string unmatched by rest of RE
  sopno ssub;   // start sop of subsubRE
  sopno esub;   // end sop of subsubRE
  char *ssp;    // start of string matched by subsubRE
  char *sep;    // end of string matched by subsubRE
  char *oldssp; // previous ssp
  char *dp;

  AT("diss", start, stop, startst, stopst);
  sp = start;
  for (ss = startst; ss < stopst; ss = es) 
  {
    // Identify end of subRE
    es = ss;
    switch (OP(m->g->strip[es])) 
    {
      case OPLUS_:
      case OQUEST_:
        es += OPND(m->g->strip[es]);
        break;

      case OCH_:
        while (OP(m->g->strip[es]) != O_CH) es += OPND(m->g->strip[es]);
        break;
    }
    es++;

    // Figure out what it matched
    switch (OP(m->g->strip[ss])) 
    {
      case OEND:
        assert(nope);
        break;

      case OCHAR:
        sp++;
        break;

      case OBOL:
      case OEOL:
      case OBOW:
      case OEOW:
        break;

      case OANY:
      case OANYOF:
        sp++;
        break;

      case OBACK_:
      case O_BACK:
        assert(nope);
        break;
    
      // cases where length of match is hard to find
      case OQUEST_:
        stp = stop;
        for (;;) 
        {
          // how long could this one be?
          rest = slow(m, sp, stp, ss, es);
          assert(rest != NULL); // it did match
          // could the rest match the rest?
          tail = slow(m, rest, stop, es, stopst);
          if (tail == stop) break;    // yes!
          // no -- try a shorter match for this one
          stp = rest - 1;
          assert(stp >= sp);  // it did work
        }
        ssub = ss + 1;
        esub = es - 1;
        // did innards match?
        if (slow(m, sp, rest, ssub, esub) != NULL) 
        {
          dp = dissect(m, sp, rest, ssub, esub);
          assert(dp == rest);
        } else    // no
          assert(sp == rest);
        sp = rest;
        break;

      case OPLUS_:
        stp = stop;
        for (;;) 
        {
          // how long could this one be?
          rest = slow(m, sp, stp, ss, es);
          assert(rest != NULL); // it did match
          // could the rest match the rest?
          tail = slow(m, rest, stop, es, stopst);
          if (tail == stop) break;    // yes!
          // no -- try a shorter match for this one
          stp = rest - 1;
          assert(stp >= sp);  // it did work
        }
        ssub = ss + 1;
        esub = es - 1;
        ssp = sp;
        oldssp = ssp;
        for (;;) 
        {  
          // find last match of innards
          sep = slow(m, ssp, rest, ssub, esub);
          if (sep == NULL || sep == ssp) break;  // failed or matched null
          oldssp = ssp; // on to next try
          ssp = sep;
        }
        if (sep == NULL) 
        {
          // last successful match
          sep = ssp;
          ssp = oldssp;
        }
        assert(sep == rest);  // must exhaust substring
        assert(slow(m, ssp, sep, ssub, esub) == rest);
        dp = dissect(m, ssp, sep, ssub, esub);
        assert(dp == sep);
        sp = rest;
        break;

      case OCH_:
        stp = stop;
        for (;;) 
        {
          // how long could this one be?
          rest = slow(m, sp, stp, ss, es);
          assert(rest != NULL); // it did match
          // could the rest match the rest?
          tail = slow(m, rest, stop, es, stopst);
          if (tail == stop) break;    // yes!
          // no -- try a shorter match for this one
          stp = rest - 1;
          assert(stp >= sp);  // it did work
        }

        ssub = ss + 1;
        esub = ss + OPND(m->g->strip[ss]) - 1;
        assert(OP(m->g->strip[esub]) == OOR1);
        for (;;) 
        {  
          // find first matching branch
          if (slow(m, sp, rest, ssub, esub) == rest) break;  // it matched all of it
          // that one missed, try next one
          assert(OP(m->g->strip[esub]) == OOR1);
          esub++;
          assert(OP(m->g->strip[esub]) == OOR2);
          ssub = esub + 1;
          esub += OPND(m->g->strip[esub]);
          if (OP(m->g->strip[esub]) == OOR2)
            esub--;
          else
            assert(OP(m->g->strip[esub]) == O_CH);
        }
        dp = dissect(m, sp, rest, ssub, esub);
        assert(dp == rest);
        sp = rest;
        break;

      case O_PLUS:
      case O_QUEST:
      case OOR1:
      case OOR2:
      case O_CH:
        assert(nope);
        break;
 
      case OLPAREN:
        i = OPND(m->g->strip[ss]);
        assert(0 < i && i <= m->g->nsub);
        m->pmatch[i].rm_so = sp - m->offp;
        break;
  
      case ORPAREN:
        i = OPND(m->g->strip[ss]);
        assert(0 < i && i <= m->g->nsub);
        m->pmatch[i].rm_eo = sp - m->offp;
        break;
  
      default:    // uh oh
        assert(nope);
        break;
    }
  }

  assert(sp == stop);
  return sp;
}

//
// backref - figure out what matched what, figuring in back references
// 
static char *backref(struct match *m, char *start, char *stop, sopno startst, sopno stopst, sopno lev)
{
  int i;
  sopno ss;     // start sop of current subRE
  char *sp;     // start of string matched by it
  sopno ssub;   // start sop of subsubRE
  sopno esub;   // end sop of subsubRE
  char *ssp;   // start of string matched by subsubRE
  char *dp;
  size_t len;
  int hard;
  sop s;
  regoff_t offsave;
  cset *cs;

  AT("back", start, stop, startst, stopst);
  sp = start;

  // Get as far as we can with easy stuff
  hard = 0;
  for (ss = startst; !hard && ss < stopst; ss++)
  {
    switch (OP(s = m->g->strip[ss])) 
    {
      case OCHAR:
        if (sp == stop || *sp++ != (char) OPND(s)) return NULL;
        break;

      case OANY:
        if (sp == stop) return NULL;
        sp++;
        break;

      case OANYOF:
        cs = &m->g->sets[OPND(s)];
        if (sp == stop || !CHIN(cs, *sp++)) return NULL;
        break;
  
      case OBOL:
        if ((sp == m->beginp && !(m->eflags & REG_NOTBOL)) ||
            (sp < m->endp && *(sp - 1) == '\n' &&
              (m->g->cflags & REG_NEWLINE)))
        { 
          // yes 
        }
        else
          return NULL;
        break;

      case OEOL:
        if ((sp == m->endp && !(m->eflags & REG_NOTEOL)) ||
            (sp < m->endp && *sp == '\n' &&
            (m->g->cflags & REG_NEWLINE)) )
        { 
          // yes 
        }
        else
          return NULL;
        break;

      case OBOW:
        if (((sp == m->beginp && !(m->eflags & REG_NOTBOL)) ||
            (sp < m->endp && *(sp-1) == '\n' && (m->g->cflags&REG_NEWLINE)) ||
            (sp > m->beginp && !ISWORD(*(sp - 1)))) && (sp < m->endp && ISWORD(*sp)))
        { 
          // yes 
        }
        else
          return NULL;
        break;

      case OEOW:
        if (((sp == m->endp && !(m->eflags & REG_NOTEOL)) ||
            (sp < m->endp && *sp == '\n' && (m->g->cflags & REG_NEWLINE)) ||
            (sp < m->endp && !ISWORD(*sp)) ) && (sp > m->beginp && ISWORD(*(sp - 1))))
        { 
          // yes 
        }
        else
          return NULL;
        break;

      case O_QUEST:
        break;

      case OOR1:  // matches null but needs to skip
        ss++;
        s = m->g->strip[ss];
        do {
          assert(OP(s) == OOR2);
          ss += OPND(s);
        } while (OP(s = m->g->strip[ss]) != O_CH);
        // note that the ss++ gets us past the O_CH
        break;

      default:  // have to make a choice
        hard = 1;
        break;
    }
  }
  if (!hard) 
  {    
    // That was it!
    if (sp != stop) return NULL;
    return sp;
  }
  ss--;     // adjust for the for's final increment

  // The hard stuff
  AT("hard", sp, stop, ss, stopst);
  s = m->g->strip[ss];
  switch (OP(s)) 
  {
    case OBACK_:    // the vilest depths
      i = OPND(s);
      assert(0 < i && i <= m->g->nsub);
      if (m->pmatch[i].rm_eo == -1) return NULL;
      assert(m->pmatch[i].rm_so != -1);
      len = m->pmatch[i].rm_eo - m->pmatch[i].rm_so;
      assert(stop - m->beginp >= len);
      if (sp > stop - len) return NULL; // not enough left to match
      ssp = m->offp + m->pmatch[i].rm_so;
      if (memcmp(sp, ssp, len) != 0) return NULL;
      while (m->g->strip[ss] != SOP(O_BACK, i)) ss++;
      return backref(m, sp + len, stop, ss + 1, stopst, lev);
      break;

    case OQUEST_:   // to null or not
      dp = backref(m, sp, stop, ss + 1, stopst, lev);
      if (dp != NULL) return dp; // not
      return backref(m, sp, stop, ss + OPND(s) + 1, stopst, lev);
      break;

    case OPLUS_:
      assert(m->lastpos != NULL);
      assert(lev + 1 <= m->g->nplus);
      m->lastpos[lev + 1] = sp;
      return backref(m, sp, stop, ss + 1, stopst, lev + 1);
      break;

    case O_PLUS:
      if (sp == m->lastpos[lev])
      {
        // last pass matched null
        return backref(m, sp, stop, ss + 1, stopst, lev - 1);
      }
      // try another pass
      m->lastpos[lev] = sp;
      dp = backref(m, sp, stop, ss - OPND(s) + 1, stopst, lev);
      if (dp == NULL)
        return backref(m, sp, stop, ss + 1, stopst, lev - 1);
      else
        return dp;
      break;

    case OCH_:    // find the right one, if any
      ssub = ss + 1;
      esub = ss + OPND(s) - 1;
      assert(OP(m->g->strip[esub]) == OOR1);
      for (;;) 
      {  
        // find first matching branch
        dp = backref(m, sp, stop, ssub, esub, lev);
        if (dp != NULL) return dp;
        // that one missed, try next one
        if (OP(m->g->strip[esub]) == O_CH) return NULL; // there is none
        esub++;
        assert(OP(m->g->strip[esub]) == OOR2);
        ssub = esub + 1;
        esub += OPND(m->g->strip[esub]);
        if (OP(m->g->strip[esub]) == OOR2)
          esub--;
        else
          assert(OP(m->g->strip[esub]) == O_CH);
      }
      break;

    case OLPAREN:   // must undo assignment if rest fails
      i = OPND(s);
      assert(0 < i && i <= m->g->nsub);
      offsave = m->pmatch[i].rm_so;
      m->pmatch[i].rm_so = sp - m->offp;
      dp = backref(m, sp, stop, ss + 1, stopst, lev);
      if (dp != NULL) return dp;
      m->pmatch[i].rm_so = offsave;
      return NULL;
      break;
  
    case ORPAREN:   // must undo assignment if rest fails
      i = OPND(s);
      assert(0 < i && i <= m->g->nsub);
      offsave = m->pmatch[i].rm_eo;
      m->pmatch[i].rm_eo = sp - m->offp;
      dp = backref(m, sp, stop, ss + 1, stopst, lev);
      if (dp != NULL) return dp;
      m->pmatch[i].rm_eo = offsave;
      return NULL;
      break;

    default:    // uh oh
      assert(nope);
      break;
  }

  // "can't happen"
  assert(nope);
  // NOTREACHED
  return NULL; // dummy
}

//
// fast - step through the string at top speed
//
static char *fast(struct match *m, char *start, char *stop, sopno startst, sopno stopst)
{
  states st = m->st;
  states fresh = m->fresh;
  states tmp = m->tmp;
  char *p = start;
  int c = (start == m->beginp) ? OUT : *(start - 1);
  int lastc; // previous c
  int flagch;
  int i;
  char *coldp; // last p after which no match was underway

  CLEAR(st);
  SET1(st, startst);
  st = step(m->g, startst, stopst, st, NOTHING, st);
  ASSIGN(fresh, st);
  SP("start", st, *p);
  coldp = NULL;
  for (;;) 
  {
    // next character
    lastc = c;
    c = (p == m->endp) ? OUT : *p;
    if (EQ(st, fresh)) coldp = p;

    // is there an EOL and/or BOL between lastc and c?
    flagch = '\0';
    i = 0;
    if ((lastc == '\n' && m->g->cflags & REG_NEWLINE) ||
        (lastc == OUT && !(m->eflags & REG_NOTBOL))) 
    {
      flagch = BOL;
      i = m->g->nbol;
    }
    if ((c == '\n' && m->g->cflags&REG_NEWLINE) ||
        (c == OUT && !(m->eflags&REG_NOTEOL))) 
    {
      flagch = (flagch == BOL) ? BOLEOL : EOL;
      i += m->g->neol;
    }
    if (i != 0) 
    {
      for (; i > 0; i--) st = step(m->g, startst, stopst, st, flagch, st);
      SP("boleol", st, c);
    }

    // how about a word boundary?
    if ((flagch == BOL || (lastc != OUT && !ISWORD(lastc))) &&
        (c != OUT && ISWORD(c))) 
    {
      flagch = BOW;
    }
    if ((lastc != OUT && ISWORD(lastc)) &&
        (flagch == EOL || (c != OUT && !ISWORD(c)))) 
    {
      flagch = EOW;
    }
    if (flagch == BOW || flagch == EOW) 
    {
      st = step(m->g, startst, stopst, st, flagch, st);
      SP("boweow", st, c);
    }

    // are we done?
    if (ISSET(st, stopst) || p == stop) break;    // NOTE BREAK OUT

    // no, we must deal with this character
    ASSIGN(tmp, st);
    ASSIGN(st, fresh);
    assert(c != OUT);
    st = step(m->g, startst, stopst, tmp, c, st);
    SP("aft", st, c);
    assert(EQ(step(m->g, startst, stopst, st, NOTHING, st), st));
    p++;
  }

  assert(coldp != NULL);
  m->coldp = coldp;
  if (ISSET(st, stopst)) 
    return p + 1;
  else
    return NULL;
}

//
// slow - step through the string more deliberately
//
static char *slow(struct match *m, char *start, char *stop, sopno startst, sopno stopst)
{
  states st = m->st;
  states empty = m->empty;
  states tmp = m->tmp;
  char *p = start;
  int c = (start == m->beginp) ? OUT : *(start - 1);
  int lastc; // previous c
  int flagch;
  int i;
  char *matchp;  // last p at which a match ended

  AT("slow", start, stop, startst, stopst);
  CLEAR(st);
  SET1(st, startst);
  SP("sstart", st, *p);
  st = step(m->g, startst, stopst, st, NOTHING, st);
  matchp = NULL;
  for (;;) 
  {
    // next character
    lastc = c;
    c = (p == m->endp) ? OUT : *p;

    // is there an EOL and/or BOL between lastc and c?
    flagch = '\0';
    i = 0;
    if ((lastc == '\n' && m->g->cflags & REG_NEWLINE) ||
        (lastc == OUT && !(m->eflags&REG_NOTBOL))) 
    {
      flagch = BOL;
      i = m->g->nbol;
    }
    if ((c == '\n' && m->g->cflags&REG_NEWLINE) ||
        (c == OUT && !(m->eflags&REG_NOTEOL))) 
    {
      flagch = (flagch == BOL) ? BOLEOL : EOL;
      i += m->g->neol;
    }
    if (i != 0) 
    {
      for (; i > 0; i--) st = step(m->g, startst, stopst, st, flagch, st);
      SP("sboleol", st, c);
    }

    // how about a word boundary?
    if ((flagch == BOL || (lastc != OUT && !ISWORD(lastc))) &&
        (c != OUT && ISWORD(c))) 
    {
      flagch = BOW;
    }
    if ((lastc != OUT && ISWORD(lastc)) &&
        (flagch == EOL || (c != OUT && !ISWORD(c)))) 
    {
      flagch = EOW;
    }
    if (flagch == BOW || flagch == EOW) 
    {
      st = step(m->g, startst, stopst, st, flagch, st);
      SP("sboweow", st, c);
    }

    // are we done?
    if (ISSET(st, stopst)) matchp = p;
    if (EQ(st, empty) || p == stop) break;    // NOTE BREAK OUT

    // no, we must deal with this character
    ASSIGN(tmp, st);
    ASSIGN(st, empty);
    assert(c != OUT);
    st = step(m->g, startst, stopst, tmp, c, st);
    SP("saft", st, c);
    assert(EQ(step(m->g, startst, stopst, st, NOTHING, st), st));
    p++;
  }

  return matchp;
}

//
// step - map set of states reachable before char to set reachable after
//
static states step(struct re_guts *g, sopno start, sopno stop, states bef, int ch, states aft)
{
  cset *cs;
  sop s;
  sopno pc;
  onestate here;   // note, macros know this name
  sopno look;
  long i;

  for (pc = start, INIT(here, pc); pc != stop; pc++, INC(here)) 
  {
    s = g->strip[pc];
    switch (OP(s)) 
    {
      case OEND:
        assert(pc == stop - 1);
        break;

      case OCHAR:
        // only characters can match
        assert(!NONCHAR(ch) || ch != (char) OPND(s));
        if (ch == (char) OPND(s)) FWD(aft, bef, 1);
        break;
 
      case OBOL:
        if (ch == BOL || ch == BOLEOL) FWD(aft, bef, 1);
        break;

      case OEOL:
        if (ch == EOL || ch == BOLEOL) FWD(aft, bef, 1);
        break;
   
      case OBOW:
        if (ch == BOW) FWD(aft, bef, 1);
        break;
   
      case OEOW:
        if (ch == EOW) FWD(aft, bef, 1);
        break;
  
      case OANY:
        if (!NONCHAR(ch)) FWD(aft, bef, 1);
        break;
 
      case OANYOF:
        cs = &g->sets[OPND(s)];
        if (!NONCHAR(ch) && CHIN(cs, ch)) FWD(aft, bef, 1);
        break;
 
      case OBACK_:    // ignored here
      case O_BACK:
        FWD(aft, aft, 1);
        break;

      case OPLUS_:    // forward, this is just an empty
        FWD(aft, aft, 1);
        break;

      case O_PLUS:    // both forward and back
        FWD(aft, aft, 1);
        i = ISSETBACK(aft, OPND(s));
        BACK(aft, aft, OPND(s));
        if (!i && ISSETBACK(aft, OPND(s))) 
        {
          // oho, must reconsider loop body
          pc -= OPND(s) + 1;
          INIT(here, pc);
        }
        break;

      case OQUEST_:   // two branches, both forward
        FWD(aft, aft, 1);
        FWD(aft, aft, OPND(s));
        break;

      case O_QUEST:   // just an empty
        FWD(aft, aft, 1);
        break;

      case OLPAREN:   // not significant here
      case ORPAREN:
        FWD(aft, aft, 1);
        break;

      case OCH_:    // mark the first two branches
        FWD(aft, aft, 1);
        assert(OP(g->strip[pc + OPND(s)]) == OOR2);
        FWD(aft, aft, OPND(s));
        break;

      case OOR1:    // done a branch, find the O_CH
        if (ISSTATEIN(aft, here)) 
        {
          for (look = 1; OP(s = g->strip[pc+look]) != O_CH; look += OPND(s)) assert(OP(s) == OOR2);
          FWD(aft, aft, look);
        }
        break;

      case OOR2:    // propagate OCH_'s marking
        FWD(aft, aft, 1);
        if (OP(g->strip[pc+OPND(s)]) != O_CH) 
        {
          assert(OP(g->strip[pc+OPND(s)]) == OOR2);
          FWD(aft, aft, OPND(s));
        }
        break;
  
      case O_CH:    // just empty
        FWD(aft, aft, 1);
        break;
  
      default:    // ooooops...
        assert(nope);
        break;
    }
  }

  return aft;
}

#ifdef REDEBUG
//
// print - print a set of states
//
static void print(struct match *m, char *caption, states st, int ch, FILE *d)
{
  struct re_guts *g = m->g;
  int i;
  int first = 1;

  if (!(m->eflags & REG_TRACE)) return;

  fprintf(d, "%s", caption);
  if (ch != '\0') fprintf(d, " %s", pchar(ch));
  for (i = 0; i < g->nstates; i++)
  {
    if (ISSET(st, i)) 
    {
      fprintf(d, "%s%d", (first) ? "\t" : ", ", i);
      first = 0;
    }
  }
  fprintf(d, "\n");
}

// 
// at - print current situation
// 
static void at(struct match *m, char *title, char *start, char *stop, sopno startst, sopno stopst)
{
  if (!(m->eflags & REG_TRACE)) return;

  printf("%s %s-", title, pchar(*start));
  printf("%s ", pchar(*stop));
  printf("%ld-%ld\n", (long) startst, (long) stopst);
}

#ifndef PCHARDONE
#define PCHARDONE // never again

//
// pchar - make a character printable
//
// Is this identical to regchar() over in debug.c?  Well, yes.  But a
// duplicate here avoids having a debugging-capable regexec.o tied to
// a matching debug.o, and this is convenient.  It all disappears in
// the non-debug compilation anyway, so it doesn't matter much.
static char *pchar(int ch)
{
  static char pbuf[10];

  if (isprint(ch) || ch == ' ') 
    sprintf(pbuf, "%c", ch);
  else
    sprintf(pbuf, "\\%o", ch);
  return pbuf;
}
#endif
#endif

#undef  matcher
#undef  fast
#undef  slow
#undef  dissect
#undef  backref
#undef  step
#undef  print
#undef  at
#undef  match
