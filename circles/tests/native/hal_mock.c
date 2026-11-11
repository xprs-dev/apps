#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* hal_db_* is sdk/hal_db_zx.c: zx, the host's own database engine,
 * linked by run.sh with ZXDB_ROOT=/tmp/ct. */

/* ---- capture of hal_msg_send ---- */
#define MAXCAP 8000
static char* g_cap[MAXCAP]; static int g_capn=0;
void cap_clear(void){ for(int i=0;i<g_capn;i++) free(g_cap[i]); g_capn=0; }
int  cap_contains(const char* s){ for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) return 1; return 0; }
const char* cap_find(const char* s){ for(int i=0;i<g_capn;i++) if(strstr(g_cap[i],s)) return g_cap[i]; return 0; }

void hal_msg_send(const char* json, uint32_t len){
  if(g_capn<MAXCAP){ char* c=(char*)malloc(len+1); memcpy(c,json,len); c[len]=0; g_cap[g_capn++]=c; }
}

/* ---- injectable inbox for module_handle_event ---- */
static char g_inbox[16384]; static int g_inbox_set=0;
void inbox_set(const char* s){ strncpy(g_inbox,s,sizeof(g_inbox)-1); g_inbox[sizeof(g_inbox)-1]=0; g_inbox_set=1; }
uint32_t hal_msg_available(void){ return g_inbox_set? (uint32_t)strlen(g_inbox):0; }
uint32_t hal_msg_recv(char* buf, uint32_t cap){ if(!g_inbox_set) return 0; uint32_t n=strlen(g_inbox); if(n>cap)n=cap; memcpy(buf,g_inbox,n); g_inbox_set=0; return n; }

/* ---- misc HAL ---- */
void hal_log(int32_t lvl,const char* m,uint32_t n){ (void)lvl;(void)m;(void)n; }
/* No lang/ in the harness: every wtr() falls back to its English literal. */
uint32_t hal_i18n_get(const char* k,uint32_t kl,char* o,uint32_t cap){ (void)k;(void)kl;(void)o;(void)cap; return 0; }
int32_t hal_event_subscribe(const char* t,uint32_t n){ (void)t;(void)n; return 0; }
uint64_t hal_time_ms(void){ return 0; }
uint64_t hal_time_epoch(void){ return 1700000000ULL; }
uint32_t hal_identity_pubkey(char* b,uint32_t cap){ const char* s="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(b,s,n); return n; }
uint32_t hal_identity_sign(const char* m,uint32_t ml,char* o,uint32_t cap){ (void)m;(void)ml; const char* s="SIGSIGSIGSIGSIGSIG"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_verify(const char* p,uint32_t pl,const char* m,uint32_t ml,const char* s,uint32_t sl){ (void)p;(void)pl;(void)m;(void)ml;(void)s;(void)sl; return 1; }
uint32_t hal_npub(const char* p,uint32_t pl,char* o,uint32_t cap){ (void)p;(void)pl; const char* s="npub1mockmock"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_encrypt(const char* p,uint32_t pl,const char* m,uint32_t ml,char* o,uint32_t cap){ (void)p;(void)pl; uint32_t n=ml<cap?ml:cap; memcpy(o,m,n); return n; }
uint32_t hal_decrypt(const char* p,uint32_t pl,const char* b,uint32_t bl,char* o,uint32_t cap){ (void)p;(void)pl; uint32_t n=bl<cap?bl:cap; memcpy(o,b,n); return n; }
uint32_t hal_crypto_keygen(char* o,uint32_t cap){ const char* s="{\"priv\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"pub\":\"2222222222222222222222222222222222222222222222222222222222222222\"}"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_crypto_sign(const char* p,uint32_t pl,const char* m,uint32_t ml,char* o,uint32_t cap){ (void)p;(void)pl;(void)m;(void)ml; const char* s="deadbeef"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
int32_t hal_crypto_verify(const char* p,uint32_t pl,const char* s,uint32_t sl,const char* m,uint32_t ml){ (void)p;(void)pl;(void)s;(void)sl;(void)m;(void)ml; return 1; }
static unsigned char g_rndctr=1;
uint32_t hal_crypto_random(char* o,uint32_t n){ for(uint32_t i=0;i<n;i++) o[i]=(char)(g_rndctr+i); g_rndctr+=7; return n; }
uint32_t hal_crypto_aes_encrypt(const char* k,uint32_t kl,const char* in,uint32_t il,char* o,uint32_t cap){ (void)k;(void)kl; if(16+il>cap) return 0; for(int i=0;i<16;i++)o[i]=0; memcpy(o+16,in,il); return 16+il; }
uint32_t hal_crypto_aes_decrypt(const char* k,uint32_t kl,const char* in,uint32_t il,char* o,uint32_t cap){ (void)k;(void)kl; if(il<=16) return 0; uint32_t n=il-16; if(n>cap)n=cap; memcpy(o,in+16,n); return n; }
uint32_t hal_rns_identity(char* o,uint32_t cap){ const char* s="rnsid"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
int32_t  hal_rns_broadcast(const char* p,uint32_t l){ (void)p;(void)l; return 1; }
int32_t  hal_rns_send_to(const char* d,uint32_t dl,const char* p,uint32_t pl){ (void)d;(void)dl;(void)p;(void)pl; return 1; }
int32_t  hal_rns_pull(const char* d,uint32_t dl){ (void)d;(void)dl; return 1; }
uint32_t hal_rns_delivery_dest(char* o,uint32_t cap){ const char* s="delivdest"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
uint32_t hal_rns_prop_dest(char* o,uint32_t cap){ const char* s="propdest"; uint32_t n=strlen(s); if(n>cap)n=cap; memcpy(o,s,n); return n; }
int32_t  hal_rns_rv_announce(const char* s,uint32_t sl,const char* a,uint32_t al){ (void)s;(void)sl;(void)a;(void)al; return 1; }
uint32_t hal_rns_rv_resolve(const char* s,uint32_t sl,char* o,uint32_t cap){ (void)s;(void)sl;(void)o;(void)cap; return 0; }
int32_t  hal_rns_rv_send(const char* s,uint32_t sl,const char* p,uint32_t pl){ (void)s;(void)sl;(void)p;(void)pl; return 1; }
uint32_t hal_rns_available(void){ return 0; }
uint32_t hal_rns_recv(char* o,uint32_t cap){ (void)o;(void)cap; return 0; }
int32_t  hal_contacts_query(const char* q,uint32_t ql,char* o,uint32_t cap){ (void)q;(void)ql; const char* s="[]"; uint32_t n=strlen(s); if(n>cap)return -2; memcpy(o,s,n); return n; }

