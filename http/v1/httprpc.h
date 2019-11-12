// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HTTPRPC_H
#define BITCOIN_HTTPRPC_H

enum HTTPStatusCode
{
    HTTP_OK                    = 200,
    HTTP_BAD_REQUEST           = 400,
    HTTP_UNAUTHORIZED          = 401,
    HTTP_FORBIDDEN             = 403,
    HTTP_NOT_FOUND             = 404,
    HTTP_BAD_METHOD            = 405,
    HTTP_INTERNAL_SERVER_ERROR = 500,
    HTTP_SERVICE_UNAVAILABLE   = 503,
};
/** Start HTTP RPC subsystem.
 * Precondition; HTTP and RPC has been started.
 */
bool StartHTTPRPC();
/** Interrupt HTTP RPC subsystem.
 */
void InterruptHTTPRPC();
/** Stop HTTP RPC subsystem.
 * Precondition; HTTP and RPC has been stopped.
 */
void StopHTTPRPC();

#endif
