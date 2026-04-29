/*
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
*******************************************************************************
**
** This file contains the implementation of the "fossil timer" command and
** its supporting helpers.  Time tracking is implemented as a first-class
** artifact type (CFTYPE_TIMER, manifest Y-card) and mirrored into the
** EVENT table with type='m' so it appears on the regular timeline.
*/
#include "config.h"
#include "timer.h"
#include <assert.h>
#include <string.h>

/*
** Build a timer artifact and store it in the repository.  The artifact
** carries the bare minimum of cards: D (event time), U (current user),
** Y (the action), Z (checksum), plus an optional C card with a free-form
** comment.  If zWhen is NULL the current time is used; otherwise zWhen
** must be parseable by date_in_standard_format() (typical input: "now",
** "YYYY-MM-DDTHH:MM:SS").  Returns 1 on success, 0 if manifest_crosslink
** rejected the new artifact.
*/
static int timer_create_artifact(const char *zAction,
                                  const char *zComment,
                                  const char *zWhen){
  Blob art, cksum;
  char *zDate;
  int nrid;
  int n;

  blob_init(&art, 0, 0);
  db_begin_transaction();

  if( zComment && zComment[0] ){
    while( fossil_isspace(zComment[0]) ) zComment++;
    n = (int)strlen(zComment);
    while( n>0 && fossil_isspace(zComment[n-1]) ){ n--; }
    if( n>0 ){
      blob_appendf(&art, "C %#F\n", n, zComment);
    }
  }

  zDate = date_in_standard_format(zWhen ? zWhen : "now");
  if( zDate==0 ){
    db_end_transaction(1);
    return 0;
  }
  blob_appendf(&art, "D %s\n", zDate);
  free(zDate);

  if( !login_is_nobody() ){
    blob_appendf(&art, "U %F\n", login_name());
  }

  blob_appendf(&art, "Y %s\n", zAction);

  md5sum_blob(&art, &cksum);
  blob_appendf(&art, "Z %b\n", &cksum);
  blob_reset(&cksum);

  nrid = content_put(&art);
  db_add_unsent(nrid);
  if( manifest_crosslink(nrid, &art, MC_NONE)==0 ){
    db_end_transaction(1);
    return 0;
  }
  assert( blob_is_reset(&art) );
  db_end_transaction(0);
  return 1;
}

/*
** SETTING: timer-min-seconds  width=8 default=0
**
** Minimum duration in seconds for a timer segment to be recorded.
** Segments shorter than this are silently discarded by "fossil timer
** stop" (which also purges the matching start) and rejected by
** "fossil timer add".  Set to 0 (default) to record every segment.
*/

/*
** Return the configured minimum-segment threshold in seconds.  Negative
** values are clamped to 0.
*/
static int timer_min_seconds(void){
  int v = db_get_int("timer-min-seconds", 0);
  return v<0 ? 0 : v;
}

/*
** Emit a technote tagged "timer:audit" on the timeline so that
** destructive timer operations leave a visible trail.  The technote
** body is empty; the message goes into the C card so it shows up as
** the timeline comment.
*/
static void timer_audit(const char *zMessage){
  Blob nullBody;
  char *zNow;
  if( zMessage==0 || zMessage[0]==0 ) return;
  zNow = date_in_standard_format("now");
  if( zNow==0 ) return;
  blob_init(&nullBody, "", 0);
  event_cmd_commit(zNow, 0, &nullBody, "text/plain",
                   zMessage, "timer:audit", "#fef");
  blob_reset(&nullBody);
  free(zNow);
}

/*
** Resolve a user-supplied identifier (full or short hash, or numeric rid)
** into the rid of a timer artifact.  Returns 0 if not found or if the
** target is not a timer artifact.
*/
static int timer_resolve_rid(const char *zId){
  int rid = 0;
  if( zId==0 || zId[0]==0 ) return 0;
  /* Plain decimal numbers are treated as raw rids - useful for the
  ** segment-list rendered by /timer where each row carries its rid. */
  if( fossil_isdigit(zId[0]) ){
    int allDigits = 1;
    int i;
    for(i=0; zId[i]; i++){
      if( !fossil_isdigit(zId[i]) ){ allDigits = 0; break; }
    }
    if( allDigits ) rid = atoi(zId);
  }
  if( rid<=0 ){
    rid = name_to_typed_rid(zId, "*");
  }
  if( rid<=0 ) return 0;
  if( !db_exists("SELECT 1 FROM timer WHERE rid=%d", rid) ) return 0;
  return rid;
}

/*
** Move a single timer artifact (by rid) into the graveyard.  Returns 1
** on success, 0 on failure.  Wraps purge_artifact_list().
*/
static int timer_purge_one(int rid, const char *zNote){
  int rc;
  int stopRid = 0;
  int isSessionStart;
  db_begin_transaction();
  /* When rid is a session start (it is the PK of a timer row), we want
  ** to drag the matching stop_rid down with it so the BLOB does not
  ** retain a half-session.  When rid is a stop pointed to by some
  ** timer row, "purging" it should re-open the session by clearing
  ** stop_rid.  Both cases handled here so callers stay simple. */
  isSessionStart = db_exists("SELECT 1 FROM timer WHERE rid=%d", rid);
  if( isSessionStart ){
    stopRid = db_int(0, "SELECT stop_rid FROM timer WHERE rid=%d", rid);
    db_multi_exec("DELETE FROM timer WHERE rid=%d", rid);
  }else{
    db_multi_exec(
      "UPDATE timer SET stop_rid=NULL, stop_mtime=NULL WHERE stop_rid=%d",
      rid);
  }
  /* purge_artifact_list() leaks related TEMP tables (`<list>_tickets`,
  ** `<list>_files`, `<list>_tags`) on purpose; drop pre-existing ones
  ** so successive calls in one connection don't collide. */
  db_multi_exec(
    "DROP TABLE IF EXISTS timer_purgelist;"
    "DROP TABLE IF EXISTS timer_purgelist_tickets;"
    "DROP TABLE IF EXISTS timer_purgelist_files;"
    "DROP TABLE IF EXISTS timer_purgelist_tags;"
    "CREATE TEMP TABLE timer_purgelist(id INTEGER PRIMARY KEY);"
    "INSERT INTO timer_purgelist VALUES(%d);", rid);
  if( stopRid>0 ){
    db_multi_exec("INSERT OR IGNORE INTO timer_purgelist VALUES(%d);",
                  stopRid);
  }
  rc = purge_artifact_list("timer_purgelist", zNote ? zNote : "", 0);
  db_end_transaction(0);
  return rc==0 ? 1 : 0;
}

/*
** Walk timer sessions (one row per start↔stop pair) and print a report.
** An open session (stop_rid IS NULL) is counted up to "now" and flagged
** as RUNNING.
*/
static void timer_aggregate(int verbose){
  Stmt q;
  double totalSecs = 0.0;
  int    pairs     = 0;
  int    nRunning  = 0;
  double nowJd     = db_double(0.0, "SELECT julianday('now')");

  db_prepare(&q,
    "SELECT start_mtime AS startJd, "
    "       stop_mtime  AS stopJd, "
    "       comment, datetime(start_mtime), "
    "       stop_rid IS NULL AS running "
    "  FROM timer "
    " ORDER BY start_mtime ASC, rid ASC");

  while( db_step(&q)==SQLITE_ROW ){
    double sJd = db_column_double(&q, 0);
    double eJd = db_column_double(&q, 1);
    const char *zComment = db_column_text(&q, 2);
    const char *zWhen    = db_column_text(&q, 3);
    int    running       = db_column_int(&q, 4);
    double secs;
    int    mn;

    if( running ){
      secs = (nowJd - sJd) * 86400.0;
      nRunning++;
    }else{
      secs = (eJd - sJd) * 86400.0;
      pairs++;
    }
    totalSecs += secs;
    mn = (int)(secs/60.0 + 0.5);

    if( verbose ){
      if( running ){
        fossil_print("  RUNNING  start=%s  %s   [+%dh %dm]\n",
                     zWhen, zComment ? zComment : "",
                     mn/60, mn%60);
      }else{
        fossil_print("  CLOSED   start=%s  %s   [+%dh %dm]\n",
                     zWhen, zComment ? zComment : "",
                     mn/60, mn%60);
      }
    }
  }
  db_finalize(&q);

  {
    int totalMin = (int)(totalSecs/60.0 + 0.5);
    fossil_print("\n  Closed segments: %d", pairs);
    if( nRunning>0 ) fossil_print(" (+%d running)", nRunning);
    fossil_print("\n  Total: %dh %dm\n",
                 totalMin/60, totalMin%60);
  }
}

/*
** Determine whether a timer session is currently open.
** Returns 1 if there is a row in `timer` with stop_rid IS NULL.
** Sets *pLastJd to the julianday of that session's start, if any.
*/
static int timer_running(double *pLastJd){
  Stmt q;
  int isRunning = 0;
  if( pLastJd ) *pLastJd = 0.0;

  db_prepare(&q,
    "SELECT start_mtime "
    "  FROM timer "
    " WHERE stop_rid IS NULL "
    " ORDER BY start_mtime DESC, rid DESC "
    " LIMIT 1");
  if( db_step(&q)==SQLITE_ROW ){
    isRunning = 1;
    if( pLastJd ) *pLastJd = db_column_double(&q, 0);
  }
  db_finalize(&q);
  return isRunning;
}

/*
** "fossil timer start ?-c COMMENT?"
*/
static void timer_start_cmd(void){
  const char *zComment = find_option("comment","c",1);

  verify_all_options();
  user_select();
  if( timer_running(0) ){
    fossil_fatal("a timer is already running; use 'fossil timer stop' first");
  }
  if( timer_create_artifact("start", zComment, 0)==0 ){
    fossil_fatal("could not create timer artifact");
  }
  fossil_print("timer started\n");
}

/*
** "fossil timer stop ?-c COMMENT?"
**
** Honours the timer-min-seconds setting: if the in-progress segment is
** shorter than that threshold, both the open start and the would-be stop
** are discarded (the start is purged into the graveyard, no stop artifact
** is ever created).
*/
static void timer_stop_cmd(void){
  const char *zComment = find_option("comment","c",1);
  double startJd = 0.0;
  double nowJd;
  int    secs;
  int    minSecs;

  verify_all_options();
  user_select();
  if( !timer_running(&startJd) ){
    fossil_fatal("no timer is currently running; use 'fossil timer start' first");
  }
  nowJd   = db_double(0.0, "SELECT julianday('now')");
  secs    = (int)((nowJd - startJd) * 86400.0 + 0.5);
  minSecs = timer_min_seconds();
  if( minSecs>0 && secs<minSecs ){
    /* Below threshold - discard the open session entirely. */
    int startRid = db_int(0,
      "SELECT rid FROM timer "
      " WHERE stop_rid IS NULL "
      " ORDER BY start_mtime DESC, rid DESC LIMIT 1");
    if( startRid>0 ) timer_purge_one(startRid, "below timer-min-seconds");
    {
      char *zMsg = mprintf(
        "timer: segment discarded (%ds < timer-min-seconds=%ds)",
        secs, minSecs);
      timer_audit(zMsg);
      fossil_free(zMsg);
    }
    fossil_print("segment too short (%ds < %ds) - discarded\n",
                 secs, minSecs);
    return;
  }
  if( timer_create_artifact("stop", zComment, 0)==0 ){
    fossil_fatal("could not create timer artifact");
  }
  fossil_print("timer stopped (+%dh %dm)\n", secs/3600, (secs/60)%60);
}

/*
** "fossil timer status"
*/
static void timer_status_cmd(void){
  double startJd = 0.0;
  verify_all_options();
  if( timer_running(&startJd) ){
    double nowJd = db_double(0.0, "SELECT julianday('now')");
    int    secs  = (int)((nowJd - startJd) * 86400.0 + 0.5);
    char  *zWhen = db_text(0,
      "SELECT datetime(%.17g)", startJd);
    fossil_print("RUNNING since %s (+%dh %dm)\n",
                 zWhen, secs/3600, (secs/60)%60);
    fossil_free(zWhen);
  }else{
    fossil_print("idle (no timer running)\n");
  }
}

/*
** "fossil timer list"
*/
static void timer_list_cmd(void){
  verify_all_options();
  timer_aggregate(1);
}

/*
** "fossil timer add START STOP ?-c COMMENT?"
**
** Add a complete segment retroactively by creating both the start and
** stop artifacts at the supplied timestamps.  STOP must be later than
** START.  Useful for backfilling time you forgot to track live.
*/
static void timer_add_cmd(void){
  const char *zComment = find_option("comment","c",1);
  const char *zStart;
  const char *zStop;
  double sJd, eJd;

  verify_all_options();
  user_select();
  if( g.argc<5 ){
    usage("add START STOP ?-c COMMENT?");
  }
  zStart = g.argv[3];
  zStop  = g.argv[4];

  sJd = db_double(0.0, "SELECT julianday(%Q)", zStart);
  eJd = db_double(0.0, "SELECT julianday(%Q)", zStop);
  if( sJd<=0 ){
    fossil_fatal("cannot parse START timestamp: %s", zStart);
  }
  if( eJd<=0 ){
    fossil_fatal("cannot parse STOP timestamp: %s", zStop);
  }
  if( eJd<=sJd ){
    fossil_fatal("STOP (%s) must be strictly later than START (%s)",
                 zStop, zStart);
  }
  {
    int minSecs = timer_min_seconds();
    int secs = (int)((eJd - sJd) * 86400.0 + 0.5);
    if( minSecs>0 && secs<minSecs ){
      fossil_fatal("segment is %ds, below timer-min-seconds=%ds; "
                   "lower the setting or expand the range",
                   secs, minSecs);
    }
  }

  if( timer_create_artifact("start", zComment, zStart)==0 ){
    fossil_fatal("could not create start artifact");
  }
  if( timer_create_artifact("stop", zComment, zStop)==0 ){
    fossil_fatal("could not create stop artifact");
  }
  {
    int secs = (int)((eJd - sJd) * 86400.0 + 0.5);
    char *zMsg = mprintf(
      "timer: segment added retroactively: %s -> %s (+%dh %dm)%s%s",
      zStart, zStop, secs/3600, (secs/60)%60,
      zComment ? " - " : "", zComment ? zComment : "");
    timer_audit(zMsg);
    fossil_free(zMsg);
    fossil_print("segment added: %s -> %s (+%dh %dm)\n",
                 zStart, zStop, secs/3600, (secs/60)%60);
  }
}

/*
** "fossil timer rm UUID|RID ..."
**
** Remove one or more timer events by hash or rid.  Affected artifacts
** go to the graveyard and can be recovered with "fossil purge undo".
*/
static void timer_rm_cmd(void){
  int i, n=0;
  verify_all_options();
  if( g.argc<4 ){
    usage("rm UUID|RID ...");
  }
  for(i=3; i<g.argc; i++){
    int rid = timer_resolve_rid(g.argv[i]);
    char *zAct;
    char *zWhen;
    char *zComment;
    char *zMsg;
    if( rid==0 ){
      fossil_print("  skip %s - not a timer artifact\n", g.argv[i]);
      continue;
    }
    /* Capture before purge for the audit message.  The user may pass
    ** either a start RID (rid in timer.rid) or a stop RID (rid in
    ** timer.stop_rid); resolve which by joining on either column. */
    zAct = db_text(0,
      "SELECT CASE WHEN rid=%d THEN 'start' WHEN stop_rid=%d THEN 'stop' END"
      "  FROM timer WHERE rid=%d OR stop_rid=%d LIMIT 1",
      rid, rid, rid, rid);
    zWhen = db_text(0,
      "SELECT datetime(CASE WHEN rid=%d THEN start_mtime ELSE stop_mtime END)"
      "  FROM timer WHERE rid=%d OR stop_rid=%d LIMIT 1",
      rid, rid, rid);
    zComment = db_text(0,
      "SELECT comment FROM timer WHERE rid=%d OR stop_rid=%d LIMIT 1",
      rid, rid);
    if( timer_purge_one(rid, "fossil timer rm") ){
      zMsg = mprintf("timer: %s event at %s deleted%s%s",
                     zAct ? zAct : "?",
                     zWhen ? zWhen : "?",
                     (zComment && zComment[0]) ? " - " : "",
                     (zComment && zComment[0]) ? zComment : "");
      timer_audit(zMsg);
      fossil_free(zMsg);
      fossil_print("  purged rid=%d (%s)\n", rid, g.argv[i]);
      n++;
    }
    fossil_free(zAct);
    fossil_free(zWhen);
    fossil_free(zComment);
  }
  fossil_print("\n%d artifact(s) purged\n", n);
}

/*
** "fossil timer edit UUID|RID ?--time NEW_TIME? ?-c COMMENT?"
**
** Replace an existing timer event with a new one having the same action
** but a new timestamp and/or comment.  Implemented as purge-then-create
** so the audit trail is preserved (purge graveyard records the prior).
*/
static void timer_edit_cmd(void){
  const char *zNewTime = find_option("time",0,1);
  const char *zComment = find_option("comment","c",1);
  int rid;
  char *zAction = 0;

  verify_all_options();
  user_select();
  if( g.argc<4 ){
    usage("edit UUID|RID ?--time NEW_TIME? ?-c COMMENT?");
  }
  if( zNewTime==0 && zComment==0 ){
    fossil_fatal("at least one of --time or -c|--comment is required");
  }

  rid = timer_resolve_rid(g.argv[3]);
  if( rid==0 ){
    fossil_fatal("not a timer session: %s", g.argv[3]);
  }
  if( !db_exists("SELECT 1 FROM timer WHERE rid=%d", rid) ){
    fossil_fatal("rid %d is a stop artifact, not a session start; "
                 "edit by session start (the timer.rid)", rid);
  }
  (void)zAction;  /* unused under the new schema */

  /* Snapshot the existing session for fallbacks + audit message. */
  {
    char *zOldStart = db_text(0,
      "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%f',start_mtime) "
      "  FROM timer WHERE rid=%d", rid);
    char *zOldStop  = db_text(0,
      "SELECT CASE WHEN stop_mtime IS NULL THEN NULL "
      "            ELSE strftime('%%Y-%%m-%%dT%%H:%%M:%%f',stop_mtime) END "
      "  FROM timer WHERE rid=%d", rid);
    char *zOldComment = db_text(0,
      "SELECT comment FROM timer WHERE rid=%d", rid);
    const char *zNewStart   = find_option("start", 0, 1);
    const char *zNewStop    = find_option("stop",  0, 1);
    const char *zEffStart, *zEffStop, *zEffComment;

    zEffStart   = (zNewStart && zNewStart[0]) ? zNewStart : zOldStart;
    zEffStop    = (zNewStop  && zNewStop[0])  ? zNewStop  : zOldStop;
    zEffComment = (zComment  && zComment[0])  ? zComment  : zOldComment;
    /* The legacy --time flag (kept for skill compat) acts on the start. */
    if( zNewTime && zNewTime[0] ) zEffStart = zNewTime;

    if( zEffStop ){
      double sJd = db_double(0.0, "SELECT julianday(%Q)", zEffStart);
      double eJd = db_double(0.0, "SELECT julianday(%Q)", zEffStop);
      if( eJd <= sJd ){
        fossil_fatal("stop (%s) must be strictly after start (%s)",
                     zEffStop, zEffStart);
      }
    }

    if( !timer_purge_one(rid, "fossil timer edit") ){
      fossil_fatal("could not purge old session");
    }
    if( !timer_create_artifact("start", zEffComment, zEffStart) ){
      fossil_fatal("could not create replacement start artifact");
    }
    if( zEffStop ){
      if( !timer_create_artifact("stop", zEffComment, zEffStop) ){
        fossil_fatal("could not create replacement stop artifact");
      }
    }

    {
      char *zMsg = mprintf(
        "timer: session edited (start %s->%s, stop %s->%s)",
        zOldStart ? zOldStart : "?", zEffStart,
        zOldStop  ? zOldStop  : "(running)",
        zEffStop  ? zEffStop  : "(running)");
      timer_audit(zMsg);
      fossil_free(zMsg);
    }

    fossil_print("session edited: start=%s stop=%s\n",
                 zEffStart, zEffStop ? zEffStop : "(running)");

    fossil_free(zOldStart);
    fossil_free(zOldStop);
    fossil_free(zOldComment);
  }
}

/*
** "fossil timer total"
*/
static void timer_total_cmd(void){
  verify_all_options();
  timer_aggregate(0);
}

/*
** WEBPAGE: timer
**
** Dashboard for the time tracker.  GET renders current state plus a
** segment list; POST with action=start or action=stop creates the
** corresponding timer artifact and redirects back here.
*/
void timer_page(void){
  const char *zAction;
  double startJd = 0.0;
  int running;
  Stmt q;
  double totalSecs = 0.0;
  int    haveOpen  = 0;
  int    pairs     = 0;

  login_check_credentials();
  if( !g.perm.RdWiki ){
    login_needed(g.anon.RdWiki);
    return;
  }

  /* POST handler: dispatch on action= */
  zAction = P("action");
  if( zAction && cgi_csrf_safe(2) ){
    if( !g.perm.WrWiki ){
      login_needed(g.anon.WrWiki);
      return;
    }
    user_select();
    if( fossil_strcmp(zAction,"start")==0
     || fossil_strcmp(zAction,"stop")==0 ){
      int isStart = fossil_strcmp(zAction,"start")==0;
      double startJd = 0.0;
      int alreadyRunning = timer_running(&startJd);
      if( isStart && !alreadyRunning ){
        const char *zComment = P("comment");
        timer_create_artifact("start", zComment, 0);
      }else if( !isStart && alreadyRunning ){
        double nowJd = db_double(0.0, "SELECT julianday('now')");
        int secs = (int)((nowJd - startJd) * 86400.0 + 0.5);
        int minSecs = timer_min_seconds();
        if( minSecs>0 && secs<minSecs ){
          int startRid = db_int(0,
            "SELECT rid FROM timer "
            " WHERE stop_rid IS NULL "
            " ORDER BY start_mtime DESC, rid DESC LIMIT 1");
          if( startRid>0 ) timer_purge_one(startRid, "below timer-min-seconds");
          {
            char *zMsg = mprintf(
              "timer: segment discarded (%ds < timer-min-seconds=%ds)",
              secs, minSecs);
            timer_audit(zMsg);
            fossil_free(zMsg);
          }
        }else{
          const char *zComment = P("comment");
          timer_create_artifact("stop", zComment, 0);
        }
      }
    }else if( fossil_strcmp(zAction,"delete")==0 ){
      const char *zRid = P("rid");
      int rid = zRid ? timer_resolve_rid(zRid) : 0;
      if( rid>0 ){
        /* timer_purge_one() auto-cascades when given a session start
        ** (its stop_rid goes too).  Capture the human-readable info
        ** for the audit message before purge wipes the rows. */
        char *zWhen = db_text(0,
          "SELECT datetime(start_mtime) FROM timer WHERE rid=%d", rid);
        timer_purge_one(rid, "/timer delete");
        {
          char *zMsg = mprintf(
            "timer: session starting %s deleted",
            zWhen ? zWhen : "?");
          timer_audit(zMsg);
          fossil_free(zMsg);
        }
        fossil_free(zWhen);
      }
    }else if( fossil_strcmp(zAction,"add")==0 ){
      const char *zStart   = P("start");
      const char *zStop    = P("stop");
      const char *zComment = P("comment");
      if( zStart && zStop && zStart[0] && zStop[0] ){
        double sJd = db_double(0.0, "SELECT julianday(%Q)", zStart);
        double eJd = db_double(0.0, "SELECT julianday(%Q)", zStop);
        int minSecs = timer_min_seconds();
        int secs = (int)((eJd - sJd) * 86400.0 + 0.5);
        if( sJd>0 && eJd>sJd && (minSecs<=0 || secs>=minSecs) ){
          timer_create_artifact("start", zComment, zStart);
          timer_create_artifact("stop",  zComment, zStop);
          {
            char *zMsg = mprintf(
              "timer: segment added retroactively: %s -> %s (+%dh %dm)%s%s",
              zStart, zStop, secs/3600, (secs/60)%60,
              (zComment && zComment[0]) ? " - " : "",
              (zComment && zComment[0]) ? zComment : "");
            timer_audit(zMsg);
            fossil_free(zMsg);
          }
        }
      }
    }
    cgi_redirectf("%R/timer");
    return;
  }

  running = timer_running(&startJd);

  style_set_current_feature("timer");
  style_header("Timer");

  /* Status block + live counter */
  @ <div id="timer-status" class="timerStatus"
  if( running ){
    @  data-running="1" data-start-jd="%.17g(startJd)">
  }else{
    @ >
  }
  if( running ){
    char *zWhen = db_text(0, "SELECT datetime(%.17g)", startJd);
    @ <h2 style="color:#080">RUNNING since %h(zWhen)</h2>
    @ <p>Elapsed:
    @   <span id="timer-elapsed" style="font-family:monospace; font-size:1.4em">
    @     calculating...
    @   </span>
    @ </p>
    fossil_free(zWhen);
  }else{
    @ <h2 style="color:#a00">idle</h2>
  }
  @ </div>

  /* Buttons */
  @ <form action="%R/timer" method="POST">
  login_insert_csrf_secret();
  @   <input type="text" name="comment" placeholder="optional comment"
  @          style="width:60%%; margin:5px 0">
  @   <br>
  if( running ){
    @   <button type="submit" name="action" value="stop"
    @           style="background:#fdd; padding:8px 16px; font-size:1.1em">
    @     STOP timer
    @   </button>
  }else{
    @   <button type="submit" name="action" value="start"
    @           style="background:#dfd; padding:8px 16px; font-size:1.1em">
    @     START timer
    @   </button>
  }
  @ </form>

  /* Sessions list - one row per start↔stop pair (or per running start). */
  @ <h3>Sessions</h3>
  @ <table border="1" cellpadding="4" cellspacing="0">
  @ <tr><th>Start</th><th>Stop</th><th>Duration</th><th>Comment</th><th>Actions</th></tr>
  {
    double nowJd = db_double(0.0, "SELECT julianday('now')");
    db_prepare(&q,
      "SELECT rid, start_mtime, stop_rid, stop_mtime, comment, "
      "       datetime(start_mtime), "
      "       CASE WHEN stop_mtime IS NULL THEN '' "
      "            ELSE datetime(stop_mtime) END "
      "  FROM timer "
      " ORDER BY start_mtime ASC, rid ASC");
    while( db_step(&q)==SQLITE_ROW ){
      int rid           = db_column_int(&q, 0);
      double sJd        = db_column_double(&q, 1);
      double eJd        = db_column_double(&q, 3);
      const char *zCmt  = db_column_text(&q, 4);
      const char *zWhen = db_column_text(&q, 5);
      const char *zStop = db_column_text(&q, 6);
      int running       = (db_column_type(&q, 2)==SQLITE_NULL);
      double secs       = running ? (nowJd - sJd) * 86400.0
                                  : (eJd - sJd) * 86400.0;
      int mn            = (int)(secs/60.0 + 0.5);
      const char *zBg   = running ? "#dfd" : "#dfe";
      totalSecs += secs;
      if( running ){
        haveOpen = 1;
      }else{
        pairs++;
      }
      @ <tr style="background:%h(zBg)">
      @   <td>%h(zWhen)</td>
      @   <td>%h(running ? "(running)" : zStop)</td>
      @   <td>%dh(mn/60) %dm(mn%60)</td>
      @   <td>%h(zCmt?zCmt:"")</td>
      @   <td>
      @     <a href="%R/timeredit?rid=%d(rid)">edit</a>
      @     |
      @     <form action="%R/timer" method="POST" style="display:inline">
      login_insert_csrf_secret();
      @       <input type="hidden" name="rid" value="%d(rid)">
      @       <button type="submit" name="action" value="delete"
      @               onclick="return confirm('Delete this session?')"
      @               style="background:none;border:none;color:#c00;cursor:pointer;text-decoration:underline">
      @         delete
      @       </button>
      @     </form>
      @   </td>
      @ </tr>
    }
    db_finalize(&q);
  }
  @ </table>

  /* Add segment form */
  @ <h3>Add a segment retroactively</h3>
  @ <form action="%R/timer" method="POST">
  login_insert_csrf_secret();
  @   <input type="hidden" name="action" value="add">
  @   <label>Start: <input type="datetime-local" name="start" required></label>
  @   <label>Stop:  <input type="datetime-local" name="stop"  required></label>
  @   <label>Comment: <input type="text" name="comment" style="width:30%%"></label>
  @   <button type="submit">Add</button>
  @ </form>

  {
    int totalMin = (int)(totalSecs/60.0);
    @ <h3>Total: %dh(totalMin/60) %dm(totalMin%60)
    if( haveOpen ){
      @  (incl. running)
    }
    @ </h3>
    @ <p>Closed segments: %d(pairs)
    if( haveOpen ){
      @  (+1 running)
    }
    @ </p>
  }

  /* Live counter JS - reads data-start-jd attribute and updates per second.
  ** All raw '%' characters are doubled so cgi_printf passes them through
  ** unchanged.  The nonce= attribute is required by fossil's
  ** Content-Security-Policy or the browser will refuse to execute. */
  @ <script nonce="%h(style_nonce())">
  @ (function(){
  @   var s = document.getElementById('timer-status');
  @   if( !s || s.getAttribute('data-running') !== '1' ) return;
  @   var startJd = parseFloat(s.getAttribute('data-start-jd'));
  @   var el = document.getElementById('timer-elapsed');
  @   /* JD epoch -> ms epoch:  (jd - 2440587.5) * 86400000 */
  @   var startMs = (startJd - 2440587.5) * 86400000;
  @   function tick(){
  @     var d = (Date.now() - startMs) / 1000;
  @     var h = Math.floor(d/3600);
  @     var m = Math.floor((d%%3600)/60);
  @     var s2 = Math.floor(d%%60);
  @     el.textContent = h + 'h ' +
  @       (m<10?'0':'') + m + 'm ' +
  @       (s2<10?'0':'') + s2 + 's';
  @   }
  @   tick();
  @   setInterval(tick, 1000);
  @ })();
  @ </script>

  style_finish_page();
}

/*
** WEBPAGE: timeredit
**
** Edit a single timer session.  GET shows a form pre-filled with the
** current start/stop/comment; POST applies the change by purging the
** existing session and creating fresh start/stop artifacts at the
** new times.  The old artifacts go to the graveyard for audit.
*/
void timeredit_page(void){
  const char *zRid = P("rid");
  int rid;
  char *zCurStart = 0;
  char *zCurStop = 0;
  char *zCurComment = 0;
  int wasRunning = 0;

  login_check_credentials();
  if( !g.perm.WrWiki ){
    login_needed(g.anon.WrWiki);
    return;
  }

  if( zRid==0 ){ cgi_redirectf("%R/timer"); return; }
  rid = timer_resolve_rid(zRid);
  if( rid==0 || !db_exists("SELECT 1 FROM timer WHERE rid=%d", rid) ){
    style_set_current_feature("timer");
    style_header("Timer session not found");
    @ <p>No such timer session: %h(zRid)</p>
    @ <p><a href="%R/timer">back to timer</a></p>
    style_finish_page();
    return;
  }

  /* POST: apply edit by purge-and-recreate */
  if( P("submit") && cgi_csrf_safe(2) ){
    const char *zNewStart   = P("startTime");
    const char *zNewStop    = P("stopTime");
    const char *zNewComment = P("comment");
    char *zOldStart, *zOldStop, *zOldComment;
    char *zEffStart, *zEffStop, *zEffComment;
    user_select();

    zOldStart = db_text(0,
      "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%f',start_mtime) "
      "  FROM timer WHERE rid=%d", rid);
    zOldStop  = db_text(0,
      "SELECT CASE WHEN stop_mtime IS NULL THEN NULL "
      "            ELSE strftime('%%Y-%%m-%%dT%%H:%%M:%%f',stop_mtime) END "
      "  FROM timer WHERE rid=%d", rid);
    zOldComment = db_text(0,
      "SELECT comment FROM timer WHERE rid=%d", rid);

    zEffStart   = (zNewStart && zNewStart[0]) ? mprintf("%s", zNewStart)
                                              : mprintf("%s", zOldStart);
    zEffStop    = (zNewStop && zNewStop[0])   ? mprintf("%s", zNewStop)
                                              : (zOldStop ? mprintf("%s", zOldStop) : 0);
    zEffComment = (zNewComment && zNewComment[0])
                                              ? mprintf("%s", zNewComment)
                                              : (zOldComment ? mprintf("%s", zOldComment) : mprintf(""));

    if( timer_purge_one(rid, "/timeredit") ){
      timer_create_artifact("start", zEffComment, zEffStart);
      if( zEffStop ){
        timer_create_artifact("stop", zEffComment, zEffStop);
      }
      {
        char *zMsg = mprintf(
          "timer: session edited via UI (start %s->%s, stop %s->%s)",
          zOldStart ? zOldStart : "?", zEffStart,
          zOldStop  ? zOldStop  : "(running)",
          zEffStop  ? zEffStop  : "(running)");
        timer_audit(zMsg);
        fossil_free(zMsg);
      }
    }

    fossil_free(zOldStart);
    fossil_free(zOldStop);
    fossil_free(zOldComment);
    fossil_free(zEffStart);
    fossil_free(zEffStop);
    fossil_free(zEffComment);
    cgi_redirectf("%R/timer");
    return;
  }

  /* GET: render form */
  zCurStart = db_text(0,
    "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%S',start_mtime) "
    "  FROM timer WHERE rid=%d", rid);
  zCurStop  = db_text(0,
    "SELECT CASE WHEN stop_mtime IS NULL THEN '' "
    "            ELSE strftime('%%Y-%%m-%%dT%%H:%%M:%%S',stop_mtime) END "
    "  FROM timer WHERE rid=%d", rid);
  zCurComment = db_text(0,
    "SELECT comment FROM timer WHERE rid=%d", rid);
  wasRunning = (zCurStop==0 || zCurStop[0]==0);

  style_set_current_feature("timer");
  style_header("Edit timer session");

  @ <h2>Edit timer session</h2>
  @ <form action="%R/timeredit?rid=%d(rid)" method="POST">
  login_insert_csrf_secret();
  @   <p>Start:
  @     <input type="datetime-local" name="startTime"
  @            value="%h(zCurStart?zCurStart:"")" required></p>
  @   <p>Stop:
  @     <input type="datetime-local" name="stopTime"
  @            value="%h(zCurStop?zCurStop:"")">
  @     %s(wasRunning ? "(leave empty to keep the timer running)" : "(clear to convert to running)")
  @   </p>
  @   <p>Comment:
  @     <input type="text" name="comment"
  @            value="%h(zCurComment?zCurComment:"")"
  @            style="width:60%%"></p>
  @   <button type="submit" name="submit" value="apply">Apply</button>
  @   <a href="%R/timer">cancel</a>
  @ </form>

  fossil_free(zCurStart);
  fossil_free(zCurStop);
  fossil_free(zCurComment);
  style_finish_page();
}

/*
** COMMAND: timer
**
** Usage: %fossil timer SUBCOMMAND ?ARGS?
**
** Track time spent on whatever this repository represents.  Each timer
** event is recorded as a first-class artifact (CFTYPE_TIMER) and also
** appears on the timeline as type='m'.
**
** Subcommands:
**
**    start  ?-c|--comment TEXT?
**       Begin a new time-tracking segment.
**
**    stop   ?-c|--comment TEXT?
**       End the most recent open segment.
**
**    status
**       Print whether a timer is currently running, plus how long
**       it has been running.
**
**    list
**       Print every closed segment with its individual duration,
**       followed by the cumulative total.  Marks any open segment
**       as RUNNING.
**
**    total
**       Print just the cumulative time.
**
**    add START STOP ?-c|--comment TEXT?
**       Backfill a complete segment by creating both the start and
**       stop artifacts at the given timestamps.  STOP must be after
**       START.  Useful for entering forgotten work retroactively.
**
**    rm UUID|RID ...
**       Move one or more timer events into the graveyard.  Recoverable
**       via "fossil purge undo".
**
**    edit UUID|RID ?--time NEW_TIME? ?-c|--comment TEXT?
**       Replace an existing event with a new one carrying the same
**       action but updated timestamp and/or comment.  Implemented as
**       purge-then-recreate so the prior version stays in the graveyard.
*/
void timer_cmd(void){
  const char *zSub;
  int n;

  db_find_and_open_repository(0, 0);
  if( g.argc<3 ) usage("SUBCOMMAND ?ARGS?");

  zSub = g.argv[2];
  n = (int)strlen(zSub);
  if( n<2 ){
    fossil_fatal("ambiguous timer subcommand: %s", zSub);
  }

  if( strncmp(zSub,"start",n)==0 ){
    timer_start_cmd();
  }else if( strncmp(zSub,"stop",n)==0 ){
    timer_stop_cmd();
  }else if( strncmp(zSub,"status",n)==0 ){
    timer_status_cmd();
  }else if( strncmp(zSub,"list",n)==0 ){
    timer_list_cmd();
  }else if( strncmp(zSub,"total",n)==0 ){
    timer_total_cmd();
  }else if( strncmp(zSub,"add",n)==0 ){
    timer_add_cmd();
  }else if( strncmp(zSub,"rm",n)==0 ){
    timer_rm_cmd();
  }else if( strncmp(zSub,"edit",n)==0 ){
    timer_edit_cmd();
  }else{
    fossil_fatal("unknown timer subcommand: %s\n"
                 "available: start, stop, status, list, total, "
                 "add, rm, edit", zSub);
  }
}
