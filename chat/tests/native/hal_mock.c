#define _GNU_SOURCE
/*
 * A native HAL for the chat wapp's tests: real sqlite3 behind hal_sqlite_*,
 * an in-memory KV, a capture of everything hal_msg_send says, and an
 * injectable event queue for module_handle_event. Modelled on
 * circles/tests/native/hal_mock.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* ---- minimal sqlite3 decls (no dev header needed) ---- */
typedef struct sqlite3 sqlite3; typedef struct sqlite3_stmt sqlite3_stmt;
extern int sqlite3_open(const char*, sqlite3**);
extern int sqlite3_close(sqlite3*);
extern int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
extern int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*));
extern int sqlite3_bind_int64(sqlite3_stmt*, int, long long);
extern int sqlite3_bind_null(sqlite3_stmt*, int);
extern int sqlite3_step(sqlite3_stmt*);
extern int sqlite3_finalize(sqlite3_stmt*);
extern int sqlite3_column_count(sqlite3_stmt*);
extern const char* sqlite3_column_name(sqlite3_stmt*, int);
extern int sqlite3_column_type(sqlite3_stmt*, int);
extern const unsigned char* sqlite3_column_text(sqlite3_stmt*, int);
extern long long sqlite3_column_int64(sqlite3_stmt*, int);
extern const char* sqlite3_errmsg(sqlite3*);
#define SROW 100
#define SDONE 101
#define SOK 0
#define S_INT 1
#define S_NULL 5
#define S_TRANSIENT ((void(*)(void*))-1)

const char *g_root = "/tmp/chat_native_test";

/* ---- multi-node simulator hooks (NULL = default single-node behaviour, so
 *      test_room.c is untouched; sim.c installs these to network N nodes) ---- */
const char* (*g_hk_identity)(void) = 0;
int32_t (*g_hk_send)(const char*,uint32_t) = 0;
int32_t (*g_hk_message)(const char*,uint32_t,const char*,uint32_t,uint32_t,char*,uint32_t) = 0;
int32_t (*g_hk_broadcast)(const char*,uint32_t,const char*,uint32_t,const char*,uint32_t,char*,uint32_t) = 0;
int32_t (*g_hk_read)(const char*,uint32_t) = 0;
int32_t (*g_hk_groups)(char*,uint32_t) = 0;
int32_t (*g_hk_roster)(const char*,uint32_t,char*,uint32_t) = 0;
int32_t (*g_hk_unlock)(const char*,uint32_t,const char*,uint32_t) = 0;
int32_t (*g_hk_redact)(const char*,uint32_t,const char*,uint32_t,const char*,uint32_t) = 0;
int32_t (*g_hk_folder)(const char*,uint32_t,const char*,uint32_t,char*,uint32_t) = 0;
int32_t (*g_hk_picture)(const char*,uint32_t,const char*,uint32_t) = 0;
int32_t (*g_hk_invite)(const char*,uint32_t,const char*,uint32_t) = 0;
int32_t (*g_hk_accept)(const char*,uint32_t) = 0;
void mock_set_root(const char* r){ static char b[256]; snprintf(b,sizeof(b),"%s",r); g_root=b; }

/* ---- capture of hal_msg_send ---- */
#define MAXCAP 8000
static char* g_cap[MAXCAP]; static int g_capn=0;
void cap_clear(void){ for(int i=0;i<g_capn;i++) free(g_cap[i]); g_capn=0; }
int  cap_count(const char* s){ int n=0; for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) n++; return n; }
int  cap_contains(const char* s){ return cap_count(s)>0; }
const char* cap_find(const char* s){ for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) return g_cap[i]; return 0; }
const char* cap_nth(int i){ return i<g_capn? g_cap[i]:0; }
int  cap_n(void){ return g_capn; }
void hal_msg_send(const char* json, uint32_t len){
  if(g_capn<MAXCAP){ char* c=(char*)malloc(len+1); memcpy(c,json,len); c[len]=0; g_cap[g_capn++]=c; }
}

/* ---- injectable host inbox ---- */
static char g_inbox[16384]; static int g_inbox_set=0;
void inbox_set(const char* s){ strncpy(g_inbox,s,sizeof(g_inbox)-1); g_inbox[sizeof(g_inbox)-1]=0; g_inbox_set=1; }
uint32_t hal_msg_available(void){ return g_inbox_set? (uint32_t)strlen(g_inbox):0; }
uint32_t hal_msg_recv(char* buf, uint32_t cap){ if(!g_inbox_set) return 0; uint32_t n=strlen(g_inbox); if(n>cap)n=cap; memcpy(buf,g_inbox,n); g_inbox_set=0; return n; }

/* ---- injectable core event queue ---- */
#define EVQ 64
static char* g_ev_topic[EVQ]; static char* g_ev_row[EVQ]; static int g_ev_r=0, g_ev_w=0;
void event_push(const char* topic, const char* row){
  if(g_ev_w-g_ev_r>=EVQ) return;
  g_ev_topic[g_ev_w%EVQ]=strdup(topic); g_ev_row[g_ev_w%EVQ]=strdup(row); g_ev_w++;
}
/* No lang/ files in the harness: every wtr() falls back to its English literal,
 * which is also what the tests compare against. */
uint32_t hal_i18n_get(const char* k,uint32_t kl,char* o,uint32_t oc){ (void)k;(void)kl;(void)o;(void)oc; return 0; }
int32_t hal_event_subscribe(const char* t,uint32_t l){ (void)t;(void)l; return 0; }
int32_t hal_event_unsubscribe(const char* t,uint32_t l){ (void)t;(void)l; return 0; }
int32_t hal_event_publish(const char* t,uint32_t tl,const char* p,uint32_t pl){ (void)t;(void)tl;(void)p;(void)pl; return 0; }
uint32_t hal_event_available(void){ return (uint32_t)(g_ev_w-g_ev_r); }
uint32_t hal_event_recv(char* tb,uint32_t tc,char* rb,uint32_t rc){
  if(g_ev_w==g_ev_r) return 0;
  int i=g_ev_r%EVQ; g_ev_r++;
  snprintf(tb,tc,"%s",g_ev_topic[i]); uint32_t n=strlen(g_ev_row[i]); if(n>rc) n=rc; memcpy(rb,g_ev_row[i],n);
  free(g_ev_topic[i]); free(g_ev_row[i]); return n;
}

/* ---- log capture ---- */
static char* g_log[2000]; static int g_logn=0;
void hal_log(int32_t lvl,const char* m,uint32_t n){ (void)lvl; if(g_logn<2000){ char* c=malloc(n+1); memcpy(c,m,n); c[n]=0; g_log[g_logn++]=c; } }
int log_count(const char* s){ int k=0; for(int i=0;i<g_logn;i++) if(strstr(g_log[i],s)) k++; return k; }
void log_clear(void){ for(int i=0;i<g_logn;i++) free(g_log[i]); g_logn=0; }

/* ---- time / identity ---- */
static uint64_t g_now=1700000000ULL;
void mock_set_time(uint64_t t){ g_now=t; }
uint64_t hal_time_ms(void){ return g_now*1000; }
uint64_t hal_time_epoch(void){ return g_now; }
int32_t hal_time_utc_offset(void){ return 0; }
uint32_t hal_identity(char* b,uint32_t cap){ const char* s= g_hk_identity? g_hk_identity() : "X1TEST"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(b,s,n); return n; }

/* ---- kv (in memory) ---- */
#define KVMAX 256
static char* g_kv_k[KVMAX]; static char* g_kv_v[KVMAX]; static uint32_t g_kv_l[KVMAX]; static int g_kv_ns_of[KVMAX]; static int g_kvn=0;
static int g_kv_ns=0;   /* the active node's KV namespace; sim.c sets it on load */
void mock_kv_ns(int n){ g_kv_ns=n; }
static int kv_find(const char* k,uint32_t kl){ for(int i=0;i<g_kvn;i++) if(g_kv_ns_of[i]==g_kv_ns&&strlen(g_kv_k[i])==kl&&!memcmp(g_kv_k[i],k,kl)) return i; return -1; }
void mock_kv_set(const char* k,const char* v){ int i=kv_find(k,strlen(k)); if(i<0){ i=g_kvn++; g_kv_k[i]=strdup(k); g_kv_v[i]=0; g_kv_ns_of[i]=g_kv_ns; } free(g_kv_v[i]); g_kv_v[i]=strdup(v); g_kv_l[i]=strlen(v); }
int mock_kv_exists(const char* k){ return kv_find(k,strlen(k))>=0; }
uint32_t hal_kv_get(const char* k,uint32_t kl,char* o,uint32_t cap){ int i=kv_find(k,kl); if(i<0) return 0; uint32_t n=g_kv_l[i]; if(n>cap)n=cap; memcpy(o,g_kv_v[i],n); return n; }
int32_t hal_kv_set(const char* k,uint32_t kl,const char* v,uint32_t vl){ char kk[64]; if(kl>=sizeof(kk)) return -1; memcpy(kk,k,kl); kk[kl]=0; int i=kv_find(kk,kl); if(i<0){ i=g_kvn++; g_kv_k[i]=strdup(kk); g_kv_v[i]=0; g_kv_ns_of[i]=g_kv_ns; } free(g_kv_v[i]); g_kv_v[i]=malloc(vl+1); memcpy(g_kv_v[i],v,vl); g_kv_v[i][vl]=0; g_kv_l[i]=vl; return 0; }
int32_t hal_kv_delete(const char* k,uint32_t kl){ int i=kv_find(k,kl); if(i<0) return -1; free(g_kv_k[i]); free(g_kv_v[i]); for(int j=i;j<g_kvn-1;j++){ g_kv_k[j]=g_kv_k[j+1]; g_kv_v[j]=g_kv_v[j+1]; g_kv_l[j]=g_kv_l[j+1]; g_kv_ns_of[j]=g_kv_ns_of[j+1]; } g_kvn--; return 0; }

/* ---- xprs doors ---- */
static int g_bcast_n=0; static char g_last_wire[1024]; static char g_history[65536]="[]";
const char* mock_last_wire(void){ return g_last_wire; }
/* Statuses (XPRS.md 27): the core names the post and the test reads the text. */
static int g_ui_attached=1; static char g_last_status[1024]; static int g_status_n=0;
void mock_ui_attached(int on){ g_ui_attached=on; }
int32_t hal_ui_attached(void){ return g_ui_attached; }
const char* mock_last_status(void){ return g_last_status; }
int32_t hal_xprs_status(const char* t,uint32_t tl,const char* mo,uint32_t ml,const char* r,uint32_t rl,char* id,uint32_t cap){
  (void)mo;(void)ml;(void)r;(void)rl; if(!tl) return -1;
  snprintf(g_last_status,sizeof(g_last_status),"%.*s",(int)tl,t);
  snprintf(id,cap,"st%04d",++g_status_n); return 0; }
void mock_clear_wire(void){ g_last_wire[0]=0; }
void mock_set_history(const char* json){ snprintf(g_history,sizeof(g_history),"%s",json); }
/* The core's answer to a send: 0 aired, -2 "not a member of that group". */
static int32_t g_send_rc = 0;
void mock_set_send_rc(int32_t rc){ g_send_rc = rc; }
int32_t hal_xprs_send(const char* w,uint32_t l){ if(g_hk_send) return g_hk_send(w,l); if(g_send_rc) return g_send_rc; if(l>=sizeof(g_last_wire)) return -1; memcpy(g_last_wire,w,l); g_last_wire[l]=0; return 0; }
int32_t hal_xprs_unlock(const char* id,uint32_t il,const char* pass,uint32_t pl){ if(g_hk_unlock) return g_hk_unlock(id,il,pass,pl); return 0; }
int32_t hal_xprs_redact(const char* c,uint32_t cl,const char* t,uint32_t tl,const char* p,uint32_t pl){ if(g_hk_redact) return g_hk_redact(c,cl,t,tl,p,pl); return 0; }
/* hal_xprs_kind: the core's xprsAddressKind, mirrored (a stand-in core,
 * docs/architecture.md 6). Only the station/open distinction and the
 * foreign form matter to the wapp. */
static int mock_is_foreign(const char* to,uint32_t tl);
int32_t hal_xprs_kind(const char* a,uint32_t l,char* o,uint32_t cap){
  if(!l) return 0;
  const char* k;
  int dash=0, digit=0; for(uint32_t i=0;i<l;i++) if(a[i]=='-') dash=1;
  for(uint32_t i=1;i<l && i<3;i++) if(a[i]>='0'&&a[i]<='9') digit=1;
  if(mock_is_foreign(a,l)) k = a[1]=='T' ? "foreign:meshtastic" : "foreign:meshcore";
  else if((l>=6 && a[0]=='X' && a[1]>='1' && a[1]<='5') || dash || digit)
    k = (a[0]=='X'&&a[1]=='5') ? "closed" : (a[0]=='X'&&a[1]=='1') ? "user" : (a[0]=='X'&&a[1]=='4') ? "device" : "station";
  else k="open";
  uint32_t n=strlen(k); if(n>cap) return -(int32_t)n; memcpy(o,k,n); return (int32_t)n;
}
/* The core's verdict for a node of another network (XPRS.md 3.2, MT/MC and
 * eight hex): never sealed (-3), plain through a gateway (3). */
static int mock_is_foreign(const char* to,uint32_t tl){ if(tl!=10||to[0]!='M'||(to[1]!='T'&&to[1]!='C')) return 0; for(uint32_t i=2;i<tl;i++){ char c=to[i]; if(!((c>='0'&&c<='9')||(c>='A'&&c<='F'))) return 0; } return 1; }
int32_t hal_xprs_message(const char* to,uint32_t tl,const char* t,uint32_t l,uint32_t priv,char* id,uint32_t cap){ if(g_hk_message) return g_hk_message(to,tl,t,l,priv,id,cap); (void)t;(void)l; int fx=mock_is_foreign(to,tl); if(fx&&priv) return -3; snprintf(id,cap,"m%05d",++g_bcast_n); return fx?3:(priv?1:2); }
static char g_bcast_scope[16];
/* The scope the wapp asked for on the last broadcast. 13.11 makes this the
 * only difference between the two broadcast rooms, so a test has to see it. */
const char* mock_bcast_scope(void){ return g_bcast_scope; }
void mock_bcast_scope_clear(void){ g_bcast_scope[0]=0; }
int32_t hal_xprs_broadcast(const char* t,uint32_t l,const char* s,uint32_t sl,const char* r,uint32_t rl,char* id,uint32_t cap){ if(g_hk_broadcast) return g_hk_broadcast(t,l,s,sl,r,rl,id,cap); (void)t;(void)l;(void)r;(void)rl; { uint32_t n=sl<sizeof(g_bcast_scope)-1?sl:sizeof(g_bcast_scope)-1; memcpy(g_bcast_scope,s,n); g_bcast_scope[n]=0; } snprintf(id,cap,"b%05d",++g_bcast_n); return 2; }
static char g_reads[2048]; static int g_reads_n=0;
const char* mock_reads(void){ return g_reads; }
int mock_reads_count(void){ return g_reads_n; }
void mock_reads_clear(void){ g_reads[0]=0; g_reads_n=0; }
int32_t hal_xprs_read(const char* id,uint32_t l){ if(g_hk_read) return g_hk_read(id,l); (void)l; size_t u=strlen(g_reads); snprintf(g_reads+u,sizeof(g_reads)-u,"[%s]",id); g_reads_n++; return 0; }
/* The room a row is in, as the core files it: the scope word before m:. */
static const char* mock_row_scope(const char* row, size_t len){
  const char* w=memmem(row,len,"\"wire\":\"",8); if(!w) return "global";
  const char* e=memmem(w,len-(size_t)(w-row)," m:",3); size_t hl=e?(size_t)(e-w):len-(size_t)(w-row);
  if(memmem(w,hl," scope:local",12)) return "local";
  if(memmem(w,hl," scope:regional",15)) return "regional";
  return "global";
}
/* Answers what the core's hal_xprs_history answers for `scope`: only the rows
 * of that room. Without a scope, everything, as the old core did. */
int32_t hal_xprs_history(const char* q,uint32_t ql,char* o,uint32_t cap){
  char scope[16]=""; const char* sq=q?memmem(q,ql,"\"scope\":\"",9):0;
  if(sq){ sq+=9; size_t i=0; while(i<sizeof(scope)-1&&sq[i]&&sq[i]!='"'&&sq<q+ql){ scope[i]=sq[i]; i++; } scope[i]=0; }
  /* And the window: the core answers the newest `limit` rows and no more,
   * which is what makes a room that out-talks another able to hide it. */
  int limit=200; const char* lq=q?memmem(q,ql,"\"limit\":",8):0; if(lq) limit=atoi(lq+8);
  static char out[sizeof(g_history)]; size_t n=0; out[n++]='['; int taken=0;
  const char* p=g_history; int depth=0, instr=0; const char* start=0; int first=1;
  for(; *p; p++){
    if(instr){ if(*p=='\\'&&p[1]) p++; else if(*p=='"') instr=0; continue; }
    if(*p=='"'){ instr=1; continue; }
    if(*p=='{'){ if(depth++==0) start=p; continue; }
    if(*p=='}'&&--depth==0&&start){
      size_t len=(size_t)(p-start)+1;
      if(taken<limit&&(!scope[0]||strcmp(mock_row_scope(start,len),scope)==0)){
        taken++;
        if(!first) out[n++]=','; first=0;
        if(n+len>=sizeof(out)-2) break;
        memcpy(out+n,start,len); n+=len;
      }
      start=0;
    }
  }
  out[n++]=']'; out[n]=0;
  if(n>cap) return -(int32_t)n; memcpy(o,out,n); return (int32_t)n;
}
static char g_groups[2048] = "[]";
void mock_set_groups(const char* json){ snprintf(g_groups,sizeof(g_groups),"%s",json&&json[0]?json:"[]"); }
int32_t hal_xprs_groups(char* o,uint32_t cap){ if(g_hk_groups) return g_hk_groups(o,cap); uint32_t n=strlen(g_groups); if(n>cap) return -(int32_t)n; memcpy(o,g_groups,n); return n; }
static char g_roster[40000] = "[]";
void mock_set_roster(const char* json){ snprintf(g_roster,sizeof(g_roster),"%s",json&&json[0]?json:"[]"); }
int32_t hal_xprs_group_roster(const char* g,uint32_t gl,char* o,uint32_t cap){ if(g_hk_roster) return g_hk_roster(g,gl,o,cap); (void)g;(void)gl; uint32_t n=strlen(g_roster); if(n>cap) return -(int32_t)n; memcpy(o,g_roster,n); return n; }
/* Minting is the core's; the mock hands back a fixed callsign, adds the group
 * to what hal_xprs_groups reports with us as admin (as the core does), and
 * remembers the nick it was asked for. -1 when a test says no profile is open. */
static char g_created_nick[64]; static int g_create_fail=0;
const char* mock_created_nick(void){ return g_created_nick; }
void mock_set_create_fail(int f){ g_create_fail=f; }
int32_t hal_xprs_group_create(const char* n,uint32_t nl,char* o,uint32_t cap){ if(g_create_fail) return -1; uint32_t k=nl<sizeof(g_created_nick)-1?nl:sizeof(g_created_nick)-1; memcpy(g_created_nick,n,k); g_created_nick[k]=0; snprintf(g_groups,sizeof(g_groups),"[{\"call\":\"X5NEWG\",\"nick\":\"%s\",\"role\":\"admin\",\"admin\":true}]",g_created_nick); const char* c="X5NEWG"; uint32_t m=strlen(c); if(m>cap) return -1; memcpy(o,c,m); if(m<cap) o[m]=0; return m; }
/* The core signs the offer; the mock records "<group>:<call>" and answers
 * what the test set (-1 = not the admin). */
static char g_invited[64]; static int32_t g_invite_rc=0;
const char* mock_invited(void){ return g_invited; }
void mock_set_invite_rc(int32_t rc){ g_invite_rc=rc; g_invited[0]=0; }
int32_t hal_xprs_group_invite(const char* g,uint32_t gl,const char* c,uint32_t cl){ if(g_hk_invite) return g_hk_invite(g,gl,c,cl); snprintf(g_invited,sizeof(g_invited),"%.*s:%.*s",(int)gl,g,(int)cl,c); return g_invite_rc; }
/* The member's own half: the mock records the group and, on success, turns
 * our role in what hal_xprs_groups reports from invited into member. */
static char g_accepted[16]; static int32_t g_accept_rc=0;
const char* mock_accepted(void){ return g_accepted; }
void mock_set_accept_rc(int32_t rc){ g_accept_rc=rc; g_accepted[0]=0; }
int32_t hal_xprs_group_accept(const char* g,uint32_t gl){ if(g_hk_accept) return g_hk_accept(g,gl); snprintf(g_accepted,sizeof(g_accepted),"%.*s",(int)gl,g); if(g_accept_rc==0){ char* r=strstr(g_groups,"\"invited\""); if(r){ memmove(r+8,r+9,strlen(r+9)+1); memcpy(r,"\"member\"",8); } } return g_accept_rc; }
/* A folder (XPRS.md 10.2) and a group's picture (6.3.2) are the core's to
 * mint and sign; single-node tests read back what was asked. */
static char g_folder_ask[96]; static char g_picture_ask[160];
const char* mock_folder_ask(void){ return g_folder_ask; }
const char* mock_picture_ask(void){ return g_picture_ask; }
int32_t hal_xprs_group_folder(const char* p,uint32_t pl,const char* n,uint32_t nl,char* o,uint32_t cap){ if(g_hk_folder) return g_hk_folder(p,pl,n,nl,o,cap); snprintf(g_folder_ask,sizeof(g_folder_ask),"%.*s:%.*s",(int)pl,p,(int)nl,n); const char* c="X5FOLD"; uint32_t m=strlen(c); if(m>cap) return -1; memcpy(o,c,m); if(m<cap) o[m]=0; return m; }
int32_t hal_xprs_group_picture(const char* g,uint32_t gl,const char* t,uint32_t tl){ if(g_hk_picture) return g_hk_picture(g,gl,t,tl); snprintf(g_picture_ask,sizeof(g_picture_ask),"%.*s:%.*s",(int)gl,g,(int)tl,t); return 0; }
/* XPRS.md 7.8: correcting and withdrawing, and a moderator's acts. Single-node
 * tests read back what was asked; sim.c installs the hooks to network them. */
int32_t (*g_hk_edit)(const char*,uint32_t,const char*,uint32_t,const char*,uint32_t,uint32_t,char*,uint32_t) = 0;
int32_t (*g_hk_withdraw)(const char*,uint32_t,const char*,uint32_t) = 0;
int32_t (*g_hk_moderation)(const char*,uint32_t,char*,uint32_t) = 0;
int32_t (*g_hk_role_at)(const char*,uint32_t,const char*,uint32_t,uint32_t) = 0;
int32_t (*g_hk_moderate)(const char*,uint32_t,const char*,uint32_t,const char*,uint32_t,uint32_t) = 0;
static char g_edit_ask[1100]; static char g_withdraw_ask[96]; static char g_moderate_ask[1100];
static char g_moderation[4096] = "[]"; static char g_roles[512];
const char* mock_edit_ask(void){ return g_edit_ask; }
const char* mock_withdraw_ask(void){ return g_withdraw_ask; }
const char* mock_moderate_ask(void){ return g_moderate_ask; }
void mock_edit_clear(void){ g_edit_ask[0]=0; g_withdraw_ask[0]=0; g_moderate_ask[0]=0; }
/* "X1PZ4Q=2,X32DVA=3": what each callsign is at any moment. */
void mock_set_roles(const char* spec){ snprintf(g_roles,sizeof(g_roles),"%s",spec?spec:""); }
void mock_set_moderation(const char* json){ snprintf(g_moderation,sizeof(g_moderation),"%s",json&&json[0]?json:"[]"); }
int32_t hal_xprs_edit(const char* to,uint32_t tl,const char* orig,uint32_t ol,const char* t,uint32_t l,uint32_t priv,char* id,uint32_t cap){
  if(g_hk_edit) return g_hk_edit(to,tl,orig,ol,t,l,priv,id,cap);
  snprintf(g_edit_ask,sizeof(g_edit_ask),"%.*s|%.*s|%.*s|%u",(int)tl,to,(int)ol,orig,(int)l,t,priv);
  int fx=mock_is_foreign(to,tl);
  if(fx&&priv) return -3;
  snprintf(id,cap,"m%05d",++g_bcast_n);
  return fx?3:(priv?1:2);
}
int32_t hal_xprs_withdraw(const char* to,uint32_t tl,const char* orig,uint32_t ol){
  if(g_hk_withdraw) return g_hk_withdraw(to,tl,orig,ol);
  snprintf(g_withdraw_ask,sizeof(g_withdraw_ask),"%.*s|%.*s",(int)tl,to,(int)ol,orig);
  return 0;
}
int32_t hal_xprs_group_moderation(const char* g,uint32_t gl,char* o,uint32_t cap){
  if(g_hk_moderation) return g_hk_moderation(g,gl,o,cap);
  uint32_t n=strlen(g_moderation);
  if(n>cap) return -(int32_t)n;
  memcpy(o,g_moderation,n);
  return (int32_t)n;
}
int32_t hal_xprs_group_role_at(const char* g,uint32_t gl,const char* c,uint32_t cl,uint32_t ts){
  if(g_hk_role_at) return g_hk_role_at(g,gl,c,cl,ts);
  (void)g;(void)gl;(void)ts;
  const char* p=g_roles;
  while(*p){
    const char* e=strchr(p,'=');
    if(!e) break;
    if((uint32_t)(e-p)==cl && !strncmp(p,c,cl)) return atoi(e+1);
    p=strchr(e,',');
    if(!p) break;
    p++;
  }
  return 0;
}
int32_t hal_xprs_group_moderate(const char* g,uint32_t gl,const char* tg,uint32_t tgl,const char* t,uint32_t l,uint32_t kind){
  if(g_hk_moderate) return g_hk_moderate(g,gl,tg,tgl,t,l,kind);
  snprintf(g_moderate_ask,sizeof(g_moderate_ask),"%.*s|%.*s|%.*s|%u",(int)gl,g,(int)tgl,tg,(int)l,t,kind);
  return 0;
}
int32_t hal_xprs_stations(char* o,uint32_t cap){ const char* s="[{\"title\":\"Heard over the air (2)\",\"items\":[{\"id\":\"X1NEAR\",\"title\":\"X1NEAR\",\"subtitle\":\"BLE - -40 dBm - 3 packets\",\"tags\":[\"seen 20s ago\",\"BLE\",\"peers 2\"]},{\"id\":\"X1TEST\",\"title\":\"X1TEST\",\"subtitle\":\"BLE\",\"tags\":[\"seen 5s ago\",\"BLE\"]}]},{\"title\":\"Heard this hour (1)\",\"items\":[{\"id\":\"X1EARLIER\",\"title\":\"X1EARLIER\",\"subtitle\":\"LAN\",\"tags\":[\"seen 40m ago\",\"LAN\"]}]},{\"title\":\"On Reticulum (1)\",\"items\":[{\"id\":\"X1FAR\",\"title\":\"X1FAR\",\"subtitle\":\"RNS\",\"tags\":[\"seen 3m ago\",\"RNS\"]}]}]"; uint32_t n=strlen(s); if(n>cap) return -(int32_t)n; memcpy(o,s,n); return n; }
int32_t hal_mesh_devices(char* o,uint32_t cap){ const char* s="[{\"title\":\"Nearby\",\"items\":[{\"id\":\"X1NEAR\",\"title\":\"X1NEAR\",\"subtitle\":\"phone\"}]},{\"title\":\"Multi-hop\",\"items\":[{\"id\":\"X1FAR\",\"title\":\"X1FAR\"}]}]"; uint32_t n=strlen(s); if(n>cap) return -(int32_t)n; memcpy(o,s,n); return n; }
int32_t hal_people_directory(const char* q,uint32_t ql,char* o,uint32_t cap){ (void)q;(void)ql; const char* s="[{\"kind\":\"xprs\",\"callsign\":\"X1PEER\",\"nick\":\"Peer\",\"live\":true}]"; uint32_t n=strlen(s); if(n>cap) return -2; memcpy(o,s,n); return n; }

/* ---- sqlite-backed hal_sqlite_* ---- */
#define MAXH 128
static sqlite3* g_db[MAXH]; static int g_dbn=1; static char g_err[256];
static uint32_t g_query_cap_limit=0;   /* tests can force -2 */
void mock_query_cap(uint32_t c){ g_query_cap_limit=c; }
static void mkparents(const char* full){ char tmp[700]; strncpy(tmp,full,sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0; for(char* p=tmp+1;*p;p++){ if(*p=='/'){ *p=0; mkdir(tmp,0777); *p='/'; } } }
int32_t hal_sqlite_open(const char* path,uint32_t len){
  char rel[512]; if(len>=sizeof(rel))return -1; memcpy(rel,path,len); rel[len]=0;
  char full[700]; snprintf(full,sizeof(full),"%s/%s",g_root,rel); mkparents(full);
  sqlite3* db; if(sqlite3_open(full,&db)!=SOK) return -1;
  /* Reuse a closed slot. hal_sqlite_close() frees g_db[h] but g_dbn only ever
   * went up, so a run was capped at MAXH opens TOTAL rather than at MAXH open
   * at once: adding one more always-present room made the later tests get -1
   * from here and fail with no captures and no explanation. */
  for(int i=1;i<MAXH;i++) if(!g_db[i]){ g_db[i]=db; if(i>=g_dbn) g_dbn=i+1; return i; }
  sqlite3_close(db); return -1;
}
static void bind_params(sqlite3_stmt* st,const char* params,uint32_t plen){
  if(!params||plen==0) return; const char* p=params; while(*p&&*p!='[')p++; if(*p)p++; int idx=1;
  while(*p){ while(*p==' '||*p==',')p++; if(*p==']'||!*p) break;
    if(*p=='"'){ p++; static char val[8192]; int v=0; while(*p&&*p!='"'){ if(*p=='\\'&&p[1]){ p++; if(*p=='n')val[v++]='\n'; else if(*p=='t')val[v++]='\t'; else if(*p=='u'){ val[v++]=(char)strtol((char[]){p[3],p[4],0},0,16); p+=4; } else val[v++]=*p; } else val[v++]=*p; p++; } val[v]=0; if(*p=='"')p++; sqlite3_bind_text(st,idx++,val,v,S_TRANSIENT); }
    else if(*p=='n'){ while(*p&&*p!=','&&*p!=']')p++; sqlite3_bind_null(st,idx++); }
    else { char num[64]; int n=0; while(*p&&(*p=='-'||(*p>='0'&&*p<='9'))){ if(n<63)num[n++]=*p; p++; } num[n]=0; sqlite3_bind_int64(st,idx++,atoll(num)); }
  }
}
int32_t hal_sqlite_exec(int32_t h,const char* sql,uint32_t sl,const char* params,uint32_t pl){
  if(h<=0||h>=MAXH||!g_db[h]) return -1; static char z[16384]; if(sl>=sizeof(z))return -1; memcpy(z,sql,sl); z[sl]=0;
  sqlite3_stmt* st; if(sqlite3_prepare_v2(g_db[h],z,-1,&st,0)!=SOK){ snprintf(g_err,sizeof(g_err),"%s",sqlite3_errmsg(g_db[h])); return -1; }
  bind_params(st,params,pl); int rc; while((rc=sqlite3_step(st))==SROW){} sqlite3_finalize(st);
  if(rc!=SDONE){ snprintf(g_err,sizeof(g_err),"%s",sqlite3_errmsg(g_db[h])); return -1; } return 0;
}
int32_t hal_sqlite_query(int32_t h,const char* sql,uint32_t sl,const char* params,uint32_t pl,char* out,uint32_t cap){
  if(h<=0||h>=MAXH||!g_db[h]) return -1; static char z[16384]; if(sl>=sizeof(z))return -1; memcpy(z,sql,sl); z[sl]=0;
  sqlite3_stmt* st; if(sqlite3_prepare_v2(g_db[h],z,-1,&st,0)!=SOK){ snprintf(g_err,sizeof(g_err),"%s",sqlite3_errmsg(g_db[h])); return -1; }
  bind_params(st,params,pl);
  static char buf[1<<20]; int o=0; buf[o++]='['; int first=1; int rc;
  while((rc=sqlite3_step(st))==SROW){ if(!first)buf[o++]=','; first=0; buf[o++]='{'; int nc=sqlite3_column_count(st);
    for(int c=0;c<nc;c++){ if(c)buf[o++]=','; o+=sprintf(buf+o,"\"%s\":",sqlite3_column_name(st,c)); int ty=sqlite3_column_type(st,c);
      if(ty==S_NULL){ o+=sprintf(buf+o,"null"); }
      else if(ty==S_INT){ o+=sprintf(buf+o,"%lld",sqlite3_column_int64(st,c)); }
      else { const unsigned char* t=sqlite3_column_text(st,c); buf[o++]='"'; for(const unsigned char* q=t;q&&*q;q++){ if(*q=='"'||*q=='\\'){buf[o++]='\\';buf[o++]=*q;} else if(*q=='\n'){buf[o++]='\\';buf[o++]='n';} else buf[o++]=*q; } buf[o++]='"'; }
    } buf[o++]='}';
  }
  sqlite3_finalize(st); buf[o++]=']'; buf[o]=0;
  if(g_query_cap_limit && (uint32_t)o>g_query_cap_limit) return -2;
  if((uint32_t)o>cap) return -2; memcpy(out,buf,o); return o;
}
uint32_t hal_sqlite_error(int32_t h,char* o,uint32_t cap){ (void)h; uint32_t n=strlen(g_err); if(n>cap)n=cap; memcpy(o,g_err,n); return n; }
void hal_sqlite_close(int32_t h){ if(h>0&&h<MAXH&&g_db[h]){ sqlite3_close(g_db[h]); g_db[h]=0; } }
