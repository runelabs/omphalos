#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <omphalos/dns.h>
#include <omphalos/util.h>
#include <asm/byteorder.h>
#include <omphalos/resolv.h>
#include <omphalos/inotify.h>
#include <omphalos/netaddrs.h>
#include <omphalos/omphalos.h>

typedef struct resolvq {
	struct l3host *l3;
	struct l2host *l2;
	struct interface *i;
	struct resolvq *next;
} resolvq;

typedef struct resolver {
	struct in_addr ina;
	struct resolver *next;
} resolver;

static resolver *resolvers;
static char *resolvconf_fn;
static pthread_mutex_t resolver_lock = PTHREAD_MUTEX_INITIALIZER;

// Resolv queue is global to all interfaces, since there's no required mapping
// between routes to resolvers and interfaces.
static resolvq *rqueue;
static pthread_mutex_t rqueue_lock = PTHREAD_MUTEX_INITIALIZER;

static void
free_resolvers(resolver **r){
	resolver *tmp;

	while( (tmp = *r) ){
		*r = tmp->next;
		free(tmp);
	}
}

static inline resolvq *
create_resolvq(struct interface *i,struct l2host *l2,struct l3host *l3){
	resolvq *r;

	if( (r = malloc(sizeof(*r))) ){
		r->l3 = l3;
		r->l2 = l2;
		r->i = i;
		r->next = NULL;
		pthread_mutex_lock(&rqueue_lock);
		r->next = rqueue;
		rqueue = r;
		pthread_mutex_unlock(&rqueue_lock);
	}
	return r;
}

int queue_for_naming(struct interface *i,struct l2host *l2,struct l3host *l3){
	return create_resolvq(i,l2,l3) ? 0 : -1;
}

int offer_resolution(const omphalos_iface *octx,int fam,const void *addr,
				const char *name,namelevel nlevel){
	resolvq *r,**p;

	for(p = &rqueue ; (r = *p) ; p = &r->next){
		if(l3addr_eq_p(r->l3,fam,addr)){
			name_l3host_absolute(octx,r->i,r->l2,r->l3,name,nlevel);
			if(nlevel >= NAMING_LEVEL_REVDNS){
				*p = r->next;
				free(r);
			}
			break;
		}
	}
	return 0;
}

static resolver *
create_resolver(const void *addr,size_t len){
	resolver *r;

	assert(len <= sizeof(r->ina));
	if( (r = malloc(sizeof(*r))) ){
		memcpy(&r->ina,addr,len);
		r->next = NULL;
	}
	return r;
}

static void
parse_resolv_conf(const omphalos_iface *octx){
	resolver *revs = NULL;
	char *line;
	FILE *fp;
	char *b;
	int l;

	if((fp = fopen(resolvconf_fn,"r")) == NULL){
		octx->diagnostic("Couldn't open %s",resolvconf_fn);
		return;
	}
	b = NULL;
	l = 0;
	errno = 0;
	while( (line = fgetl(&b,&l,fp)) ){
		struct in_addr ina;
		resolver *r;
		char *nl;

#define NSTOKEN "nameserver"
		while(isspace(*line)){
			++line;
		}
		if(*line == '#' || !*line){
			continue;
		}
		if(strncmp(line,NSTOKEN,__builtin_strlen(NSTOKEN))){
			continue;
		}
		line += __builtin_strlen(NSTOKEN);
		if(!isspace(*line)){
			continue;
		}
		do{
			++line;
		}while(isspace(*line));
		nl = strchr(line,'\n');
		*nl = '\0';
		if(inet_pton(AF_INET,line,&ina) != 1){
			continue;
		}
		if((r = create_resolver(&ina,sizeof(ina))) == NULL){
			break; // FIXME
		}
		r->next = revs;
		revs = r;
		// FIXME
		//
#undef NSTOKEN
	}
	free(b);
	fclose(fp);
	if(errno){
		free_resolvers(&revs);
	}else{
		resolver *r;

		pthread_mutex_lock(&resolver_lock);
		r = resolvers;
		resolvers = revs;
		pthread_mutex_unlock(&resolver_lock);
		free_resolvers(&r);
		octx->diagnostic("Reloaded resolvers from %s",resolvconf_fn);
	}
}

int init_naming(const omphalos_iface *octx,const char *resolvconf){
	if((resolvconf_fn = strdup(resolvconf)) == NULL){
		goto err;
	}
	if(watch_file(octx,resolvconf,parse_resolv_conf)){
		goto err;
	}
	return 0;

err:
	free(resolvconf_fn);
	resolvconf_fn = NULL;
	return -1;
}

int cleanup_naming(const omphalos_iface *octx){
	resolvq *r;
	int er;

	er = 0;
	while( (r = rqueue) ){
		rqueue = r->next;
		free(r);
		++er;
	}
	octx->diagnostic("%d outstanding resolutions",er);
	if( (er = pthread_mutex_destroy(&rqueue_lock)) ){
		octx->diagnostic("Error destroying resolvq lock (%s)",strerror(er));
	}
	if( (er = pthread_mutex_destroy(&resolver_lock)) ){
		octx->diagnostic("Error destroying resolver lock (%s)",strerror(er));
	}
	free_resolvers(&resolvers);
	free(resolvconf_fn);
	resolvconf_fn = NULL;
	return er;
}
