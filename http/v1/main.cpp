#include <iostream>
#include <httpserver.h>
#include <assert.h>
#include <cstring>
#include <unistd.h>
#include "shutdown.h"
#include "httprpc.h"
static bool AppInitServers()
{
    // RPCServer::OnStarted(&OnRPCStarted);
    // RPCServer::OnStopped(&OnRPCStopped);
    if (!InitHTTPServer())
        return false;
    // StartRPC();
    // if (!StartHTTPRPC())
    //     return false;
    // if (gArgs.GetBoolArg("-rest", DEFAULT_REST_ENABLE)) StartREST();
    StartHTTPRPC();
    StartHTTPServer();
    return true;
}
static void WaitForShutdown()
{
    while (!ShutdownRequested())
    {
        usleep(100000);
    }
    //Interrupt(node);
}
int main(int argc, char* argv[])
{
    AppInitServers();
    WaitForShutdown();
    return 0;
}