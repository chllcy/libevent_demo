// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httpserver.h>

//#include <chainparamsbase.h>
#include <compat.h>
#include <threadnames.h>
//#include <util/system.h>
//#include <strencodings.h>
#include <netbase.h>
#include <math.h>
//#include <rpc/protocol.h> // For HTTP status codes
#include <shutdown.h>
#include <sync.h>
//#include <ui_interface.h>

#include <deque>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <list>

#include <sys/types.h>
#include <sys/stat.h>

#include <event2/thread.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#include <support/events.h>

#ifdef EVENT__HAVE_NETINET_IN_H
#include <netinet/in.h>
#ifdef _XOPEN_SOURCE_EXTENDED
#include <arpa/inet.h>
#endif
#endif

/** Maximum size of http request (request line + headers) */
static const size_t MAX_HEADERS_SIZE = 8192;
static std::vector<std::thread> threadHTTP;
static std::array<size_t, DEFAULT_HTTP_THREADS> threadIndex;
static std::vector<std::thread> g_thread_http_workers;

/** HTTP request work item */
class HTTPWorkItem final : public HTTPClosure
{
public:
    HTTPWorkItem(std::unique_ptr<HTTPRequest> _req, const std::string &_path, const HTTPRequestHandler& _func):
        req(std::move(_req)), path(_path), func(_func)
    {
    }
    void operator()() override
    {
        func(req.get(), path);
    }

    std::unique_ptr<HTTPRequest> req;

private:
    std::string path;
    HTTPRequestHandler func;
};

/** Simple work queue for distributing work over multiple threads.
 * Work items are simply callable objects.
 */
template <typename WorkItem>
class WorkQueue
{
private:
    /** Mutex protects entire object */
    Mutex cs;
    std::condition_variable cond;
    std::deque<std::unique_ptr<WorkItem>> queue;
    bool running;
    size_t maxDepth;

public:
    explicit WorkQueue(size_t _maxDepth) : running(true),
                                 maxDepth(_maxDepth)
    {
    }
    /** Precondition: worker threads have all stopped (they have been joined).
     */
    ~WorkQueue()
    {
    }
    /** Enqueue a work item */
    bool Enqueue(WorkItem* item)
    {
        LOCK(cs);
        if (queue.size() >= maxDepth) {
            return false;
        }
        queue.emplace_back(std::unique_ptr<WorkItem>(item));
        cond.notify_one();
        return true;
    }
    /** Thread function */
    void Run()
    {
        while (true) {
            std::unique_ptr<WorkItem> i;
            {
                WAIT_LOCK(cs, lock);
                while (running && queue.empty())
                    cond.wait(lock);
                if (!running)
                    break;
                i = std::move(queue.front());
                queue.pop_front();
            }
            (*i)();
        }
    }
    /** Interrupt and exit loops */
    void Interrupt()
    {
        LOCK(cs);
        running = false;
        cond.notify_all();
    }
};

struct HTTPPathHandler
{
    HTTPPathHandler(std::string _prefix, bool _exactMatch, HTTPRequestHandler _handler):
        prefix(_prefix), exactMatch(_exactMatch), handler(_handler)
    {
    }
    std::string prefix;
    bool exactMatch;
    HTTPRequestHandler handler;
};

/** HTTP module state */

//! libevent event loop
static std::vector<struct event_base*> eventBase;
//! HTTP server
static std::vector<struct evhttp*> eventHTTP;
//! List of subnets to allow RPC connections from
// static std::vector<CSubNet> rpc_allow_subnets;
//! Work queue for handling longer requests off the event loop thread
static WorkQueue<HTTPClosure>* workQueue = nullptr;
//! Handlers for (sub)paths
static std::vector<HTTPPathHandler> pathHandlers;
//! Bound listening sockets
static std::vector<std::vector<evhttp_bound_socket *>> boundSockets;

// /** Check if a network address is allowed to access the HTTP server */
// static bool ClientAllowed(const CNetAddr& netaddr)
// {
//     if (!netaddr.IsValid())
//         return false;
//     for(const CSubNet& subnet : rpc_allow_subnets)
//         if (subnet.Match(netaddr))
//             return true;
//     return false;
// }

// /** Initialize ACL list for HTTP server */
// static bool InitHTTPAllowList()
// {
//     rpc_allow_subnets.clear();
//     CNetAddr localv4;
//     CNetAddr localv6;
//     LookupHost("127.0.0.1", localv4, false);
//     LookupHost("::1", localv6, false);
//     rpc_allow_subnets.push_back(CSubNet(localv4, 8));      // always allow IPv4 local subnet
//     rpc_allow_subnets.push_back(CSubNet(localv6));         // always allow IPv6 localhost
//     for (const std::string& strAllow : gArgs.GetArgs("-rpcallowip")) {
//         CSubNet subnet;
//         LookupSubNet(strAllow.c_str(), subnet);
//         if (!subnet.IsValid()) {
//             uiInterface.ThreadSafeMessageBox(
//                 strprintf("Invalid -rpcallowip subnet specification: %s. Valid are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24).", strAllow),
//                 "", CClientUIInterface::MSG_ERROR);
//             return false;
//         }
//         rpc_allow_subnets.push_back(subnet);
//     }
//     std::string strAllowed;
//     for (const CSubNet& subnet : rpc_allow_subnets)
//         strAllowed += subnet.ToString() + " ";
//     printf("Allowing HTTP connections from: %s\n", strAllowed);
//     return true;
// }

/** HTTP request method as string - use for logging only */
static std::string RequestMethodString(HTTPRequest::RequestMethod m)
{
    switch (m) {
    case HTTPRequest::GET:
        return "GET";
        break;
    case HTTPRequest::POST:
        return "POST";
        break;
    case HTTPRequest::HEAD:
        return "HEAD";
        break;
    case HTTPRequest::PUT:
        return "PUT";
        break;
    default:
        return "unknown";
    }
}

/** HTTP request callback */
static void http_request_cb(struct evhttp_request* req, void* arg)
{
    size_t thread_index = *(size_t*)arg;
    // Disable reading to work around a libevent bug, fixed in 2.2.0.
    if (event_get_version_number() >= 0x02010600 && event_get_version_number() < 0x02020001) {
        evhttp_connection* conn = evhttp_request_get_connection(req);
        if (conn) {
            bufferevent* bev = evhttp_connection_get_bufferevent(conn);
            if (bev) {
                bufferevent_disable(bev, EV_READ);
            }
        }
    }
    std::unique_ptr<HTTPRequest> hreq(new HTTPRequest(req));

    // // Early address-based allow check
    // if (!ClientAllowed(hreq->GetPeer())) {
    //     printf("HTTP request from %s rejected: Client network is not allowed RPC access\n",
    //              hreq->GetPeer().ToString());
    //     hreq->WriteReply(HTTP_FORBIDDEN);
    //     return;
    // }

    // Early reject unknown HTTP methods
    if (hreq->GetRequestMethod() == HTTPRequest::UNKNOWN) {
        printf("HTTP request from %s rejected: Unknown HTTP request method\n",
                 hreq->GetPeer().ToString().c_str());
        hreq->WriteReply(HTTP_BADMETHOD, thread_index);
        return;
    }

    std::string req_body = hreq->ReadBody();

    hreq->WriteReply(HTTP_OK, thread_index, req_body);
 
    return true;
}

/** Callback to reject HTTP requests after shutdown. */
static void http_reject_request_cb(struct evhttp_request* req, void*)
{
    printf("Rejecting request while shutting down\n");
    evhttp_send_error(req, HTTP_SERVUNAVAIL, nullptr);
}

/** Event dispatcher thread */
static bool ThreadHTTP(struct event_base* base, size_t thread_index)
{
    util::ThreadRename("http" + std::to_string(thread_index));
    printf("Entering http event loop\n");
    event_base_dispatch(base);
    // Event loop will be interrupted by InterruptHTTPServer()
    printf("Exited http event loop\n");
    return event_base_got_break(base) == 0;
}

/** Bind HTTP server to specified addresses */
static bool HTTPBindAddresses(struct evhttp* http, size_t thread_index)
{
    int http_port = 8088;
    std::vector<std::pair<std::string, uint16_t> > endpoints;

    // // Determine what addresses to bind to
    // if (!(gArgs.IsArgSet("-rpcallowip") && gArgs.IsArgSet("-rpcbind"))) { // Default to loopback if not allowing external IPs
    //     endpoints.push_back(std::make_pair("::1", http_port));
    //     endpoints.push_back(std::make_pair("127.0.0.1", http_port));
    //     if (gArgs.IsArgSet("-rpcallowip")) {
    //         printf("WARNING: option -rpcallowip was specified without -rpcbind; this doesn't usually make sense\n");
    //     }
    //     if (gArgs.IsArgSet("-rpcbind")) {
    //         printf("WARNING: option -rpcbind was ignored because -rpcallowip was not specified, refusing to allow everyone to connect\n");
    //     }
    // } else if (gArgs.IsArgSet("-rpcbind")) { // Specific bind address
    //     for (const std::string& strRPCBind : gArgs.GetArgs("-rpcbind")) {
    //         int port = http_port;
    //         std::string host;
    //         SplitHostPort(strRPCBind, port, host);
    //         endpoints.push_back(std::make_pair(host, port));
    //     }
    // }
    //endpoints.push_back(std::make_pair("::", http_port));
    endpoints.push_back(std::make_pair("0.0.0.0", http_port));

    // Bind addresses
    std::vector<evhttp_bound_socket *> boundSockets_local;
    for (std::vector<std::pair<std::string, uint16_t> >::iterator i = endpoints.begin(); i != endpoints.end(); ++i) {
        printf("Binding RPC on address %s port %i\n", i->first.c_str(), i->second);
        evhttp_bound_socket *bind_handle = evhttp_bind_socket_with_handle(http, i->first.empty() ? nullptr : i->first.c_str(), i->second);
        if (bind_handle) {
            // CNetAddr addr;
            // if (i->first.empty() || (LookupHost(i->first.c_str(), addr, false) && addr.IsBindAny())) {
            //     printf("WARNING: the RPC server is not safe to expose to untrusted networks such as the public internet\n");
            // }
            boundSockets_local.push_back(bind_handle);
            
        } else {
            printf("Binding RPC on address %s port %i failed.\n", i->first.c_str(), i->second);
        }
    }
    boundSockets.push_back(boundSockets_local);
    return !boundSockets[thread_index].empty();
}

/** Simple wrapper to set thread name and run work queue */
static void HTTPWorkQueueRun(WorkQueue<HTTPClosure>* queue, int worker_num)
{
    util::ThreadRename(("httpworker." + std::to_string(worker_num)));
    queue->Run();
}

/** libevent event log callback */
static void libevent_log_cb(int severity, const char *msg)
{
#ifndef EVENT_LOG_WARN
// EVENT_LOG_WARN was added in 2.0.19; but before then _EVENT_LOG_WARN existed.
# define EVENT_LOG_WARN _EVENT_LOG_WARN
#endif
    if (severity >= EVENT_LOG_WARN) // Log warn messages and higher without debug category
        printf("libevent: %s\n", msg);
    else
        printf("libevent: %s\n", msg);
}

bool InitHTTPServer()
{
    // if (!InitHTTPAllowList())
    //     return false;
    for (size_t thread_index = 0; thread_index < DEFAULT_HTTP_THREADS; ++thread_index) {
        threadIndex[thread_index] = (thread_index);
        // Redirect libevent's logging to our own log
        // event_set_log_callback(&libevent_log_cb);
        // Update libevent's log handling. Returns false if our version of
        // libevent doesn't support debug logging, in which case we should
        // clear the BCLog::LIBEVENT flag.
        if (!UpdateHTTPServerLogging(false)) {
            //void*(0);
            //LogInstance().DisableCategory(BCLog::LIBEVENT);
        }

    #ifdef WIN32
        evthread_use_windows_threads();
    #else
        evthread_use_pthreads();
    #endif

        raii_event_base base_ctr = obtain_event_base();

        /* Create a new evhttp object to handle requests. */
        raii_evhttp http_ctr = obtain_evhttp(base_ctr.get());
        struct evhttp* http = http_ctr.get();
        if (!http) {
            printf("couldn't create evhttp. Exiting.\n");
            return false;
        }

        evhttp_set_timeout(http, DEFAULT_HTTP_SERVER_TIMEOUT);
        evhttp_set_max_headers_size(http, MAX_HEADERS_SIZE);
        static const unsigned int MAX_SIZE = 0x02000000;
        evhttp_set_max_body_size(http, MAX_SIZE);
        evhttp_set_gencb(http, http_request_cb, &(threadIndex[thread_index]));

        if (!HTTPBindAddresses(http, thread_index)) {
            printf("Unable to bind any endpoint for RPC server\n");
            return false;
        }

        eventBase.push_back(base_ctr.release());
        eventHTTP.push_back(http_ctr.release());
        threadHTTP.emplace_back(std::thread(ThreadHTTP, eventBase[thread_index], thread_index));
    }
    return true;
}

bool UpdateHTTPServerLogging(bool enable) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010100
    if (enable) {
        event_enable_debug_logging(EVENT_DBG_ALL);
    } else {
        event_enable_debug_logging(EVENT_DBG_NONE);
    }
    return true;
#else
    // Can't update libevent logging if version < 02010100
    return false;
#endif
}



void StartHTTPServer()
{
}

void InterruptHTTPServer()
{
    printf("Interrupting HTTP server\n");
    for(size_t i = 0; i < eventHTTP.size(); ++i) {
        if (eventHTTP[i]) {
            // Reject requests on current connections
            evhttp_set_gencb(eventHTTP[i], http_reject_request_cb, nullptr);
        }
    }

    if (workQueue)
        workQueue->Interrupt();
}

void StopHTTPServer()
{
    printf("Stopping HTTP server\n");
    if (workQueue) {
        printf("Waiting for HTTP worker threads to exit\n");
        for (auto& thread: g_thread_http_workers) {
            thread.join();
        }
        g_thread_http_workers.clear();
        delete workQueue;
        workQueue = nullptr;
    }
    // Unlisten sockets, these are what make the event loop running, which means
    // that after this and all connections are closed the event loop will quit.
    for(size_t i = 0; i < boundSockets.size(); ++i) {
        for (evhttp_bound_socket *socket : boundSockets[i]) {
            evhttp_del_accept_socket(eventHTTP[i], socket);
        }
    }

    boundSockets.clear();
    for (size_t i = 0; i < eventBase.size(); ++i) {
        if (eventBase[i]) {
            printf("Waiting for HTTP event thread to exit\n");
            threadHTTP[i].join();
        }
        if (eventHTTP[i]) {
            evhttp_free(eventHTTP[i]);
            eventHTTP[i] = nullptr;
        }
        if (eventBase[i]) {
            event_base_free(eventBase[i]);
            eventBase[i] = nullptr;
        }
    }


    printf("Stopped HTTP server\n");
}

static void httpevent_callback_fn(evutil_socket_t, short, void* data)
{
    // Static handler: simply call inner handler
    HTTPEvent *self = static_cast<HTTPEvent*>(data);
    self->handler();
    if (self->deleteWhenTriggered)
        delete self;
}

HTTPEvent::HTTPEvent(struct event_base* base, bool _deleteWhenTriggered, const std::function<void()>& _handler):
    deleteWhenTriggered(_deleteWhenTriggered), handler(_handler)
{
    ev = event_new(base, -1, 0, httpevent_callback_fn, this);
    assert(ev);
}
HTTPEvent::~HTTPEvent()
{
    event_free(ev);
}
void HTTPEvent::trigger(struct timeval* tv)
{
    if (tv == nullptr)
        event_active(ev, 0, 0); // immediately trigger event in main thread
    else
        evtimer_add(ev, tv); // trigger after timeval passed
}
HTTPRequest::HTTPRequest(struct evhttp_request* _req) : req(_req),
                                                       replySent(false)
{
}
HTTPRequest::~HTTPRequest()
{
    if (!replySent) {
        // Keep track of whether reply was sent to avoid request leaks
        printf("%s: Unhandled request\n", __func__);
        WriteReply(HTTP_INTERNAL, "Unhandled request");
    }
    // evhttpd cleans up the request, as long as a reply was sent.
}

std::pair<bool, std::string> HTTPRequest::GetHeader(const std::string& hdr) const
{
    const struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    assert(headers);
    const char* val = evhttp_find_header(headers, hdr.c_str());
    if (val)
        return std::make_pair(true, val);
    else
        return std::make_pair(false, "");
}

std::string HTTPRequest::ReadBody()
{
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (!buf)
        return "";
    size_t size = evbuffer_get_length(buf);
    /** Trivial implementation: if this is ever a performance bottleneck,
     * internal copying can be avoided in multi-segment buffers by using
     * evbuffer_peek and an awkward loop. Though in that case, it'd be even
     * better to not copy into an intermediate string but use a stream
     * abstraction to consume the evbuffer on the fly in the parsing algorithm.
     */
    const char* data = (const char*)evbuffer_pullup(buf, size);
    if (!data) // returns nullptr in case of empty buffer
        return "";
    std::string rv(data, size);
    evbuffer_drain(buf, size);
    return rv;
}

void HTTPRequest::WriteHeader(const std::string& hdr, const std::string& value)
{
    struct evkeyvalq* headers = evhttp_request_get_output_headers(req);
    assert(headers);
    evhttp_add_header(headers, hdr.c_str(), value.c_str());
}

/** Closure sent to main thread to request a reply to be sent to
 * a HTTP request.
 * Replies must be sent in the main loop in the main http thread,
 * this cannot be done from worker threads.
 */
void HTTPRequest::WriteReply(int nStatus, const size_t thread_index, const std::string& strReply)
{
    assert(!replySent && req);
    if (ShutdownRequested()) {
        WriteHeader("Connection", "close");
    }
    // Send event to main http thread to send reply message
    struct evbuffer* evb = evhttp_request_get_output_buffer(req);
    assert(evb);
    evbuffer_add(evb, strReply.data(), strReply.size());
    auto req_copy = req;
    HTTPEvent* ev = new HTTPEvent(eventBase[thread_index], true, [req_copy, nStatus]{
        evhttp_send_reply(req_copy, nStatus, nullptr, nullptr);
        // Re-enable reading from the socket. This is the second part of the libevent
        // workaround above.
        if (event_get_version_number() >= 0x02010600 && event_get_version_number() < 0x02020001) {
            evhttp_connection* conn = evhttp_request_get_connection(req_copy);
            if (conn) {
                bufferevent* bev = evhttp_connection_get_bufferevent(conn);
                if (bev) {
                    bufferevent_enable(bev, EV_READ | EV_WRITE);
                }
            }
        }
    });
    ev->trigger(nullptr);
    replySent = true;
    req = nullptr; // transferred back to main thread
}

CService HTTPRequest::GetPeer() const
{
    evhttp_connection* con = evhttp_request_get_connection(req);
    CService peer;
    if (con) {
        // evhttp retains ownership over returned address string
        const char* address = "";
        uint16_t port = 0;
        evhttp_connection_get_peer(con, (char**)&address, &port);
        peer = LookupNumeric(address, port);
    }
    return peer;
}

std::string HTTPRequest::GetURI() const
{
    return evhttp_request_get_uri(req);
}

HTTPRequest::RequestMethod HTTPRequest::GetRequestMethod() const
{
    switch (evhttp_request_get_command(req)) {
    case EVHTTP_REQ_GET:
        return GET;
        break;
    case EVHTTP_REQ_POST:
        return POST;
        break;
    case EVHTTP_REQ_HEAD:
        return HEAD;
        break;
    case EVHTTP_REQ_PUT:
        return PUT;
        break;
    default:
        return UNKNOWN;
        break;
    }
}

void RegisterHTTPHandler(const std::string &prefix, bool exactMatch, const HTTPRequestHandler &handler)
{
    printf("Registering HTTP handler for %s (exactmatch %d)\n", prefix.c_str(), exactMatch);
    pathHandlers.push_back(HTTPPathHandler(prefix, exactMatch, handler));
}

void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch)
{
    std::vector<HTTPPathHandler>::iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::iterator iend = pathHandlers.end();
    for (; i != iend; ++i)
        if (i->prefix == prefix && i->exactMatch == exactMatch)
            break;
    if (i != iend)
    {
        printf("Unregistering HTTP handler for %s (exactmatch %d)\n", prefix.c_str(), exactMatch);
        pathHandlers.erase(i);
    }
}
