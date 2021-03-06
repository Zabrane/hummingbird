/*
 * hstress - HTTP load generator with periodic output.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <event.h>
#include <evhttp.h>

#include "u.h"

#define NBUFFER 10
#define MAX_BUCKETS 100

#define OUTFILE_BUFFER_SIZE 4096
#define DRAIN_BUFFER_SIZE 4096

// cheap hack to get closer to our Hz target
#define USEC_FUDGE -300

#define debug(s) ;
//#define debug(s) fprintf(stderr, "run %d -> ", run->id); fprintf(stderr, s);

char *http_hostname;
uint16_t http_port;
char http_hosthdr[2048];

struct{
    int count;
    int concurrency;
    int buckets[MAX_BUCKETS];
    int nbuckets;
    int rpc;
    int qps;

    // for logging output time
    char *tsvout;
    FILE *tsvoutfile;

    // request path
    char *path;
    char *host_hdr;
}params;

struct{
    int conns;
    int conn_successes;
    int counters[MAX_BUCKETS + 1];
    int conn_errors;
    int conn_timeouts;
    int conn_closes;
    int http_successes;
    int http_errors;
}counts;

int num_cols = 6;

struct request{
    struct timeval           starttv;
    struct event             timeoutev;
    struct event             dispatchev;
    int                      sock;
    struct evhttp_connection *evcon;
    struct evhttp_request    *evreq;
    int                      evcon_reqno;
};

struct runner{
    struct timeval            tv;
    struct event              ev;
    struct evhttp_connection *evcon;
    struct request            *req;
    int                       reqno;
    int                       id;
};
typedef struct runner runner;

int runid = 0;

enum{
    Success,
    Closed,
    Error,
    Timeout
};

struct event    reportev;
struct timeval  reporttv ={ 1, 0 };
struct timeval  timeouttv ={ 1, 0 };
struct timeval  lastreporttv;
int             request_timeout;
struct timeval  ratetv;
int             ratecount = 0;
int             nreport = 0;
int             nreportbuf[NBUFFER];
int             *reportbuf[NBUFFER];

void mkhttp(runner *run);

void recvcb(struct evhttp_request *req, void *arg);
void timeoutcb(int fd, short what, void *arg);
void closecb(struct evhttp_connection *evcon, void *arg);

void report();
void sigint(int which);

unsigned char
qps_enabled()
{
    return params.qps > 0;
}

unsigned char
rpc_enabled()
{
    return params.rpc > 0;
}

unsigned char
tsv_enabled()
{
    return params.tsvout != nil;
}

/*
    Reporting.
*/

long
milliseconds_since_start(struct timeval *tv)
{
    long milliseconds;
    struct timeval now, diff;

    gettimeofday(&now, nil);
    timersub(&now, tv, &diff);
    milliseconds = diff.tv_sec * 1000L + diff.tv_usec / 1000L;

    return milliseconds;
}

long
mkrate(struct timeval *tv, int count)
{
    long milliseconds;
    milliseconds = milliseconds_since_start(tv);
    return(1000L * count / milliseconds);
}

void
reset_time(struct timeval *tv)
{
    gettimeofday(tv, nil);
}

void
reportcb(int fd, short what, void *arg)
{
    int i;

    printf("%d\t", nreport++);
    printf("%d\t", counts.conn_successes);
    printf("%d\t", counts.conn_errors);
    printf("%d\t", counts.conn_timeouts);
    printf("%d\t", counts.conn_closes);
    printf("%d\t", counts.http_successes);
    printf("%d\t", counts.http_errors);

    counts.conn_successes = counts.conn_errors = counts.conn_timeouts = counts.conn_closes = counts.http_successes = counts.http_errors = 0;

    for(i=0; params.buckets[i]!=0; i++)
        printf("%d\t", counts.counters[i]);

    printf("%d\n", counts.counters[i]);
    fflush(stdout);

    memset(counts.counters, 0, sizeof(counts.counters));

    if(params.count<0 || counts.conns<params.count){
        evtimer_add(&reportev, &reporttv);
    }
}

/*
    HTTP, via libevent's HTTP support.
*/

void
mkhttp(runner *run)
{
    struct evhttp_connection *evcon;

    evcon = evhttp_connection_new(http_hostname, http_port);
    if(evcon == nil)
        panic("evhttp_connection_new");

    evhttp_connection_set_closecb(evcon, &closecb, run);
    /*
        note: we manage our own per-request timeouts, since the underlying
        library does not give us enough error reporting fidelity
    */
    run->evcon = evcon;
}

void
dispatch(runner *run, int reqno)
{
    struct evhttp_connection *evcon = run->evcon;
    struct evhttp_request *evreq;
    struct request *req;

    if((req = calloc(1, sizeof(*req))) == nil)
        panic("calloc");

    run->req = req;
    req->evcon = evcon;
    req->evcon_reqno = reqno;

    evreq = evhttp_request_new(&recvcb, run);
    if(evreq == nil)
        panic("evhttp_request_new");

    req->evreq = evreq;

    evreq->response_code = -1;
    evhttp_add_header(evreq->output_headers, "Host", http_hosthdr);

    gettimeofday(&req->starttv, nil);
    evtimer_set(&req->timeoutev, timeoutcb, run);
    evtimer_add(&req->timeoutev, &timeouttv);
    debug("dispatch(): evtimer_add(&req->timeoutev, &timeouttv);\n");

    counts.conns++;
    evhttp_make_request(evcon, evreq, EVHTTP_REQ_GET, params.path);
}

void
save_request(int how, runner *run)
{
    struct request *req = run->req;
    int i;
    long start_microseconds, now_microseconds, milliseconds;
    struct timeval now, diff;

    gettimeofday(&now, nil);

    start_microseconds = req->starttv.tv_sec * 1000000 + req->starttv.tv_usec;
    now_microseconds   = now.tv_sec * 1000000 + now.tv_usec;

    timersub(&now, &req->starttv, &diff);
    milliseconds = (now_microseconds - start_microseconds)/1000;

    if(tsv_enabled()) {
        fprintf(params.tsvoutfile, "%ld\t%ld\t%d\n", start_microseconds, now_microseconds, how);
    }

    switch(how){
    case Success:
        counts.conn_successes++;
    switch(req->evreq->response_code){
        case 200:
            for(i=0; params.buckets[i]<milliseconds && params.buckets[i]!=0; i++);
            counts.counters[i]++;
            counts.http_successes++;
            break;
        default:
            counts.http_errors++;
            break;
        }
        break;
    case Error:
        counts.conn_errors++;
        break;
    case Timeout:
        counts.conn_timeouts++;
        break;
    }
}

void
complete(int how, runner *run)
{
    struct request *req = run->req;
    save_request(how, run);

    evtimer_del(&req->timeoutev);
    debug("complete(): evtimer_del(&req->timeoutev);\n");

    /* enqueue the next one */
    if(params.count<0 || counts.conns<params.count){
        // re-scheduling is handled by the callback
        if(!qps_enabled()){
            if(!rpc_enabled() || req->evcon_reqno<params.rpc){
                dispatch(run, req->evcon_reqno + 1);
            }else{
                // re-establish the connection
                evhttp_connection_free(run->evcon);
                mkhttp(run);
                dispatch(run, 1);
            }
        }
    }else{
        /* We'll count this as a close. I guess that's ok. */
        evhttp_connection_free(req->evcon);
        if(--params.concurrency == 0){
            evtimer_del(&reportev);
            debug("last call to reportcb\n");
            reportcb(0, 0, nil);  /* issue a last report */
        }
    }

    if(!qps_enabled())
      // FIXME possible memory leak
      free(req);
}

void
runnercb(int fd, short what, void *arg)
{
    runner *run = (runner *)arg;
    debug("runnercb()\n");
    if(qps_enabled()) {
        event_add(&run->ev, &run->tv);
    }

    dispatch(run, run->reqno + 1);
}

/**
 start a new, potentially, rate-limited run
 */
void
mkrunner()
{
    runner *run = calloc(1, sizeof(runner));
    run->id = runid++;

    if(run == nil)
        panic("calloc");

    mkhttp(run);

    if(qps_enabled()) {
        run->tv.tv_sec = 0;
        run->tv.tv_usec = 1000000/params.qps + USEC_FUDGE;

        if(run->tv.tv_usec < 1)
            run->tv.tv_usec = 1;

        evtimer_set(&run->ev, runnercb, run);
        evtimer_add(&run->ev, &run->tv);
        debug("mkrunner(): evtimer_add(&run->ev, &run->tv);\n");
    } else {
        // skip the timers and just loop as fast as possible
        runnercb(0, 0, run);
    }
}

void
recvcb(struct evhttp_request *evreq, void *arg)
{

    int status = Success;

    /*
        It seems that, under certain circumstances,
        evreq may be null on failure.

        we'll count it as an error.
    */

    if(evreq == nil || evreq->response_code < 0)
        status = Error;

    complete(status, (runner *)arg);
}

void
timeoutcb(int fd, short what, void *arg)
{
    runner *run = (runner *)arg;
    debug("timeoutcb()\n");

    /* re-establish the connection */
    evhttp_connection_free(run->evcon);
    mkhttp(run);

    complete(Timeout, run);
}

void
closecb(struct evhttp_connection *evcon, void *arg)
{
    runner *run = (runner *)arg;
    debug("closecb()\n");
    counts.conn_closes++;
}


/*
    Aggregation.
*/

void
chldreadcb(struct bufferevent *b, void *arg)
{
    char *line, *sp, *ap;
    int n, i, nprocs = *(int *)arg;

    if((line=evbuffer_readline(b->input)) != nil){
        sp = line;

        if((ap = strsep(&sp, "\t")) == nil)
            panic("report error\n");
        n = atoi(ap);
        if(n - nreport > NBUFFER)
            panic("a process fell too far behind\n");

        n %= NBUFFER;

        for(i=0; i<params.nbuckets + num_cols && (ap=strsep(&sp, "\t")) != nil; i++)
            reportbuf[n][i] += atoi(ap);

        if(++nreportbuf[n] >= nprocs){
            /* Timestamp it.  */
            printf("%d\t",(int)time(nil));
            for(i = 0; i < params.nbuckets + num_cols; i++)
                printf("%d\t", reportbuf[n][i]);
            printf("%ld\n", mkrate(&lastreporttv, reportbuf[n][0]));
            reset_time(&lastreporttv);

            /* Aggregate. */
            counts.conn_successes += reportbuf[n][0];
            counts.conn_errors += reportbuf[n][1];
            counts.conn_timeouts += reportbuf[n][2];
            counts.conn_closes += reportbuf[n][3];
            counts.http_successes += reportbuf[n][4];
            counts.http_errors += reportbuf[n][5];

            for(i=0; i<params.nbuckets; i++)
                counts.counters[i] += reportbuf[n][i + num_cols];

            /* Clear it. Advance nreport. */
            memset(reportbuf[n], 0,(params.nbuckets + num_cols) * sizeof(int));
            nreportbuf[n] = 0;
            nreport++;
        }

        free(line);
    }

    bufferevent_enable(b, EV_READ);
}

void
chlderrcb(struct bufferevent *b, short what, void *arg)
{
    bufferevent_setcb(b, nil, nil, nil, nil);
    bufferevent_disable(b, EV_READ | EV_WRITE);
    bufferevent_free(b);
}

void
parentd(int nprocs, int *sockets)
{
    int *fdp, i, status;
    struct bufferevent *b;

    signal(SIGINT, sigint);

    gettimeofday(&ratetv, nil);
    gettimeofday(&lastreporttv, nil);
    memset(nreportbuf, 0, sizeof(nreportbuf));
    for(i=0; i<NBUFFER; i++){
        if((reportbuf[i] = calloc(params.nbuckets + num_cols, sizeof(int))) == nil)
            panic("calloc");
    }

    event_init();

    for(fdp=sockets; *fdp!=-1; fdp++){
        b = bufferevent_new(
            *fdp, chldreadcb, nil,
            chlderrcb,(void *)&nprocs);
        bufferevent_enable(b, EV_READ);
    }

    event_dispatch();

    for(i=0; i<nprocs; i++)
        waitpid(0, &status, 0);

    report();
}

void
sigint(int which)
{
    report();
    exit(0);
}

void
printcount(const char *name, int total, int count)
{
    fprintf(stderr, "# %s", name);
    if(total > 0)
        fprintf(stderr, "\t%d\t%.05f", count,(1.0f*count) /(1.0f*total));

    fprintf(stderr, "\n");
}

void
report()
{
    char buf[128];
    int i, total = counts.conn_successes + counts.conn_errors + counts.conn_timeouts;

    fprintf(stderr, "# hz\t\t\t%ld\n", mkrate(&ratetv, total));
    fprintf(stderr, "# time\t\t\t%.3f\n", milliseconds_since_start(&ratetv)/1000.0);

    printcount("conn_total    ", total, total);
    printcount("conn_successes", total, counts.conn_successes);
    printcount("conn_errors   ", total, counts.conn_errors);
    printcount("conn_timeouts ", total, counts.conn_timeouts);
    printcount("conn_closes   ", total, counts.conn_closes);
    printcount("http_successes", total, counts.http_successes);
    printcount("http_errors   ", total, counts.http_errors);
    for(i=0; params.buckets[i]!=0; i++){
        snprintf(buf, sizeof(buf), "<%d\t\t", params.buckets[i]);
        printcount(buf, total, counts.counters[i]);
    }

    snprintf(buf, sizeof(buf), ">=%d\t\t", params.buckets[i - 1]);
    printcount(buf, total, counts.counters[i]);
}

/*
    Main, dispatch.
*/

void
usage(char *cmd)
{
    fprintf(
        stderr,
        "%s: [-c CONCURRENCY] [-b BUCKETS] [-n COUNT] [-p NUMPROCS]\n"
        "[-r RPC] [-i INTERVAL] [-o TSV RECORD] [-l MAX_QPS]\n"
        "[-u PATH] [-H HOST_HDR] [HOST] [PORT]\n",
        cmd);

    exit(0);
}

int
main(int argc, char **argv)
{
    int ch, i, nprocs = 1, is_parent = 1, port, *sockets, fds[2];
    pid_t pid;
    char *sp, *ap, *host, *cmd = argv[0];

    /* Defaults */
    params.count = -1;
    params.rpc = -1;
    params.concurrency = 1;
    memset(params.buckets, 0, sizeof(params.buckets));
    params.buckets[0] = 1;
    params.buckets[1] = 10;
    params.buckets[2] = 100;
    params.nbuckets = 4;
    params.path = "/";
    params.host_hdr = 0;

    memset(&counts, 0, sizeof(counts));

    signal(SIGPIPE, SIG_IGN);

    while((ch = getopt(argc, argv, "c:l:b:n:p:r:i:u:o:H:h")) != -1){
        switch(ch){
        case 'b':
            sp = optarg;

            memset(params.buckets, 0, sizeof(params.buckets));

            for(i=0; i<MAX_BUCKETS && (ap=strsep(&sp, ",")) != nil; i++)
                params.buckets[i] = atoi(ap);

            params.nbuckets = i+1;

            if(params.buckets[0] == 0)
                panic("first bucket must be >0\n");

            for(i=1; params.buckets[i]!=0; i++){
                if(params.buckets[i]<params.buckets[i-1])
                    panic("invalid bucket specification!\n");
            }
            break;

        case 'c':
            params.concurrency = atoi(optarg);
            break;

        case 'n':
            params.count = atoi(optarg);
            break;

        case 'p':
            nprocs = atoi(optarg);
            break;

        case 'i':
            reporttv.tv_sec = atoi(optarg);
            break;

        case 'r':
            params.rpc = atoi(optarg);
            break;

        case 'l':
            params.qps = atoi(optarg);
            break;

        case 'o':
            params.tsvout = optarg;
            params.tsvoutfile = fopen(params.tsvout, "w+");

            if(params.tsvoutfile == nil)
                panic("Could not open TSV outputfile: %s", optarg);
            break;

        case 'u':
            params.path = optarg;
            break;

        case 'H':
            params.host_hdr = optarg;
            break;

        case 'h':
            usage(cmd);
            break;
        }
    }

    argc -= optind;
    argv += optind;

    host = "127.0.0.1";
    port = 80;
    switch(argc){
    case 2:
        port = atoi(argv[1]);
    case 1:
        host = argv[0];
    case 0:
        break;
    default:
        fprintf(stderr, "# Optind: %d, argc %d\n", optind, argc);
        panic("Invalid arguments: couldn't understand host and port.");
    }

    if(qps_enabled() && rpc_enabled())
      panic("Invalid arguments: -l (MAX_QPS) does not support -r (RPC).");

    http_hostname = host;
    http_port = port;

    if (params.host_hdr != 0) {
        if(snprintf(http_hosthdr, sizeof(http_hosthdr), "%s", params.host_hdr) > sizeof(http_hosthdr))
            panic("snprintf");
    } else {
        if(snprintf(http_hosthdr, sizeof(http_hosthdr), "%s:%d", host, port) > sizeof(http_hosthdr))
            panic("snprintf");
    }

    fprintf(stderr, "# Host: %s\n", http_hosthdr);

    for(i = 0; params.buckets[i] != 0; i++)
        request_timeout = params.buckets[i];

    // FIXME Should also show bucket parameters
    fprintf(stderr, "# params: -c %d -n %d -p %d -r %d -i %d -l %d -u %s %s %d\n",
        params.concurrency, params.count, nprocs, params.rpc, (int) reporttv.tv_sec, params.qps, params.path, http_hostname, http_port);

    // Convert absolute params to be relative to concurrency
    params.count /= nprocs;

    params.qps /= nprocs;
    params.qps /= params.concurrency;

    fprintf(stderr, "# \t\tconn\tconn\tconn\tconn\thttp\thttp\n");
    fprintf(stderr, "# ts\t\tsuccess\terrors\ttimeout\tcloses\tsuccess\terror\t");
    for(i=0; params.buckets[i]!=0; i++)
        fprintf(stderr, "<%d\t", params.buckets[i]);

    fprintf(stderr, ">=%d\thz\n", params.buckets[i - 1]);

    if((sockets = calloc(nprocs + 1, sizeof(int))) == nil)
        panic("malloc\n");

    sockets[nprocs] = -1;

    for(i=0; i<nprocs; i++){
        if(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0){
            perror("socketpair");
            exit(1);
        }

        sockets[i] = fds[0];

        if((pid = fork()) < 0){
            kill(0, SIGINT);
            perror("fork");
            exit(1);
        }else if(pid != 0){
            close(fds[1]);
            continue;
        }

        is_parent = 0;

        event_init();

        /* Set up output. */
        if(dup2(fds[1], STDOUT_FILENO) < 0){
            perror("dup2");
            exit(1);
        }

        close(fds[1]);

        // create a buffer for this process
        if(tsv_enabled()) {
            char *out = mal(sizeof(char) * OUTFILE_BUFFER_SIZE);
            setvbuf(params.tsvoutfile, out, _IOLBF, OUTFILE_BUFFER_SIZE);
        }

        for(i=0; i<params.concurrency; i++)
            mkrunner();

        /* event handler for reports */
        evtimer_set(&reportev, reportcb, nil);
        evtimer_add(&reportev, &reporttv);
        event_dispatch();

        break;
    }

    if(is_parent)
        parentd(nprocs, sockets);

    return(0);
}
