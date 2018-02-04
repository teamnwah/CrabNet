/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  Copyright (c) 2016-2018, TES3MP Team
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_HTTPConnection2==1 && _RAKNET_SUPPORT_TCPInterface==1

#include "HTTPConnection2.h"
#include "TCPInterface.h"

using namespace RakNet;

STATIC_FACTORY_DEFINITIONS(HTTPConnection2,HTTPConnection2)

HTTPConnection2::HTTPConnection2()
{
}
HTTPConnection2::~HTTPConnection2()
{

}
bool HTTPConnection2::TransmitRequest(const char* stringToTransmit, const char* host, unsigned short port, bool useSSL, int ipVersion, SystemAddress useAddress, void *userData)
{
    Request *request =new Request;
    request->host=host;
    request->chunked = false;
    if (useAddress!=UNASSIGNED_SYSTEM_ADDRESS)
    {
        request->hostEstimatedAddress=useAddress;
        if (IsConnected(useAddress)==false)
        {
            delete request;
            return false;
        }
    }
    else
    {
        if (request->hostEstimatedAddress.FromString(host, '|', ipVersion)==false)
        {
            delete request;
            return false;
        }
    }
    request->hostEstimatedAddress.SetPortHostOrder(port);
    request->port=port;
    request->stringToTransmit=stringToTransmit;
    request->contentLength=-1;
    request->contentOffset=0;
    request->useSSL=useSSL;
    request->ipVersion=ipVersion;
    request->userData=userData;

    if (IsConnected(request->hostEstimatedAddress))
    {
        std::lock_guard<RakNet::SimpleMutex> lock(sentRequestsMutex);
        if (sentRequests.Size()==0)
        {
            request->hostCompletedAddress=request->hostEstimatedAddress;
            sentRequests.Push(request);

            SendRequest(request);
        }
        else
        {
            // Request pending, push it
            std::lock_guard<RakNet::SimpleMutex> lock2(pendingRequestsMutex);
            pendingRequests.Push(request);
        }
    }
    else
    {
        {
            std::lock_guard<RakNet::SimpleMutex> lock2(pendingRequestsMutex);
            pendingRequests.Push(request);
        }

        if (ipVersion!=6)
        {
            tcpInterface->Connect(host, port, false, AF_INET);
        }
        else
        {
            #if RAKNET_SUPPORT_IPV6
                tcpInterface->Connect(host, port, false, AF_INET6);
            #else
                RakAssert("HTTPConnection2::TransmitRequest needs define  RAKNET_SUPPORT_IPV6" && 0);
            #endif
        }
    }
    return true;
}
bool HTTPConnection2::GetResponse( RakString &stringTransmitted, RakString &hostTransmitted, RakString &responseReceived, SystemAddress &hostReceived, int &contentOffset )
{
    void *userData;
    return GetResponse(stringTransmitted, hostTransmitted, responseReceived, hostReceived, contentOffset, &userData);

}
bool HTTPConnection2::GetResponse( RakString &stringTransmitted, RakString &hostTransmitted, RakString &responseReceived, SystemAddress &hostReceived, int &contentOffset, void **userData )
{
    std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
    if (completedRequests.Size() > 0)
    {
        Request *completedRequest = completedRequests[0];
        completedRequests.RemoveAtIndexFast(0);

        responseReceived = completedRequest->stringReceived;
        hostReceived = completedRequest->hostCompletedAddress;
        stringTransmitted = completedRequest->stringToTransmit;
        hostTransmitted = completedRequest->host;
        contentOffset = completedRequest->contentOffset;
        *userData = completedRequest->userData;

        delete completedRequest;
        return true;
    }
    return false;
}
bool HTTPConnection2::IsBusy() const
{
    return pendingRequests.Size()>0 || sentRequests.Size()>0;
}
bool HTTPConnection2::HasResponse() const
{
    return completedRequests.Size()>0;
}
int ReadChunkSize( char *txtStart, char **txtEnd ) 
{
//     char lengthStr[32];
//     memset(lengthStr, 0, 32);
//     memcpy(lengthStr, txtStart, txtEnd - txtStart);
    return strtoul(txtStart, txtEnd,16);
    // return atoi(lengthStr);
}
void ReadChunkBlock( size_t &currentChunkSize, size_t &bytesReadSoFar, char *txtIn, RakString &txtOut)
{
    size_t bytesToRead;
    size_t sLen;

    do
    {
        bytesToRead = currentChunkSize - bytesReadSoFar;
        sLen = strlen(txtIn);
        if (sLen < bytesToRead)
            bytesToRead = sLen;
        txtOut.AppendBytes(txtIn, bytesToRead);
        txtIn += bytesToRead;
        bytesReadSoFar += bytesToRead;
        if (*txtIn == 0)
        {
        //    currentChunkSize=0;
            return;
        }
        // char *newLine = strstr(txtIn, "\r\n");
        if (txtIn[0] && txtIn[0]=='\r' && txtIn[1] && txtIn[1]=='\n' )
            txtIn += 2; // Newline
        char *newLine;
        currentChunkSize = ReadChunkSize(txtIn, &newLine);
        RakAssert(currentChunkSize < 50000); // Sanity check
        if (currentChunkSize == 0)
            return;
        if (newLine == 0)
            return;
        bytesReadSoFar=0;
        txtIn = newLine + 2;
    } while (txtIn);
}
PluginReceiveResult HTTPConnection2::OnReceive(Packet *packet)
{
    bool locked=true;
    sentRequestsMutex.lock();
    for (unsigned i =0; i < sentRequests.Size(); i++)
    {
        Request *sentRequest = sentRequests[i];
        if (sentRequest->hostCompletedAddress==packet->systemAddress)
        {
            sentRequests.RemoveAtIndexFast(i);
            locked = false;
            sentRequestsMutex.unlock();

            const char *isFirstChunk = strstr((char*) packet->data, "Transfer-Encoding: chunked");
            if (isFirstChunk)
            {
                //printf((char*) packet->data);

                locked =false;
                sentRequestsMutex.unlock();

                sentRequest->chunked = true;
                char *chunkStrStart = strstr((char*) packet->data, "\r\n\r\n");
                RakAssert(chunkStrStart);

                chunkStrStart += 4; // strlen("\r\n\r\n");
                char *body_header; // = strstr(chunkStrStart, "\r\n");
                sentRequest->thisChunkSize = ReadChunkSize(chunkStrStart, &body_header);
                sentRequest->bytesReadForThisChunk = 0;
                sentRequest->contentOffset = 0;

                if (sentRequest->thisChunkSize == 0)
                {
                    // Done
                    {
                        std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
                        completedRequests.Push(sentRequest);
                    }

                    // If there is another command waiting for this server, send it
                    SendPendingRequestToConnectedSystem(packet->systemAddress);
                }
                else
                {

                    // char *offset = strstr((char*) packet->data+1, "2000");

                    body_header+=2;
                    ReadChunkBlock(sentRequest->thisChunkSize, sentRequest->bytesReadForThisChunk, body_header, sentRequest->stringReceived);

                    if (sentRequest->thisChunkSize==0)
                    {
                        // Done
                        {
                            std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
                            completedRequests.Push(sentRequest);
                        }

                        // If there is another command waiting for this server, send it
                        SendPendingRequestToConnectedSystem(packet->systemAddress);
                    }
                    else
                    {
                        // Not done
                        std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                        sentRequests.Push(sentRequest);
                    }
                }
            }
            else if (sentRequest->chunked)
            {
                ReadChunkBlock(sentRequest->thisChunkSize, sentRequest->bytesReadForThisChunk, (char*) packet->data, sentRequest->stringReceived);

                if (sentRequest->thisChunkSize==0)
                {
                    // Done
                    {
                        std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
                        completedRequests.Push(sentRequest);
                    }

                    // If there is another command waiting for this server, send it
                    SendPendingRequestToConnectedSystem(packet->systemAddress);
                }
                else
                {
                    // Not done
                    std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                    sentRequests.Push(sentRequest);
                }

            }
            else
            {
                sentRequest->stringReceived+=packet->data;

                if (sentRequest->contentLength==-1)
                {
                    const char *length_header = strstr(sentRequest->stringReceived.C_String(), "Content-Length: ");
                    if(length_header)
                    {
                        length_header += 16; // strlen("Content-Length: ");

                        unsigned int clLength;
                        for (clLength=0; length_header[clLength] && length_header[clLength] >= '0' && length_header[clLength] <= '9'; clLength++)
                            ;
                        if (clLength>0 && (length_header[clLength]=='\r' || length_header[clLength]=='\n'))
                        {
                            sentRequest->contentLength = RakString::ReadIntFromSubstring(length_header, 0, clLength);
                        }
                    }
                }

                // If we know the content length, find \r\n\r\n
                if (sentRequest->contentLength != -1)
                {
                    if (sentRequest->contentLength > 0)
                    {
                        const char *body_header = strstr(sentRequest->stringReceived.C_String(), "\r\n\r\n");
                        if (body_header)
                        {
                            body_header += 4; // strlen("\r\n\r\n");
                            size_t slen = strlen(body_header);
                            //RakAssert(slen <= (size_t) sentRequest->contentLength);
                            if (slen >= (size_t) sentRequest->contentLength)
                            {
                                sentRequest->contentOffset = body_header - sentRequest->stringReceived.C_String();

                                {
                                    std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                                    completedRequests.Push(sentRequest);
                                }

                                // If there is another command waiting for this server, send it
                                SendPendingRequestToConnectedSystem(packet->systemAddress);
                            }
                            else
                            {
                                std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                                sentRequests.Push(sentRequest);
                            }
                        }

                        else
                        {
                            std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                            sentRequests.Push(sentRequest);
                        }
                    }
                    else
                    {
                        sentRequest->contentOffset = -1;
                        {
                            std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
                            completedRequests.Push(sentRequest);
                        }

                        // If there is another command waiting for this server, send it
                        SendPendingRequestToConnectedSystem(packet->systemAddress);
                    }
                }
                else
                {
                    const char *firstNewlineSet = strstr(sentRequest->stringReceived.C_String(), "\r\n\r\n");
                    if (firstNewlineSet != nullptr)
                    {
                        int offset = firstNewlineSet - sentRequest->stringReceived.C_String();
                        if (sentRequest->stringReceived.C_String()[offset+4]==0)
                            sentRequest->contentOffset=-1;
                        else
                            sentRequest->contentOffset=offset+4;

                        {
                            std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
                            completedRequests.Push(sentRequest);
                        }

                        // If there is another command waiting for this server, send it
                        SendPendingRequestToConnectedSystem(packet->systemAddress);
                    }
                    else
                    {
                        std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                        sentRequests.Push(sentRequest);
                    }
                }
            }


            break;
        }
    }

    if (locked)
        sentRequestsMutex.unlock();

    return RR_CONTINUE_PROCESSING;
}

void HTTPConnection2::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
    (void) rakNetGUID;
    (void) isIncoming; // unknown

    SendPendingRequestToConnectedSystem(systemAddress);
}
void HTTPConnection2::SendPendingRequestToConnectedSystem(SystemAddress sa)
{
    if (sa==UNASSIGNED_SYSTEM_ADDRESS)
        return;

    unsigned int requestsSent=0;

    // Search through requests to find a match for this instance of TCPInterface and SystemAddress
    pendingRequestsMutex.lock();
    for (unsigned i = 0; i < pendingRequests.Size();)
    {
        Request *request = pendingRequests[i];
        if (request->hostEstimatedAddress==sa)
        {
            pendingRequests.RemoveAtIndex(i);
            // Send this request
            request->hostCompletedAddress=sa;

            {
                std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                sentRequests.Push(request);
            }

            pendingRequestsMutex.unlock();

#if OPEN_SSL_CLIENT_SUPPORT==1
            if (request->useSSL)
                tcpInterface->StartSSLClient(sa);
#endif

            SendRequest(request);
            requestsSent++;
            pendingRequestsMutex.lock();
            break;
        }
        else
            i++;
    }
    pendingRequestsMutex.unlock();

    if (requestsSent == 0)
    {
        pendingRequestsMutex.lock();
        if (pendingRequests.Size() > 0)
        {
            // Just assign
            Request *request = pendingRequests[0];
            pendingRequests.RemoveAtIndex(0);

            request->hostCompletedAddress = sa;

            {
                std::lock_guard<RakNet::SimpleMutex> lock2(sentRequestsMutex);
                sentRequests.Push(request);
            }

            pendingRequestsMutex.unlock();

            // Send
#if OPEN_SSL_CLIENT_SUPPORT==1
            if (request->useSSL)
                tcpInterface->StartSSLClient(sa);
#endif


            SendRequest(request);
        }
        else
            pendingRequestsMutex.unlock();
    }
}
void HTTPConnection2::RemovePendingRequest(SystemAddress sa)
{
    std::lock_guard<RakNet::SimpleMutex> lock2(pendingRequestsMutex);
    for (unsigned i = 0; i < pendingRequests.Size(); i++)
    {
        Request *request = pendingRequests[i];
        if (request->hostEstimatedAddress == sa)
        {
            pendingRequests.RemoveAtIndex(i);
            delete request;
        }
        else
            i++;
    }
}

void HTTPConnection2::SendNextPendingRequest()
{
    // Send a pending request
    pendingRequestsMutex.lock();
    if (pendingRequests.Size()>0)
    {
        Request *pendingRequest = pendingRequests.Peek();
        pendingRequestsMutex.unlock();

        if (pendingRequest->ipVersion!=6)
        {
            tcpInterface->Connect(pendingRequest->host.C_String(), pendingRequest->port, false, AF_INET);
        }
        else
        {
#if RAKNET_SUPPORT_IPV6
            tcpInterface->Connect(pendingRequest->host.C_String(), pendingRequest->port, false, AF_INET6);
#else
            RakAssert("HTTPConnection2::TransmitRequest needs define  RAKNET_SUPPORT_IPV6" && 0);
#endif
        }
    }
    else
    {
        pendingRequestsMutex.unlock();
    }
}

void HTTPConnection2::OnFailedConnectionAttempt(Packet *packet, PI2_FailedConnectionAttemptReason failedConnectionAttemptReason)
{
    (void) failedConnectionAttemptReason;
    if (packet->systemAddress==UNASSIGNED_SYSTEM_ADDRESS)
        return;

    RemovePendingRequest(packet->systemAddress);

    SendNextPendingRequest();
}
void HTTPConnection2::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void) lostConnectionReason;
    (void) rakNetGUID;

    if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
        return;

    // Update sent requests to completed requests
    sentRequestsMutex.lock();
    for (unsigned i = 0; i < sentRequests.Size();)
    {
        if (sentRequests[i]->hostCompletedAddress == systemAddress)
        {
            Request *sentRequest = sentRequests[i];
            if (!sentRequest->chunked && !sentRequest->stringReceived.IsEmpty())
            {
                if (strstr(sentRequest->stringReceived.C_String(), "Content-Length: "))
                {
                    char *body_header = strstr((char*) sentRequest->stringReceived.C_String(), "\r\n\r\n");
                    if (body_header)
                    {
                        body_header += 4; // strlen("\r\n\r\n");
                        sentRequest->contentOffset = body_header - sentRequest->stringReceived.C_String();
                    }
                    else
                    {
                        sentRequest->contentOffset = 0;
                    }

                }
                else
                {
                    sentRequest->contentOffset = 0;
                }
            }

            std::lock_guard<RakNet::SimpleMutex> lock2(completedRequestsMutex);
            completedRequests.Push(sentRequests[i]);

            sentRequests.RemoveAtIndexFast(i);
        }
        else
            i++;
    }
    sentRequestsMutex.unlock();

    SendNextPendingRequest();
}

bool HTTPConnection2::IsConnected(SystemAddress sa)
{
    SystemAddress remoteSystems[64];
    unsigned short numberOfSystems = 64;
    tcpInterface->GetConnectionList(remoteSystems, &numberOfSystems);
    for (unsigned i = 0; i < numberOfSystems; i++)
    {
        if (remoteSystems[i] == sa)
            return true;
    }
    return false;
}
void HTTPConnection2::SendRequest(Request *request)
{
    tcpInterface->Send(request->stringToTransmit.C_String(), (unsigned int) request->stringToTransmit.GetLength(), request->hostCompletedAddress, false);
}

#endif // #if _RAKNET_SUPPORT_HTTPConnection2==1 && _RAKNET_SUPPORT_TCPInterface==1
