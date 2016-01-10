#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <Wincrypt.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

#define IPv4MaxPacketSize 65507

#define MAX_CLIENTS 4

typedef unsigned int uint;

const char* HostAddress = "127.0.0.1";
char* DEFAULT_PORT = "9037";


typedef enum {
    PROTOCOL_WebSocket = 1,
    PROTOCOL_NotWebSocket
} socketType;

typedef struct {
    SOCKET Socket;
    socketType Type;
} socketData;

typedef struct {
    socketData SocketData;
} ThreadContext;

SOCKET ListenSocket;
socketData ClientSockets[MAX_CLIENTS]= {0};

void
WriteFile(char* filename, unsigned char* data, long length)
{
    FILE *fp = fopen(filename, "w+b");
    fwrite(data, 1, length, fp);
    fclose(fp);
}

int
strends(char* longStr, char* endSequence)
{
    return strcmp(longStr + (strlen(longStr) - strlen(endSequence)), endSequence) == 0;
}

void
strreplace(char* str, char find, char replace)
{
    while(*str)
    {
        if (*str == find)
        {
            *str = replace;
        }
        ++str;
    }
}

HANDLE ClientListLock;

int
AddSocket(socketData newSocket)
{
    WaitForSingleObject( 
        ClientListLock,
        INFINITE);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!ClientSockets[i].Socket)
        {
            ClientSockets[i] = newSocket;
            return 1;
        }
    }

    ReleaseMutex(ClientListLock);
    
    return 0;
}

int
ClearSocket(socketData oldSocketData)
{
    WaitForSingleObject( 
        ClientListLock,
        INFINITE);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if (ClientSockets[i].Socket == oldSocketData.Socket)
        {
            ClientSockets[i].Socket = 0;
            return 1;
        }
    }

    ReleaseMutex(ClientListLock);

    return 0;
}

void
ServerStartup()
{
    WSADATA SocketsData;
    if (WSAStartup(MAKEWORD(2,2), &SocketsData) != NO_ERROR)
    {
        printf("Error at winsock startup\n");
        return;
    }

    addrinfo *result = 0;
    addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, DEFAULT_PORT, &hints, &result) != 0)
    {
        printf("get addr info failed");
        WSACleanup();
        return;
    }
    
    ListenSocket = socket(result->ai_family,
                          result->ai_socktype,
                          result->ai_protocol);

    if (ListenSocket == INVALID_SOCKET) {
        printf("failed to create socket");
        freeaddrinfo(result);
        WSACleanup();
        return;
    }
    
    if (bind(ListenSocket,
             result->ai_addr,
             (int)result->ai_addrlen)==SOCKET_ERROR)
    {
        printf("Error on server bind.\n");
        return;
    }
    if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Error listening on socket.\n");
        return;
    }

    printf("Listening on port: %s", DEFAULT_PORT);
}

void
ServerDispose()
{
    closesocket(ListenSocket);
}

SOCKET AcceptClient()
{
    sockaddr_in ClientAddress = {0};
    SOCKET ClientSocket = accept(ListenSocket,
                                 (sockaddr*)&ClientAddress,
                                 NULL);
    printf("Client connected on socket %x. Address %s:%d\n",
           ClientSocket,
           inet_ntoa(ClientAddress.sin_addr),
           ntohs(ClientAddress.sin_port));
    return ClientSocket;
}

void
ServeRequest(SOCKET ClientSocket, char* filename, char* contentType, char* contentDisposition)
{
    strreplace(filename, '/', '\\');
    
    //respond with the matching file
    char* responseFormat =
        "HTTP/1.1 200 OK\n"
        "Content-length: %d\n"
        "Content-type: %s\n"
        "%s\n";
    char responseHeader[BUFSIZ];

    if (strcmp(filename, "") == 0)
    {
        filename = "index.html";
    }
    
    printf("Sending: %s\n", filename);
    FILE* contentFile = fopen(filename, "r");
    if (contentFile == 0)
    {
        //404
        printf("404: file not found");
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA fileData = {0};
        GetFileAttributesEx(
            filename,
            GetFileExInfoStandard,
            &fileData);

        int contentLength = fileData.nFileSizeLow;
        
        char* content = (char*)malloc(sizeof(char)*contentLength);
        fread(content, 1, contentLength, contentFile);

        char contentDispositionHeader[50];
        contentDispositionHeader[0] = '\0';
        if (contentDisposition != 0)
        {
            char* contentDispositionHeaderFormat = "Content-disposition: %s\n";
            sprintf(contentDispositionHeader, contentDispositionHeaderFormat, contentDisposition);
        }
            
        sprintf(responseHeader, responseFormat, contentLength, contentType, contentDispositionHeader);
        
        int headerLength = strlen(responseHeader);

        printf(responseHeader);
        
        send(ClientSocket, responseHeader, headerLength, 0);

        
        printf("File Size: %d\n", contentLength);
        int sentAmount = 0;
        while (sentAmount < contentLength)
        {
            sentAmount += send(ClientSocket,
                               content+sentAmount,
                               contentLength, 0);
        }
        free(content);
    }   
}

void
SendHTML(SOCKET ClientSocket, char* filename)
{
    ServeRequest(ClientSocket, filename, "text/html; charset=UTF-8", 0);
}

void
SendFile(SOCKET ClientSocket, char* filename)
{
    char* dispositionFormat = "attachment; filename=%s";
    char disposition[50];
    sprintf(disposition, dispositionFormat, filename);
    ServeRequest(ClientSocket, filename, "application/octet-stream", disposition);
}

void
Respond(SOCKET ClientSocket, char* Request)
{
    int index = 0;
    char verb[20];
    char path[20];
    char version[20];

    sscanf(Request, "%s %s %s\n", &verb, &path, &version);
    printf("Verb: %s\n", verb);
    printf("Path: %s\n", path);
    printf("Version: %s\n", version);

    if (strcmp("GET", verb) == 0)
    {
        //TODO: make this check for a file extension..? like if it's just an empty path return index.html, for now forget about that       
        if (strlen(path+1) == 0)
        {
            SendHTML(ClientSocket, "index.html");
        }
        else if (strends(path, ".html"))
        {
            SendHTML(ClientSocket, path+1);
        }
        else
        {
            SendFile(ClientSocket, path+1);
        }
    }
    //free(RequestCpy);
}

int
main(int argc, char** argv)
{

    if (argc > 1)
    {
        DEFAULT_PORT = argv[1];
    }
    ClientListLock = CreateMutex(
        NULL,
        FALSE,
        NULL);
    
    HANDLE Threads[MAX_CLIENTS];
    
    ServerStartup();

    while(true)
    {
        SOCKET ClientSocket = AcceptClient();
        
        char requestbuf[BUFSIZ];
        char responsebuf[BUFSIZ];
        char encoded[BUFSIZ];

        memset(requestbuf,0,BUFSIZ);
        memset(responsebuf,0,BUFSIZ);
        int received = recv(ClientSocket, requestbuf, BUFSIZ, 0);
        printf("%s", requestbuf);
        Respond(ClientSocket, requestbuf);
        closesocket(ClientSocket);
    }
    
    ServerDispose();
    return 0;
}
