// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httprpc.h>


#include <httpserver.h>
#include <strencodings.h>

#include <memory>
#include <stdio.h>


// /** WWW-Authenticate to present with 401 Unauthorized response */
// static const char* WWW_AUTH_HEADER_DATA = "Basic realm=\"jsonrpc\"";

// /** Simple one-shot callback timer to be used by the RPC mechanism to e.g.
//  * re-lock the wallet.
//  */
// class HTTPRPCTimer : public RPCTimerBase
// {
// public:
//     HTTPRPCTimer(struct event_base* eventBase, std::function<void()>& func, int64_t millis) :
//         ev(eventBase, false, func)
//     {
//         struct timeval tv;
//         tv.tv_sec = millis/1000;
//         tv.tv_usec = (millis%1000)*1000;
//         ev.trigger(&tv);
//     }
// private:
//     HTTPEvent ev;
// };

// class HTTPRPCTimerInterface : public RPCTimerInterface
// {
// public:
//     explicit HTTPRPCTimerInterface(struct event_base* _base) : base(_base)
//     {
//     }
//     const char* Name() override
//     {
//         return "HTTP";
//     }
//     RPCTimerBase* NewTimer(std::function<void()>& func, int64_t millis) override
//     {
//         return new HTTPRPCTimer(base, func, millis);
//     }
// private:
//     struct event_base* base;
// };


// /* Pre-base64-encoded authentication token */
// static std::string strRPCUserColonPass;
// /* Stored RPC timer interface (for unregistration) */
// static std::unique_ptr<HTTPRPCTimerInterface> httpRPCTimerInterface;

// static void JSONErrorReply(HTTPRequest* req, const UniValue& objError, const UniValue& id)
// {
//     // Send error reply from json-rpc error object
//     int nStatus = HTTP_INTERNAL_SERVER_ERROR;
//     int code = find_value(objError, "code").get_int();

//     if (code == RPC_INVALID_REQUEST)
//         nStatus = HTTP_BAD_REQUEST;
//     else if (code == RPC_METHOD_NOT_FOUND)
//         nStatus = HTTP_NOT_FOUND;

//     std::string strReply = JSONRPCReply(NullUniValue, objError, id);

//     req->WriteHeader("Content-Type", "application/json");
//     req->WriteReply(nStatus, strReply);
// }



static bool HTTPReq_JSONRPC(HTTPRequest* req, const std::string &)
{
    // JSONRPC handles only POST
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "JSONRPC server handles only POST requests");
        return false;
    }

    std::string req_body = req->ReadBody();

    req->WriteReply(HTTP_OK, req_body);
 
    return true;
}

bool StartHTTPRPC()
{
    // printf("Starting HTTP RPC server\n");
    // if (!InitRPCAuthentication())
    //     return false;

    RegisterHTTPHandler("/", true, HTTPReq_JSONRPC);
    // if (g_wallet_init_interface.HasWalletSupport()) {
    //     RegisterHTTPHandler("/wallet/", false, HTTPReq_JSONRPC);
    // }
    // struct event_base* eventBase = EventBase();
    // assert(eventBase);
    // httpRPCTimerInterface = MakeUnique<HTTPRPCTimerInterface>(eventBase);
    // RPCSetTimerInterface(httpRPCTimerInterface.get());
    return true;
}

void InterruptHTTPRPC()
{
    printf("Interrupting HTTP RPC server\n");
}

void StopHTTPRPC()
{
    printf("Stopping HTTP RPC server\n");
    UnregisterHTTPHandler("/", true);
    // if (g_wallet_init_interface.HasWalletSupport()) {
    //     UnregisterHTTPHandler("/wallet/", false);
    // }
    // if (httpRPCTimerInterface) {
    //     RPCUnsetTimerInterface(httpRPCTimerInterface.get());
    //     httpRPCTimerInterface.reset();
    // }
}
